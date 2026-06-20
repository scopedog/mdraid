/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Minimal subset of ISA-L's <erasure_code.h> for use inside the
 * mdraid kernel module. Only the *_base functions are exposed —
 * the SIMD multibinary entry points are intentionally omitted, as
 * is everything that drags in isal_api.h.
 *
 * Vendored from
 * https://github.com/scopedog/isa-l/blob/master/include/erasure_code.h
 */

#ifndef _ISA_L_EC_H_
#define _ISA_L_EC_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

void
ec_init_tables_base(int k, int rows, unsigned char *a, unsigned char *gftbls);

void
ec_encode_data_base(int len, int srcs, int dests, unsigned char *v,
		    unsigned char **src, unsigned char **dest);

void
ec_encode_data_update_base(int len, int k, int rows, int vec_i, unsigned char *v,
			   unsigned char *data, unsigned char **dest);

void
gf_vect_dot_prod_base(int len, int vlen, unsigned char *v, unsigned char **src,
		      unsigned char *dest);

void
gf_vect_mad_base(int len, int vec, int vec_i, unsigned char *v, unsigned char *src,
		 unsigned char *dest);

int
gf_vect_mul_base(int len, unsigned char *a, unsigned char *src, unsigned char *dest);

void
gf_vect_mul_init_base(unsigned char c, unsigned char *tbl);

void
gf_gen_rs_matrix(unsigned char *a, int m, int k);

void
gf_gen_cauchy1_matrix(unsigned char *a, int m, int k);

int
gf_invert_matrix(unsigned char *in_mat, unsigned char *out_mat, const int n);

unsigned char
gf_mul(unsigned char a, unsigned char b);

unsigned char
gf_inv(unsigned char a);

#ifdef __KERNEL__
/*
 * GFNI kernels (kernel-side only). Caller must check isal_have_gfni()
 * returns true before invoking the dot-prod wrapper. The wrapper takes
 * care of kernel_fpu_begin/end internally.
 */

#include <linux/types.h>

bool
isal_have_gfni(void);

void
gf_vect_mul_init_gfni(unsigned char c, unsigned char *tbl);

void
ec_init_tables_gfni(int k, int rows, unsigned char *a, unsigned char *gftbls);

void
gf_vect_dot_prod_gfni(int len, int vec, unsigned char *gftbls,
		      unsigned char **src, unsigned char *dest);

/*
 * Arbitrary k+m Reed-Solomon encode using AVX2 / AVX-512 GFNI kernels.
 * `data` is k source pointers; `coding` is m destination pointers.
 * `g_tbls` is m * k * 32 bytes built via ec_init_tables_gfni().
 * Both functions are kernel_fpu_begin/end-bracketed internally; the
 * AVX-512 variant requires isal_have_avx512_gfni() == true.
 */
bool
isal_have_avx512_gfni(void);

void
ec_encode_data_avx2_gfni(int len, int k, int rows, unsigned char *g_tbls,
			 unsigned char **data, unsigned char **coding);

void
ec_encode_data_avx512_gfni(int len, int k, int rows, unsigned char *g_tbls,
			   unsigned char **data, unsigned char **coding);

/*
 * Incremental parity update used by the small-write RMW path.
 *  coding[r] ^= g_tbls[r][vec_i] * data    for each row r
 * Caller computes delta = new_data XOR old_data over a single source
 * vector and calls this to fold the change into existing parity
 * buffers in place.  FPU state is bracketed internally.
 */
void
ec_encode_data_update_avx2_gfni(int len, int k, int rows, int vec_i,
				unsigned char *g_tbls, unsigned char *data,
				unsigned char **coding);

void
ec_encode_data_update_avx512_gfni(int len, int k, int rows, int vec_i,
				  unsigned char *g_tbls, unsigned char *data,
				  unsigned char **coding);
#endif

#endif /* _ISA_L_EC_H_ */
