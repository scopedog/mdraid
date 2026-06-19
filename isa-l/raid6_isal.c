// SPDX-License-Identifier: GPL-2.0-only
/*
 * raid6_isal - replace raid6_call.gen_syndrome and raid6_call.xor_syndrome
 * with ISA-L GFNI implementations when the host CPU supports GFNI.
 *
 * The in-tree lib/raid6 picks the fastest *_gen_syndrome at boot
 * (avx2/avx512/etc.). raid6_call is an EXPORT'd writable global;
 * we save its current contents at module init, install GFNI
 * gen_syndrome / xor_syndrome, and restore on exit.
 *
 * gen_syndrome: P (XOR) and Q (Reed-Solomon, data[d] coefficient
 * alpha^d) are computed via gf_2vect_dot_prod_avx2_gfni — both rows
 * in one inner-loop pass, single kernel_fpu_begin/end window.
 *
 * xor_syndrome: incremental P/Q update over data slots [start..stop]
 * for the RMW write path. Uses gf_2vect_mad_avx2_gfni once per disk
 * (one call updates both P and Q):
 *   P_new = P_old XOR sum data[z]
 *   Q_new = Q_old XOR sum alpha^z * data[z]   for z in [start..stop]
 * Caller pre-positions old P / old Q in ptrs[disks-2] / ptrs[disks-1]
 * and we XOR-accumulate.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/raid/pq.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <asm/fpu/api.h>

#include "isa-l_ec.h"

#define ISAL_MAX_DATA  256

extern void
gf_2vect_dot_prod_avx2_gfni(int len, int vec, unsigned char *gftbls,
                            unsigned char **src, unsigned char **dest);

extern void
gf_2vect_mad_avx2_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
                       unsigned char *src, unsigned char **dest);

extern void
gf_2vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

extern void
gf_2vect_mad_avx512_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
                         unsigned char *src, unsigned char **dest);

/* AVX-512 GFNI is preferred when the host supports it (Sapphire Rapids,
 * Ice Lake-SP, etc.).  Falls back to the AVX2 2vect kernels otherwise.
 * The probe (isal_have_avx512_gfni) lives in gfni_glue.c. */
static bool use_avx512;

static int bench;
module_param(bench, int, 0444);
MODULE_PARM_DESC(bench,
		 "if non-zero, micro-benchmark GFNI vs in-tree recov at load");

int  isal_install_raid6_call(void);
void isal_uninstall_raid6_call(void);

static unsigned char alpha_pow(unsigned int e)
{
	unsigned char r = 1;

	while (e--)
		r = gf_mul(r, 0x02);
	return r;
}

static void isal_gen_syndrome(int disks, size_t bytes, void **ptrs)
{
	int k = disks - 2;
	unsigned char coefs[2 * 32];	/* small fixed; raid6 with k>32 is rare */
	unsigned char *tbls;
	unsigned char *dests[2];
	int d;

	if (k <= 0 || k > (int)(sizeof(coefs) / 2)) {
		WARN_ONCE(1, "raid6_isal: k=%d out of range (1..%zu)\n",
			  k, sizeof(coefs) / 2);
		return;
	}

	tbls = kmalloc((size_t)2 * k * 32, GFP_NOIO);
	if (!tbls) {
		WARN_ONCE(1, "raid6_isal: kmalloc failed for k=%d\n", k);
		return;
	}

	for (d = 0; d < k; d++) {
		coefs[d]     = 0x01;		/* P row: XOR */
		coefs[k + d] = alpha_pow(d);	/* Q row: data[d] gets alpha^d */
	}
	ec_init_tables_gfni(k, 2, coefs, tbls);

	dests[0] = (unsigned char *)ptrs[k];     /* P */
	dests[1] = (unsigned char *)ptrs[k + 1]; /* Q */

	kernel_fpu_begin();
	if (use_avx512)
		gf_2vect_dot_prod_avx512_gfni((int)bytes, k, tbls,
					      (unsigned char **)ptrs, dests);
	else
		gf_2vect_dot_prod_avx2_gfni((int)bytes, k, tbls,
					    (unsigned char **)ptrs, dests);
	kernel_fpu_end();
	kfree(tbls);
}

