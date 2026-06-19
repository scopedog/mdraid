// SPDX-License-Identifier: BSD-3-Clause
/*
 * Kernel-side glue for the vendored ISA-L GFNI erasure-code kernels.
 *
 * - gf_vect_mul_init_gfni / ec_init_tables_gfni: trivial C helpers
 *   that build the 32-byte-per-coefficient GFNI table layout from
 *   gf_table_gfni[] (defined in ec_base.h).
 * - gf_vect_dot_prod_gfni: kernel-FPU-bracketed wrapper around the
 *   vendored gf_vect_dot_prod_avx2_gfni assembly. Caller must have
 *   already verified isal_have_gfni() returns true.
 *
 * gf_vect_mul_init_gfni and ec_init_tables_gfni are vendored from
 * https://github.com/TheLustreCollective/isa-l/blob/master/erasure_code/ec_highlevel_func.c
 */

#include <linux/types.h>
#include <linux/string.h>
#include <linux/export.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>

#include "isa-l_ec.h"
#include "ec_base.h"   /* for gf_table_gfni[] */

extern void
gf_vect_dot_prod_avx2_gfni(int len, int vec, unsigned char *gftbls,
                           unsigned char **src, unsigned char *dest);

extern void
gf_2vect_dot_prod_avx2_gfni(int len, int vec, unsigned char *gftbls,
                            unsigned char **src, unsigned char **dest);

extern void
gf_3vect_dot_prod_avx2_gfni(int len, int vec, unsigned char *gftbls,
                            unsigned char **src, unsigned char **dest);

extern void
gf_vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                             unsigned char **src, unsigned char *dest);

extern void
gf_2vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

extern void
gf_3vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

extern void
gf_4vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

extern void
gf_5vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

extern void
gf_6vect_dot_prod_avx512_gfni(int len, int vec, unsigned char *gftbls,
                              unsigned char **src, unsigned char **dest);

/* Multiply-add (parity-update) kernels for the small-write RMW path. */
extern void
gf_vect_mad_avx2_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
                      unsigned char *src, unsigned char *dest);

extern void
gf_2vect_mad_avx2_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
                       unsigned char *src, unsigned char **dest);

extern void
gf_2vect_mad_avx512_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
                         unsigned char *src, unsigned char **dest);

bool
isal_have_gfni(void)
{
	return boot_cpu_has(X86_FEATURE_GFNI) &&
	       boot_cpu_has(X86_FEATURE_AVX) &&
	       boot_cpu_has(X86_FEATURE_AVX2);
}
EXPORT_SYMBOL_GPL(isal_have_gfni);

bool
isal_have_avx512_gfni(void)
{
	return boot_cpu_has(X86_FEATURE_GFNI) &&
	       boot_cpu_has(X86_FEATURE_AVX512F) &&
	       boot_cpu_has(X86_FEATURE_AVX512BW);
}
EXPORT_SYMBOL_GPL(isal_have_avx512_gfni);

void
gf_vect_mul_init_gfni(unsigned char c, unsigned char *tbl)
{
	const u64 val = gf_table_gfni[c];
	const u64 tmp[4] = { val, val, val, val };

	memcpy(tbl, tmp, 32);
}
EXPORT_SYMBOL_GPL(gf_vect_mul_init_gfni);

void
ec_init_tables_gfni(int k, int rows, unsigned char *a, unsigned char *g_tbls)
{
	int i, j;

	for (i = 0; i < rows; i++) {
		for (j = 0; j < k; j++) {
			gf_vect_mul_init_gfni(*a++, g_tbls);
			g_tbls += 32;
		}
	}
}
EXPORT_SYMBOL_GPL(ec_init_tables_gfni);

void
gf_vect_dot_prod_gfni(int len, int vec, unsigned char *g_tbls,
                      unsigned char **src, unsigned char *dest)
{
	kernel_fpu_begin();
	gf_vect_dot_prod_avx2_gfni(len, vec, g_tbls, src, dest);
	kernel_fpu_end();
}
EXPORT_SYMBOL_GPL(gf_vect_dot_prod_gfni);

/*
 * ec_encode_data_avx2_gfni: arbitrary k+m Reed-Solomon encode using
 * AVX2 GFNI kernels.  Mirrors ISA-L's userspace dispatcher in
 * erasure_code/ec_highlevel_func.c — peels off rows in groups of 3
 * (gf_3vect_dot_prod) and finishes with a 2vect or 1vect call.
 *
 * Caller must have verified isal_have_gfni() and built g_tbls via
 * ec_init_tables_gfni() with `rows` parity rows over `k` data
 * sources.  The whole encode runs inside one kernel_fpu_begin/end
 * window.
 */
