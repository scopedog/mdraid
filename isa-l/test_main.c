// SPDX-License-Identifier: GPL-2.0-only
/*
 * isal_test - smoke test for vendored ISA-L *_base path inside a
 * kernel module. Verifies that gf_mul matches expected values and
 * that ec_encode_data_base produces a deterministic, nonzero output
 * for a small (k=4, m=2) Reed-Solomon configuration.
 *
 * On insmod, runs the test and prints PASS/FAIL via dmesg, then
 * stays loaded (rmmod isal_test to remove).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>

#include "isa-l_ec.h"

#define K          4    /* data shards */
#define M          2    /* parity shards */
#define LEN        64   /* bytes per shard (short region: scalar fallback) */
#define LEN_BIG    1024 /* >= GF_REGION_TBL_MIN_LEN: stack-table path */

static int run_test(void)
{
	unsigned char *src_buf, *dst_buf, *dst2_buf;
	unsigned char *src[K], *dst[M], *dst2[M];
	unsigned char *encode_matrix;       /* (K+M) x K */
	unsigned char *parity_coeffs;       /* M x K (rows K..K+M-1 of encode_matrix) */
	unsigned char *g_tbls;              /* 32 * K * M */
	int i, j, rc = 0;
	bool nonzero = false;

	/* Sanity: GF arithmetic table loaded correctly. */
	if (gf_mul(2, 3) != 6) {
		pr_err("isal_test: gf_mul(2,3) = %u, expected 6\n",
		       gf_mul(2, 3));
		return -EINVAL;
	}
	if (gf_mul(0, 0xff) != 0 || gf_mul(0xff, 0) != 0) {
		pr_err("isal_test: gf_mul with zero operand failed\n");
		return -EINVAL;
	}

	/* Allocate K source shards + M destination shards (twice). */
	src_buf  = kmalloc(K * LEN, GFP_KERNEL);
	dst_buf  = kmalloc(M * LEN, GFP_KERNEL);
	dst2_buf = kmalloc(M * LEN, GFP_KERNEL);
	encode_matrix = kmalloc((K + M) * K, GFP_KERNEL);
	g_tbls = kmalloc(32 * K * M, GFP_KERNEL);
	if (!src_buf || !dst_buf || !dst2_buf || !encode_matrix || !g_tbls) {
		rc = -ENOMEM;
		goto out;
	}
	for (i = 0; i < K; i++)
		src[i] = src_buf + i * LEN;
	for (i = 0; i < M; i++) {
		dst[i]  = dst_buf  + i * LEN;
		dst2[i] = dst2_buf + i * LEN;
	}

	/* Deterministic input pattern. */
	for (i = 0; i < K; i++)
		for (j = 0; j < LEN; j++)
			src[i][j] = (unsigned char)(i * 31 + j);

	/* Build Reed-Solomon encode matrix; only the parity rows are used. */
	gf_gen_rs_matrix(encode_matrix, K + M, K);
	parity_coeffs = encode_matrix + K * K;

	ec_init_tables_base(K, M, parity_coeffs, g_tbls);

	memset(dst_buf,  0, M * LEN);
	memset(dst2_buf, 0, M * LEN);

	ec_encode_data_base(LEN, K, M, g_tbls, src, dst);
	ec_encode_data_base(LEN, K, M, g_tbls, src, dst2);

	/* Determinism: two encodes of same input must agree. */
	if (memcmp(dst_buf, dst2_buf, M * LEN) != 0) {
		pr_err("isal_test: ec_encode_data_base non-deterministic\n");
		rc = -EINVAL;
		goto out;
	}

	/* Sanity: nonzero input must produce some nonzero parity bytes. */
	for (i = 0; i < M * LEN; i++) {
		if (dst_buf[i] != 0) {
			nonzero = true;
			break;
		}
	}
	if (!nonzero) {
		pr_err("isal_test: ec_encode_data_base produced all-zero parity\n");
		rc = -EINVAL;
		goto out;
	}

	pr_info("isal_test: base PASS (k=%d m=%d len=%d)\n", K, M, LEN);
	for (i = 0; i < M; i++)
		print_hex_dump(KERN_INFO, "isal_test: base parity ",
			       DUMP_PREFIX_OFFSET, 16, 1,
			       dst[i], LEN, false);

	/* GFNI cross-check: encode same input via gf_vect_dot_prod_gfni and
	 * compare per-row parity bytes against the base path. Skipped on
	 * CPUs without GFNI support. */
	if (isal_have_gfni()) {
		unsigned char *gfni_g_tbls = kmalloc(32 * K * M, GFP_KERNEL);
		if (!gfni_g_tbls) {
			rc = -ENOMEM;
			goto out;
		}
		ec_init_tables_gfni(K, M, parity_coeffs, gfni_g_tbls);
		memset(dst2_buf, 0, M * LEN);
		for (i = 0; i < M; i++)
			gf_vect_dot_prod_gfni(LEN, K,
					      gfni_g_tbls + (size_t)i * K * 32,
					      src, dst2[i]);
		if (memcmp(dst_buf, dst2_buf, M * LEN) != 0) {
			pr_err("isal_test: GFNI parity mismatch vs base\n");
			kfree(gfni_g_tbls);
			rc = -EINVAL;
			goto out;
		}
		pr_info("isal_test: GFNI PASS (matches base path byte-for-byte)\n");
		kfree(gfni_g_tbls);
	} else {
		pr_info("isal_test: GFNI not supported on this CPU, skipped\n");
	}

	/* Large-region cross-check: LEN_BIG >= GF_REGION_TBL_MIN_LEN (256), so the
	 * allocation-free *_base region multiply takes its stack-table path rather
	 * than the short-region scalar gf_mul() fallback exercised above (LEN=64).
	 * Verifies determinism + nonzero, and (on GFNI hosts) byte-for-byte match
	 * against the GFNI path at the large length. */
	{
		unsigned char *lsrc_buf, *lbase_buf, *lcmp_buf;
		unsigned char *lem, *lpc, *lg_base, *lg_gfni;
		unsigned char *lsrc[K], *lbase[M], *lcmp[M];
		bool lb_nonzero = false;

		lsrc_buf  = kmalloc(K * LEN_BIG, GFP_KERNEL);
		lbase_buf = kmalloc(M * LEN_BIG, GFP_KERNEL);
		lcmp_buf  = kmalloc(M * LEN_BIG, GFP_KERNEL);
		lem       = kmalloc((K + M) * K, GFP_KERNEL);
		lg_base   = kmalloc(32 * K * M, GFP_KERNEL);
		lg_gfni   = kmalloc(32 * K * M, GFP_KERNEL);
		if (!lsrc_buf || !lbase_buf || !lcmp_buf || !lem ||
		    !lg_base || !lg_gfni) {
			kfree(lg_gfni); kfree(lg_base); kfree(lem);
			kfree(lcmp_buf); kfree(lbase_buf); kfree(lsrc_buf);
			rc = -ENOMEM;
			goto out;
		}
		for (i = 0; i < K; i++)
			lsrc[i] = lsrc_buf + i * LEN_BIG;
		for (i = 0; i < M; i++) {
			lbase[i] = lbase_buf + i * LEN_BIG;
			lcmp[i]  = lcmp_buf  + i * LEN_BIG;
		}
		for (i = 0; i < K; i++)
			for (j = 0; j < LEN_BIG; j++)
				lsrc[i][j] = (unsigned char)(i * 31 + j * 7 + 1);
		gf_gen_rs_matrix(lem, K + M, K);
		lpc = lem + K * K;
		ec_init_tables_base(K, M, lpc, lg_base);

		memset(lbase_buf, 0, M * LEN_BIG);
		memset(lcmp_buf,  0, M * LEN_BIG);
		ec_encode_data_base(LEN_BIG, K, M, lg_base, lsrc, lbase);
		ec_encode_data_base(LEN_BIG, K, M, lg_base, lsrc, lcmp);
		if (memcmp(lbase_buf, lcmp_buf, M * LEN_BIG) != 0) {
			pr_err("isal_test: large-region base non-deterministic (len=%d)\n",
			       LEN_BIG);
			rc = -EINVAL;
		}
		for (i = 0; !rc && i < M * LEN_BIG; i++)
			if (lbase_buf[i]) {
				lb_nonzero = true;
				break;
			}
		if (!rc && !lb_nonzero) {
			pr_err("isal_test: large-region base all-zero parity (len=%d)\n",
			       LEN_BIG);
			rc = -EINVAL;
		}
		if (!rc && isal_have_gfni()) {
			ec_init_tables_gfni(K, M, lpc, lg_gfni);
			memset(lcmp_buf, 0, M * LEN_BIG);
			for (i = 0; i < M; i++)
				gf_vect_dot_prod_gfni(LEN_BIG, K,
						      lg_gfni + (size_t)i * K * 32,
						      lsrc, lcmp[i]);
			if (memcmp(lbase_buf, lcmp_buf, M * LEN_BIG) != 0) {
				pr_err("isal_test: large-region GFNI mismatch vs base (len=%d)\n",
				       LEN_BIG);
				rc = -EINVAL;
			}
		}
		if (!rc)
			pr_info("isal_test: large-region base PASS (len=%d, stack-table path%s)\n",
				LEN_BIG,
				isal_have_gfni() ? " + GFNI match" : "");
		kfree(lg_gfni); kfree(lg_base); kfree(lem);
		kfree(lcmp_buf); kfree(lbase_buf); kfree(lsrc_buf);
		if (rc)
			goto out;
	}

	/* k+m cross-check: ec_encode_data_avx2_gfni vs ec_encode_data_base for
	 * a sweep of (k, m) configurations.  This exercises the multi-row
	 * dispatcher (1vect / 2vect / 3vect, with peel-off in groups of 3). */
	if (isal_have_gfni()) {
		const int km_pairs[][2] = {
			{4, 1}, {4, 2}, {4, 3},
			{6, 4}, {8, 5}, {10, 6},
			{12, 7}, {16, 8},
		};
		const int n_pairs = (int)(sizeof(km_pairs) / sizeof(km_pairs[0]));
		bool km_ok = true;
		int p;

		for (p = 0; p < n_pairs && km_ok; p++) {
			int kk = km_pairs[p][0];
			int mm = km_pairs[p][1];
			unsigned char *km_src_buf, *km_dst_base, *km_dst_gfni;
			unsigned char *km_em, *km_pc, *km_g_base, *km_g_gfni;
			unsigned char **km_src, **km_db, **km_dg;
			int x;

			km_src_buf  = kmalloc(kk * LEN, GFP_KERNEL);
			km_dst_base = kmalloc(mm * LEN, GFP_KERNEL);
			km_dst_gfni = kmalloc(mm * LEN, GFP_KERNEL);
			km_em       = kmalloc((kk + mm) * kk, GFP_KERNEL);
			km_g_base   = kmalloc(32 * kk * mm, GFP_KERNEL);
			km_g_gfni   = kmalloc(32 * kk * mm, GFP_KERNEL);
			km_src = kmalloc(kk * sizeof(*km_src), GFP_KERNEL);
			km_db  = kmalloc(mm * sizeof(*km_db), GFP_KERNEL);
			km_dg  = kmalloc(mm * sizeof(*km_dg), GFP_KERNEL);
			if (!km_src_buf || !km_dst_base || !km_dst_gfni ||
			    !km_em || !km_g_base || !km_g_gfni ||
			    !km_src || !km_db || !km_dg) {
				km_ok = false;
				goto km_free;
			}
			for (x = 0; x < kk; x++)
				km_src[x] = km_src_buf + x * LEN;
			for (x = 0; x < mm; x++) {
				km_db[x] = km_dst_base + x * LEN;
				km_dg[x] = km_dst_gfni + x * LEN;
			}
			for (x = 0; x < kk; x++)
				for (j = 0; j < LEN; j++)
					km_src[x][j] =
						(unsigned char)(x * 17 + j * 5 + 3);
			gf_gen_rs_matrix(km_em, kk + mm, kk);
			km_pc = km_em + kk * kk;
			ec_init_tables_base(kk, mm, km_pc, km_g_base);
			ec_init_tables_gfni(kk, mm, km_pc, km_g_gfni);

			memset(km_dst_base, 0, mm * LEN);
			memset(km_dst_gfni, 0, mm * LEN);
			ec_encode_data_base(LEN, kk, mm, km_g_base,
					    km_src, km_db);
			ec_encode_data_avx2_gfni(LEN, kk, mm, km_g_gfni,
						 km_src, km_dg);

			if (memcmp(km_dst_base, km_dst_gfni, mm * LEN) != 0) {
				pr_err("isal_test: k+m mismatch at k=%d m=%d\n",
				       kk, mm);
				km_ok = false;
			}
km_free:
			kfree(km_dg); kfree(km_db); kfree(km_src);
			kfree(km_g_gfni); kfree(km_g_base);
			kfree(km_em);
			kfree(km_dst_gfni); kfree(km_dst_base); kfree(km_src_buf);
		}
		if (km_ok)
			pr_info("isal_test: k+m AVX2 PASS (ec_encode_data_avx2_gfni matches base for %d configs up to k=16 m=8)\n",
				n_pairs);
		else {
			rc = -EINVAL;
			goto out;
		}
	}

	/* Same k+m sweep but via the AVX-512 dispatcher (peels in groups of
	 * 6, with 5/4/3/2/1-vect handlers for the remainder). Skipped on
	 * hosts without AVX-512F + AVX-512BW + GFNI. */
	if (isal_have_avx512_gfni()) {
		const int km_pairs[][2] = {
			{4, 1}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6},
			{6, 4}, {8, 5}, {10, 6}, {12, 7}, {16, 8},
		};
		const int n_pairs = (int)(sizeof(km_pairs) / sizeof(km_pairs[0]));
		bool km_ok = true;
		int p;

		for (p = 0; p < n_pairs && km_ok; p++) {
			int kk = km_pairs[p][0];
			int mm = km_pairs[p][1];
			unsigned char *km_src_buf, *km_dst_base, *km_dst_gfni;
			unsigned char *km_em, *km_pc, *km_g_base, *km_g_gfni;
			unsigned char **km_src, **km_db, **km_dg;
			int x;

			km_src_buf  = kmalloc(kk * LEN, GFP_KERNEL);
			km_dst_base = kmalloc(mm * LEN, GFP_KERNEL);
			km_dst_gfni = kmalloc(mm * LEN, GFP_KERNEL);
			km_em       = kmalloc((kk + mm) * kk, GFP_KERNEL);
			km_g_base   = kmalloc(32 * kk * mm, GFP_KERNEL);
			km_g_gfni   = kmalloc(32 * kk * mm, GFP_KERNEL);
			km_src = kmalloc(kk * sizeof(*km_src), GFP_KERNEL);
			km_db  = kmalloc(mm * sizeof(*km_db), GFP_KERNEL);
			km_dg  = kmalloc(mm * sizeof(*km_dg), GFP_KERNEL);
			if (!km_src_buf || !km_dst_base || !km_dst_gfni ||
			    !km_em || !km_g_base || !km_g_gfni ||
			    !km_src || !km_db || !km_dg) {
				km_ok = false;
				goto km512_free;
			}
			for (x = 0; x < kk; x++)
				km_src[x] = km_src_buf + x * LEN;
			for (x = 0; x < mm; x++) {
				km_db[x] = km_dst_base + x * LEN;
				km_dg[x] = km_dst_gfni + x * LEN;
			}
			for (x = 0; x < kk; x++)
				for (j = 0; j < LEN; j++)
					km_src[x][j] =
						(unsigned char)(x * 17 + j * 5 + 3);
			gf_gen_rs_matrix(km_em, kk + mm, kk);
			km_pc = km_em + kk * kk;
			ec_init_tables_base(kk, mm, km_pc, km_g_base);
			ec_init_tables_gfni(kk, mm, km_pc, km_g_gfni);

			memset(km_dst_base, 0, mm * LEN);
			memset(km_dst_gfni, 0, mm * LEN);
			ec_encode_data_base(LEN, kk, mm, km_g_base,
					    km_src, km_db);
			ec_encode_data_avx512_gfni(LEN, kk, mm, km_g_gfni,
						   km_src, km_dg);

			if (memcmp(km_dst_base, km_dst_gfni, mm * LEN) != 0) {
				pr_err("isal_test: k+m AVX-512 mismatch at k=%d m=%d\n",
				       kk, mm);
				km_ok = false;
			}
km512_free:
			kfree(km_dg); kfree(km_db); kfree(km_src);
			kfree(km_g_gfni); kfree(km_g_base);
			kfree(km_em);
			kfree(km_dst_gfni); kfree(km_dst_base); kfree(km_src_buf);
		}
		if (km_ok)
			pr_info("isal_test: k+m AVX-512 PASS (ec_encode_data_avx512_gfni matches base for %d configs up to k=16 m=8)\n",
				n_pairs);
		else {
			rc = -EINVAL;
			goto out;
		}
	} else {
		pr_info("isal_test: AVX-512 GFNI not supported on this CPU, k+m AVX-512 cross-check skipped\n");
	}

out:
	kfree(g_tbls);
	kfree(encode_matrix);
	kfree(dst2_buf);
	kfree(dst_buf);
	kfree(src_buf);
	return rc;
}

static int __init isal_test_init(void)
{
	int rc = run_test();
	if (rc)
		pr_err("isal_test: FAIL (rc=%d)\n", rc);
	return rc;
}

static void __exit isal_test_exit(void)
{
}

module_init(isal_test_init);
module_exit(isal_test_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Smoke test for vendored ISA-L *_base path in a kernel module");
MODULE_AUTHOR("Scopedog");