/*
 * isal_xor_syndrome: incremental P/Q update for data slots [start..stop].
 * Semantics match the in-tree raid6_*_xor_syndrome:
 *   P_new = P_old XOR (data[start] XOR data[start+1] ... XOR data[stop])
 *   Q_new = Q_old XOR (alpha^start * data[start] XOR ... XOR alpha^stop * data[stop])
 * Caller passes existing P at ptrs[disks-2] and existing Q at ptrs[disks-1];
 * we update them in-place via gf_vect_mad_avx2_gfni (XOR-accumulate).
 */
static void isal_xor_syndrome(int disks, int start, int stop, size_t bytes,
			      void **ptrs)
{
	int k = disks - 2;
	unsigned char coefs[2 * 32];	/* row 0: P (all 1), row 1: Q (alpha^d) */
	unsigned char *tbls;
	unsigned char *dests[2];
	int z;

	if (k <= 0 || k > (int)(sizeof(coefs) / 2) ||
	    start < 0 || stop >= k || start > stop) {
		WARN_ONCE(1, "raid6_isal: xor_syndrome k=%d start=%d stop=%d\n",
			  k, start, stop);
		return;
	}

	tbls = kmalloc((size_t)2 * k * 32, GFP_NOIO);
	if (!tbls) {
		WARN_ONCE(1, "raid6_isal: xor_syndrome kmalloc failed for k=%d\n",
			  k);
		return;
	}

	for (z = 0; z < k; z++) {
		coefs[z]     = 0x01;		/* row 0 (P) */
		coefs[k + z] = alpha_pow(z);	/* row 1 (Q) */
	}
	ec_init_tables_gfni(k, 2, coefs, tbls);

	dests[0] = (unsigned char *)ptrs[k];     /* P */
	dests[1] = (unsigned char *)ptrs[k + 1]; /* Q */

	kernel_fpu_begin();
	for (z = start; z <= stop; z++) {
		/* P_new ^= 1 * data[z]; Q_new ^= alpha^z * data[z]
		 * Both rows in one call (vec_i = z indexes within each row). */
		if (use_avx512)
			gf_2vect_mad_avx512_gfni((int)bytes, k, z, tbls,
						 (unsigned char *)ptrs[z],
						 dests);
		else
			gf_2vect_mad_avx2_gfni((int)bytes, k, z, tbls,
					       (unsigned char *)ptrs[z],
					       dests);
	}
	kernel_fpu_end();

	kfree(tbls);
}

/*
 * ---- GFNI decode (recovery) override -------------------------------------
 *
 * The in-tree optimized recov (recov_avx2/ssse3) computes the partial
 * syndrome via raid6_call.gen_syndrome (already GFNI once we're installed)
 * and then does a PSHUFB-table fixup (raid6_vgfmul + vpshufb) to solve for
 * the lost symbols. We replace the whole thing with the idiomatic ISA-L
 * decode: invert the k x k (survivor -> original-data) matrix, fold the two
 * lost output rows through the inverse to obtain a 2 x k decode-coefficient
 * matrix, and reconstruct both lost symbols in one GFNI gf_2vect_dot_prod
 * pass. No PSHUFB, no scalar per-byte fixup.
 *
 * k beyond ISAL_RECOV_MAX_K (rare for RAID6) or any allocation/singular
 * failure falls back to the saved in-tree recov, so we never corrupt.
 */
#define ISAL_RECOV_MAX_K  64

static void (*saved_2data_recov)(int disks, size_t bytes, int faila, int failb,
				 void **ptrs);
static void (*saved_datap_recov)(int disks, size_t bytes, int faila,
				 void **ptrs);

/*
 * ---- Per-CPU decode-table cache ------------------------------------------
 *
 * md drives recovery one page at a time, but the decode tables depend only on
 * the failure pattern (mode, k, faila, failb), which is constant for the whole
 * degraded period. Rebuilding the k x k inverse + GFNI tables on every page
 * (as the plain path does) costs more than the in-tree PSHUFB fixup. So we
 * memoise the built tables per CPU: a hit runs just the GFNI dot product.
 *
 * Lock-free by construction: the cache is touched only under preempt_disable,
 * so each CPU owns its entry exclusively (recovery runs in process context,
 * never nested on one CPU). enc/inv are per-CPU scratch so a rebuild needs no
 * allocation and is safe with preemption off. k beyond ISAL_CACHE_MAX_K falls
 * through to the plain (kmalloc) path.
 */