void
ec_encode_data_avx2_gfni(int len, int k, int rows, unsigned char *g_tbls,
                         unsigned char **data, unsigned char **coding)
{
	kernel_fpu_begin();
	while (rows >= 3) {
		gf_3vect_dot_prod_avx2_gfni(len, k, g_tbls, data, coding);
		g_tbls += 3 * k * 32;
		coding += 3;
		rows -= 3;
	}
	switch (rows) {
	case 2:
		gf_2vect_dot_prod_avx2_gfni(len, k, g_tbls, data, coding);
		break;
	case 1:
		gf_vect_dot_prod_avx2_gfni(len, k, g_tbls, data, *coding);
		break;
	case 0:
	default:
		break;
	}
	kernel_fpu_end();
}
EXPORT_SYMBOL_GPL(ec_encode_data_avx2_gfni);

/*
 * AVX-512 GFNI variant of ec_encode_data: peels off rows in groups of 6
 * (gf_6vect_dot_prod_avx512_gfni), then 5/4/3/2/1 for the remainder.
 * Caller must have verified isal_have_avx512_gfni() returns true.
 */
void
ec_encode_data_avx512_gfni(int len, int k, int rows, unsigned char *g_tbls,
                           unsigned char **data, unsigned char **coding)
{
	kernel_fpu_begin();
	while (rows >= 6) {
		gf_6vect_dot_prod_avx512_gfni(len, k, g_tbls, data, coding);
		g_tbls += 6 * k * 32;
		coding += 6;
		rows -= 6;
	}
	switch (rows) {
	case 5:
		gf_5vect_dot_prod_avx512_gfni(len, k, g_tbls, data, coding);
		break;
	case 4:
		gf_4vect_dot_prod_avx512_gfni(len, k, g_tbls, data, coding);
		break;
	case 3:
		gf_3vect_dot_prod_avx512_gfni(len, k, g_tbls, data, coding);
		break;
	case 2:
		gf_2vect_dot_prod_avx512_gfni(len, k, g_tbls, data, coding);
		break;
	case 1:
		gf_vect_dot_prod_avx512_gfni(len, k, g_tbls, data, *coding);
		break;
	case 0:
	default:
		break;
	}
	kernel_fpu_end();
}
EXPORT_SYMBOL_GPL(ec_encode_data_avx512_gfni);

/*
 * ec_encode_data_update_avx2_gfni: incremental parity update for a
 * single source vector at column `vec_i` over `rows` parity rows.
 *
 *   coding[r] ^= g_tbls[r][vec_i] * data    for each row r
 *
 * Used by the small-write RMW path: callers compute
 * delta = new_data XOR old_data over a single chunk, then call this
 * to fold the change into the existing parity buffers in place.
 *
 * Caller must have built g_tbls via ec_init_tables_gfni() with
 * `rows` parity rows over a `k`-source matrix and verified
 * isal_have_gfni().  FPU state is bracketed internally.
 */
void
ec_encode_data_update_avx2_gfni(int len, int k, int rows, int vec_i,
                                unsigned char *g_tbls, unsigned char *data,
                                unsigned char **coding)
{
	kernel_fpu_begin();
	while (rows >= 2) {
		gf_2vect_mad_avx2_gfni(len, k, vec_i, g_tbls, data, coding);
		g_tbls += 2 * k * 32;
		coding += 2;
		rows -= 2;
	}
	if (rows == 1)
		gf_vect_mad_avx2_gfni(len, k, vec_i, g_tbls, data, coding[0]);
	kernel_fpu_end();
}
EXPORT_SYMBOL_GPL(ec_encode_data_update_avx2_gfni);

/*
 * AVX-512 variant.  We have gf_2vect_mad_avx512_gfni for paired rows
 * but no 1vect AVX-512 mad kernel — fall back to the AVX2 1vect for
 * the odd-row remainder.  (Both AVX-512 and AVX2 instructions are
 * usable inside one kernel_fpu_begin/end window.)
 */
void
ec_encode_data_update_avx512_gfni(int len, int k, int rows, int vec_i,
                                  unsigned char *g_tbls, unsigned char *data,
                                  unsigned char **coding)
{
	kernel_fpu_begin();
	while (rows >= 2) {
		gf_2vect_mad_avx512_gfni(len, k, vec_i, g_tbls, data, coding);
		g_tbls += 2 * k * 32;
		coding += 2;
		rows -= 2;
	}
	if (rows == 1)
		gf_vect_mad_avx2_gfni(len, k, vec_i, g_tbls, data, coding[0]);
	kernel_fpu_end();
}
EXPORT_SYMBOL_GPL(ec_encode_data_update_avx512_gfni);