#define ISAL_CACHE_MAX_K  32
#define ISAL_RECOV_2DATA  0	/* lost: two data disks (P,Q alive)   */
#define ISAL_RECOV_DATAP  1	/* lost: one data disk + P (Q alive)  */

struct isal_cpu_cache {
	int valid, mode, k, faila, failb;
	unsigned char tbls[2 * ISAL_CACHE_MAX_K * 32];	/* 2 KiB */
	unsigned char enc[ISAL_CACHE_MAX_K * ISAL_CACHE_MAX_K];
	unsigned char inv[ISAL_CACHE_MAX_K * ISAL_CACHE_MAX_K];
};
static DEFINE_PER_CPU(struct isal_cpu_cache, isal_cc);

/*
 * Fill srcs[] with the survivor data pointers in the canonical order:
 * surviving data disks ascending, then P (2data only), then Q. Returns the
 * survivor count (always k). Must stay in lockstep with isal_build_tbls().
 */
static int isal_build_srcs(int mode, int k, int faila, int failb,
			   void **ptrs, unsigned char **srcs)
{
	int j, r = 0;

	for (j = 0; j < k; j++) {
		if (j == faila || (mode == ISAL_RECOV_2DATA && j == failb))
			continue;
		srcs[r++] = (unsigned char *)ptrs[j];
	}
	if (mode == ISAL_RECOV_2DATA)
		srcs[r++] = (unsigned char *)ptrs[k];		/* P */
	srcs[r++] = (unsigned char *)ptrs[k + 1];		/* Q */
	return r;
}

/*
 * (Re)build the 2-row decode tables for (mode,k,faila,failb) into `tbls`,
 * using enc/inv scratch (k*k each). Same survivor ordering as isal_build_srcs.
 * Returns 0, or -1 if the survivor matrix is singular.
 */
static int isal_build_tbls(int mode, int k, int faila, int failb,
			   unsigned char *enc, unsigned char *inv,
			   unsigned char *tbls)
{
	unsigned char dec[2 * ISAL_CACHE_MAX_K];
	unsigned char out0[ISAL_CACHE_MAX_K], out1[ISAL_CACHE_MAX_K];
	int i, j, r = 0;

	for (j = 0; j < k; j++) {		/* surviving data rows = e_j */
		if (j == faila || (mode == ISAL_RECOV_2DATA && j == failb))
			continue;
		memset(enc + (size_t)r * k, 0, k);
		enc[(size_t)r * k + j] = 0x01;
		r++;
	}
	if (mode == ISAL_RECOV_2DATA) {		/* P row = all ones */
		for (j = 0; j < k; j++)
			enc[(size_t)r * k + j] = 0x01;
		r++;
	}
	for (j = 0; j < k; j++)			/* Q row = alpha^j */
		enc[(size_t)r * k + j] = alpha_pow(j);

	if (gf_invert_matrix(enc, inv, k) < 0)
		return -1;

	/* Lost output rows over original data. */
	memset(out0, 0, k);
	out0[faila] = 0x01;
	if (mode == ISAL_RECOV_2DATA) {
		memset(out1, 0, k);
		out1[failb] = 0x01;
	} else {
		for (j = 0; j < k; j++)
			out1[j] = 0x01;		/* P = sum of all data */
	}

	for (i = 0; i < k; i++) {		/* dec = [out0; out1] * inv */
		unsigned char a = 0, b = 0;

		for (j = 0; j < k; j++) {
			a ^= gf_mul(out0[j], inv[j * k + i]);
			b ^= gf_mul(out1[j], inv[j * k + i]);
		}
		dec[i]     = a;
		dec[k + i] = b;
	}
	ec_init_tables_gfni(k, 2, dec, tbls);
	return 0;
}

/*
 * Cached fast path: reconstruct the two lost symbols (d0, d1) for the given
 * failure pattern. Returns true on success; false (k too large or singular)
 * tells the caller to use the plain path.
 */
static bool isal_recov_cached(int mode, int k, int faila, int failb,
			      size_t bytes, void **ptrs,
			      unsigned char *d0, unsigned char *d1)
{
	unsigned char *srcs[ISAL_CACHE_MAX_K];
	unsigned char *dests[2];
	struct isal_cpu_cache *cc;

	if (k > ISAL_CACHE_MAX_K)
		return false;

	isal_build_srcs(mode, k, faila, failb, ptrs, srcs);

	preempt_disable();
	cc = this_cpu_ptr(&isal_cc);
	if (!cc->valid || cc->mode != mode || cc->k != k ||
	    cc->faila != faila || cc->failb != failb) {
		if (isal_build_tbls(mode, k, faila, failb,
				    cc->enc, cc->inv, cc->tbls) < 0) {
			cc->valid = 0;
			preempt_enable();
			return false;
		}
		cc->mode  = mode;
		cc->k     = k;
		cc->faila = faila;
		cc->failb = failb;
		cc->valid = 1;
	}
	dests[0] = d0;
	dests[1] = d1;
	kernel_fpu_begin();
	if (use_avx512)
		gf_2vect_dot_prod_avx512_gfni((int)bytes, k, cc->tbls, srcs, dests);
	else
		gf_2vect_dot_prod_avx2_gfni((int)bytes, k, cc->tbls, srcs, dests);
	kernel_fpu_end();
	preempt_enable();
	return true;
}

/*
 * Invert enc (k x k, survivor->data; DESTROYED in place), then for each lost
 * output row (out0, out1 -- each a length-k vector over the original data)
 * compute its decode coefficients dec = out * enc^-1 and run one GFNI
 * 2-output dot product over the survivors. Returns 0 on success.
 */
static int isal_recov_finish(int k, size_t bytes, unsigned char **srcs,
			     unsigned char *enc, const unsigned char *out0,
			     const unsigned char *out1, unsigned char *d0,
			     unsigned char *d1)
{
	unsigned char dec[2 * ISAL_RECOV_MAX_K];
	unsigned char *inv, *tbls;
	unsigned char *dests[2];
	int j, r, ret = -ENOMEM;

	inv  = kmalloc((size_t)k * k, GFP_NOIO);
	tbls = kmalloc((size_t)2 * k * 32, GFP_NOIO);
	if (!inv || !tbls)
		goto out;

	if (gf_invert_matrix(enc, inv, k) < 0) {
		ret = -EINVAL;		/* singular: should not happen for RAID6 */
		goto out;
	}

	/* dec = [out0; out1] * inv   (two 1xk rows times the k x k inverse) */
	for (r = 0; r < k; r++) {
		unsigned char a = 0, b = 0;

		for (j = 0; j < k; j++) {
			a ^= gf_mul(out0[j], inv[j * k + r]);
			b ^= gf_mul(out1[j], inv[j * k + r]);
		}
		dec[r]     = a;		/* row 0, col r */
		dec[k + r] = b;		/* row 1, col r */
	}

	ec_init_tables_gfni(k, 2, dec, tbls);
	dests[0] = d0;
	dests[1] = d1;

	kernel_fpu_begin();
	if (use_avx512)
		gf_2vect_dot_prod_avx512_gfni((int)bytes, k, tbls, srcs, dests);
	else
		gf_2vect_dot_prod_avx2_gfni((int)bytes, k, tbls, srcs, dests);
	kernel_fpu_end();
	ret = 0;
out:
	kfree(tbls);
	kfree(inv);
	return ret;
}

/* Two lost DATA disks (faila < failb); P and Q both alive. */
static void isal_2data_recov(int disks, size_t bytes, int faila, int failb,
			     void **ptrs)
{
	int k = disks - 2;
	unsigned char *enc, *srcs[ISAL_RECOV_MAX_K];
	unsigned char out0[ISAL_RECOV_MAX_K], out1[ISAL_RECOV_MAX_K];
	int j, r;

	if (k < 2)
		goto fallback;
	/* Fast path: per-CPU cached decode tables. */
	if (isal_recov_cached(ISAL_RECOV_2DATA, k, faila, failb, bytes, ptrs,
			      ptrs[faila], ptrs[failb]))
		return;
	if (k > ISAL_RECOV_MAX_K)
		goto fallback;
	enc = kmalloc((size_t)k * k, GFP_NOIO);
	if (!enc)
		goto fallback;

	/* Survivors in order: surviving data disks, then P, then Q. */
	r = 0;
	for (j = 0; j < k; j++) {
		if (j == faila || j == failb)
			continue;
		srcs[r] = ptrs[j];
		memset(enc + (size_t)r * k, 0, k);
		enc[(size_t)r * k + j] = 0x01;		/* e_j */
		r++;
	}
	srcs[r] = ptrs[k];				/* P: all ones */
	for (j = 0; j < k; j++)
		enc[(size_t)r * k + j] = 0x01;
	r++;
	srcs[r] = ptrs[k + 1];				/* Q: alpha^j */
	for (j = 0; j < k; j++)
		enc[(size_t)r * k + j] = alpha_pow(j);

	/* Lost outputs: data[faila] = e_faila, data[failb] = e_failb. */
	memset(out0, 0, k); out0[faila] = 0x01;
	memset(out1, 0, k); out1[failb] = 0x01;

	r = isal_recov_finish(k, bytes, srcs, enc, out0, out1,
			      ptrs[faila], ptrs[failb]);
	kfree(enc);
	if (r == 0)
		return;
fallback:
	saved_2data_recov(disks, bytes, faila, failb, ptrs);
}

/* One lost DATA disk (faila) plus the P disk; Q alive. Recovers data + P. */
static void isal_datap_recov(int disks, size_t bytes, int faila, void **ptrs)
{
	int k = disks - 2;
	unsigned char *enc, *srcs[ISAL_RECOV_MAX_K];
	unsigned char out0[ISAL_RECOV_MAX_K], out1[ISAL_RECOV_MAX_K];
	int j, r;

	if (k < 1)
		goto fallback;
	/* Fast path: per-CPU cached decode tables (failb=-1 keys datap). */
	if (isal_recov_cached(ISAL_RECOV_DATAP, k, faila, -1, bytes, ptrs,
			      ptrs[faila], ptrs[k]))
		return;
	if (k > ISAL_RECOV_MAX_K)
		goto fallback;
	enc = kmalloc((size_t)k * k, GFP_NOIO);
	if (!enc)
		goto fallback;

	/* Survivors: surviving data disks (!= faila), then Q. P is lost. */
	r = 0;
	for (j = 0; j < k; j++) {
		if (j == faila)
			continue;
		srcs[r] = ptrs[j];
		memset(enc + (size_t)r * k, 0, k);
		enc[(size_t)r * k + j] = 0x01;		/* e_j */
		r++;
	}
	srcs[r] = ptrs[k + 1];				/* Q: alpha^j */
	for (j = 0; j < k; j++)
		enc[(size_t)r * k + j] = alpha_pow(j);

	/* Lost outputs: data[faila] = e_faila, P = all ones. */
	memset(out0, 0, k); out0[faila] = 0x01;
	for (j = 0; j < k; j++)
		out1[j] = 0x01;

	r = isal_recov_finish(k, bytes, srcs, enc, out0, out1,
			      ptrs[faila], ptrs[k]);
	kfree(enc);
	if (r == 0)
		return;
fallback:
	saved_datap_recov(disks, bytes, faila, ptrs);
}

static struct raid6_calls saved_call;
static bool overridden;

/*
 * Validate isal_gen_syndrome against the in-tree raid6 impl on a small
 * test stripe (k data + P + Q) before installing. Returns true on byte-
 * for-byte match. We sweep k from 1..MAX_TEST_K so the Vandermonde
 * coefficient layout is verified for several widths.
 */
#define VALIDATE_LEN     256
#define VALIDATE_MAX_K   8

static bool isal_self_validate(void)
{
	unsigned char *buf;
	void *ptrs_ref[VALIDATE_MAX_K + 2];
	void *ptrs_isal[VALIDATE_MAX_K + 2];
	unsigned char *p_ref, *q_ref, *p_isal, *q_isal;
	int k, i;
	bool ok = true;

	buf = kmalloc((VALIDATE_MAX_K + 4) * VALIDATE_LEN, GFP_KERNEL);
	if (!buf)
		return false;

	for (i = 0; i < VALIDATE_MAX_K; i++) {
		unsigned char *d = buf + (size_t)i * VALIDATE_LEN;
		int j;

		for (j = 0; j < VALIDATE_LEN; j++)
			d[j] = (unsigned char)(i * 31 + j * 7 + 1);
	}

	p_ref  = buf + (size_t)(VALIDATE_MAX_K + 0) * VALIDATE_LEN;
	q_ref  = buf + (size_t)(VALIDATE_MAX_K + 1) * VALIDATE_LEN;
	p_isal = buf + (size_t)(VALIDATE_MAX_K + 2) * VALIDATE_LEN;
	q_isal = buf + (size_t)(VALIDATE_MAX_K + 3) * VALIDATE_LEN;

	for (k = 1; k <= VALIDATE_MAX_K; k++) {
		for (i = 0; i < k; i++) {
			ptrs_ref[i]  = buf + (size_t)i * VALIDATE_LEN;
			ptrs_isal[i] = buf + (size_t)i * VALIDATE_LEN;
		}
		ptrs_ref[k]      = p_ref;
		ptrs_ref[k + 1]  = q_ref;
		ptrs_isal[k]     = p_isal;
		ptrs_isal[k + 1] = q_isal;

		memset(p_ref,  0, VALIDATE_LEN);
		memset(q_ref,  0, VALIDATE_LEN);
		memset(p_isal, 0, VALIDATE_LEN);
		memset(q_isal, 0, VALIDATE_LEN);

		raid6_call.gen_syndrome(k + 2, VALIDATE_LEN, ptrs_ref);
		isal_gen_syndrome(k + 2, VALIDATE_LEN, ptrs_isal);

		if (memcmp(p_ref, p_isal, VALIDATE_LEN) != 0) {
			pr_err("raid6_isal: gen P mismatch at k=%d\n", k);
			ok = false;
			break;
		}
		if (memcmp(q_ref, q_isal, VALIDATE_LEN) != 0) {
			pr_err("raid6_isal: gen Q mismatch at k=%d\n", k);
			ok = false;
			break;
		}

		/*
		 * xor_syndrome cross-check. The in-tree impl XOR-accumulates the
		 * incremental contribution of data[start..stop] into the existing
		 * P/Q buffers. Initialise both copies of P/Q to non-zero garbage
		 * (so we don't accidentally mask a pure-XOR bug that would still
		 * produce 0 ^ X == X), then run both impls over the same range
		 * and require byte-for-byte agreement.
		 */
		if (raid6_call.xor_syndrome) {
			int start, stop, j;

			for (start = 0, stop = k - 1; start <= stop;
			     start++, stop = (stop > start ? stop - 1 : stop)) {
				for (j = 0; j < VALIDATE_LEN; j++) {
					p_ref[j]  = (unsigned char)(j * 13 + 0x55);
					q_ref[j]  = (unsigned char)(j * 17 + 0xAA);
					p_isal[j] = p_ref[j];
					q_isal[j] = q_ref[j];
				}
				raid6_call.xor_syndrome(k + 2, start, stop,
							VALIDATE_LEN, ptrs_ref);
				isal_xor_syndrome(k + 2, start, stop,
						  VALIDATE_LEN, ptrs_isal);
				if (memcmp(p_ref, p_isal, VALIDATE_LEN) != 0) {
					pr_err("raid6_isal: xor P mismatch k=%d start=%d stop=%d\n",
					       k, start, stop);
					ok = false;
					goto out;
				}
				if (memcmp(q_ref, q_isal, VALIDATE_LEN) != 0) {
					pr_err("raid6_isal: xor Q mismatch k=%d start=%d stop=%d\n",
					       k, start, stop);
					ok = false;
					goto out;
				}
				if (start == stop)
					break;
			}
		}
	}
out:
	kfree(buf);
	return ok;
}

/*
 * Validate isal_2data_recov / isal_datap_recov against ground truth: build a
 * consistent stripe with the in-tree gen_syndrome, then for every data pair
 * (2data) and every data disk + P (datap), clobber the failed slots, run the
 * GFNI recov, and require the reconstructed bytes to equal the originals.
 */
static bool isal_validate_recov(void)
{
	unsigned char *buf, *gold[VALIDATE_MAX_K + 2];
	void *gptrs[VALIDATE_MAX_K + 2];
	void *ptrs[VALIDATE_MAX_K + 2];
	int k, a, b, i, j;
	bool ok = true;

	/* First half = pristine "gold" stripe; second half = working copy. */
	buf = kmalloc((size_t)(2 * (VALIDATE_MAX_K + 2)) * VALIDATE_LEN,
		      GFP_KERNEL);
	if (!buf)
		return false;
	for (j = 0; j < VALIDATE_MAX_K + 2; j++) {
		gold[j]  = buf + (size_t)j * VALIDATE_LEN;
		gptrs[j] = gold[j];
		ptrs[j]  = buf + (size_t)(VALIDATE_MAX_K + 2 + j) * VALIDATE_LEN;
	}

	for (k = 2; k <= VALIDATE_MAX_K; k++) {
		for (j = 0; j < k; j++)
			for (i = 0; i < VALIDATE_LEN; i++)
				gold[j][i] = (unsigned char)(j * 53 + i * 11 + 7);
		memset(gold[k],     0, VALIDATE_LEN);
		memset(gold[k + 1], 0, VALIDATE_LEN);
		raid6_call.gen_syndrome(k + 2, VALIDATE_LEN, gptrs);

		/* 2data: every data pair, P and Q alive */
		for (a = 0; a < k; a++) {
			for (b = a + 1; b < k; b++) {
				for (j = 0; j < k + 2; j++)
					memcpy(ptrs[j], gold[j], VALIDATE_LEN);
				memset(ptrs[a], 0xEE, VALIDATE_LEN);
				memset(ptrs[b], 0x33, VALIDATE_LEN);
				isal_2data_recov(k + 2, VALIDATE_LEN, a, b, ptrs);
				if (memcmp(ptrs[a], gold[a], VALIDATE_LEN) ||
				    memcmp(ptrs[b], gold[b], VALIDATE_LEN)) {
					pr_err("raid6_isal: 2data recov mismatch k=%d a=%d b=%d\n",
					       k, a, b);
					ok = false;
					goto out;
				}
			}
		}
		/* datap: every data disk plus P, Q alive */
		for (a = 0; a < k; a++) {
			for (j = 0; j < k + 2; j++)
				memcpy(ptrs[j], gold[j], VALIDATE_LEN);
			memset(ptrs[a], 0xEE, VALIDATE_LEN);	/* lost data */
			memset(ptrs[k], 0x55, VALIDATE_LEN);	/* lost P */
			isal_datap_recov(k + 2, VALIDATE_LEN, a, ptrs);
			if (memcmp(ptrs[a], gold[a], VALIDATE_LEN) ||
			    memcmp(ptrs[k], gold[k], VALIDATE_LEN)) {
				pr_err("raid6_isal: datap recov mismatch k=%d a=%d\n",
				       k, a);
				ok = false;
				goto out;
			}
		}
	}
out:
	kfree(buf);
	return ok;
}

int isal_install_raid6_call(void)
{
	if (!isal_have_gfni()) {
		pr_info("raid6_isal: GFNI absent, keeping in-tree '%s'\n",
			raid6_call.name ? raid6_call.name : "?");
		return -ENODEV;
	}

	use_avx512 = isal_have_avx512_gfni();

	/* Save in-tree recov first: isal_*_recov fall back to these. */
	saved_2data_recov = raid6_2data_recov;
	saved_datap_recov = raid6_datap_recov;

	if (!isal_self_validate()) {
		pr_err("raid6_isal: self-validation failed (%s path), NOT installing\n",
		       use_avx512 ? "AVX-512 GFNI" : "AVX2 GFNI");
		return -EINVAL;
	}
	if (!isal_validate_recov()) {
		pr_err("raid6_isal: recov self-validation failed, NOT installing\n");
		return -EINVAL;
	}
	saved_call = raid6_call;
	raid6_call.gen_syndrome = isal_gen_syndrome;
	raid6_call.xor_syndrome = isal_xor_syndrome;
	raid6_call.name = use_avx512 ? "isal_avx512_gfni" : "isal_avx2_gfni";
	raid6_2data_recov = isal_2data_recov;
	raid6_datap_recov = isal_datap_recov;
	overridden = true;
	pr_info("raid6_isal: installed %s gen_syndrome + xor_syndrome + recov (was '%s')\n",
		use_avx512 ? "AVX-512 GFNI" : "AVX2 GFNI",
		saved_call.name ? saved_call.name : "?");
	return 0;
}

void isal_uninstall_raid6_call(void)
{
	if (overridden) {
		raid6_call = saved_call;
		raid6_2data_recov = saved_2data_recov;
		raid6_datap_recov = saved_datap_recov;
		overridden = false;
		pr_info("raid6_isal: restored in-tree raid6_call + recov\n");
	}
}

/*
 * Micro-benchmark: in-tree recov (avx2/ssse3: GFNI gen_syndrome + PSHUFB
 * fixup) vs the unified GFNI dot-product recov, over a large stripe. Only
 * runs when loaded with bench=1; results go to dmesg. Throughput is reported
 * per failed-disk width (2data reconstructs two such widths).
 */
#define BENCH_LEN    4096		/* one page per call, as md drives recov */
#define BENCH_K      6
#define BENCH_ITERS  20000

static void isal_bench_recov(void)
{
	const int k = BENCH_K, disks = BENCH_K + 2;
	const int faila = 1, failb = 4;
	void *ptrs[BENCH_K + 2] = { NULL };
	ktime_t t0;
	s64 stock_ns, isal_ns;
	int i, j;

	/* Page-backed kmalloc buffers, like real md stripe pages (the GFNI
	 * kernels dislike the vmalloc region). */
	for (j = 0; j < disks; j++) {
		ptrs[j] = kmalloc(BENCH_LEN, GFP_KERNEL);
		if (!ptrs[j]) {
			pr_warn("raid6_isal: bench alloc failed\n");
			goto out;
		}
	}
	for (j = 0; j < k; j++)
		memset(ptrs[j], (j * 37 + 1) & 0xff, BENCH_LEN);
	memset(ptrs[k],     0, BENCH_LEN);
	memset(ptrs[k + 1], 0, BENCH_LEN);
	raid6_call.gen_syndrome(disks, BENCH_LEN, ptrs);	/* now GFNI gen */

	/* 2data: in-tree (avx2 PSHUFB fixup) vs isal (GFNI) */
	saved_2data_recov(disks, BENCH_LEN, faila, failb, ptrs);	/* warm */
	t0 = ktime_get();
	for (i = 0; i < BENCH_ITERS; i++)
		saved_2data_recov(disks, BENCH_LEN, faila, failb, ptrs);
	stock_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	isal_2data_recov(disks, BENCH_LEN, faila, failb, ptrs);	/* warm */
	t0 = ktime_get();
	for (i = 0; i < BENCH_ITERS; i++)
		isal_2data_recov(disks, BENCH_LEN, faila, failb, ptrs);
	isal_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	pr_info("raid6_isal: bench 2data k=%d len=%dKiB iters=%d  in-tree=%lld ns (%lld MB/s)  gfni=%lld ns (%lld MB/s)\n",
		k, BENCH_LEN >> 10, BENCH_ITERS,
		stock_ns / BENCH_ITERS,
		stock_ns ? (s64)BENCH_LEN * BENCH_ITERS * 1000 / stock_ns : 0,
		isal_ns / BENCH_ITERS,
		isal_ns ? (s64)BENCH_LEN * BENCH_ITERS * 1000 / isal_ns : 0);

out:
	for (j = 0; j < disks; j++)
		kfree(ptrs[j]);
}

static int __init raid6_isal_init(void)
{
	int ret = isal_install_raid6_call();

	if (ret == 0 && bench)
		isal_bench_recov();
	return ret;
}

static void __exit raid6_isal_exit(void)
{
	isal_uninstall_raid6_call();
}

module_init(raid6_isal_init);
module_exit(raid6_isal_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ISA-L GFNI gen_syndrome + recov override for in-tree raid6");
MODULE_AUTHOR("The Lustre Collective");
