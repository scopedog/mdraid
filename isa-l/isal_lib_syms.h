/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * isal_lib.ko export prefix.
 *
 * The ISA-L kernel port in this directory is vendored, and other out-of-tree
 * modules vendor the same code and export it under the upstream ISA-L API
 * names.  The kernel has no way to arbitrate that: verify_exported_symbols()
 * in kernel/module/main.c matches on the bare symbol name, so the second
 * module to load is rejected with -ENOEXEC and
 *
 *   "%s: exports duplicate symbol %s (owned by %s)"
 *
 * taking its consumers (raidkm.ko, raid_isal.ko) down with it.  Which module
 * loses depends only on boot order.  Symbol namespaces do not fix this:
 * EXPORT_SYMBOL_NS adds an import-side check and leaves the name global.
 *
 * So isal_lib.ko yields the canonical names and prefixes every export with
 * isal_lib_.  See GFNI_VENDORED.md for which other consumers share this code.
 *
 * This is done with macros rather than by editing the sources because
 * ec_base.c, ec_base.h and the twelve gf_*vect_*_gfni.c kernels are kept
 * byte-identical to the canonical isa-l tree (see GFNI_VENDORED.md) and must
 * not be hand-edited in a consumer tree.  The renames reach them two ways:
 *
 *   - isa-l_ec.h includes this header, which covers ec_base.c (via the
 *     erasure_code.h shim), gfni_glue.c, lib_main.c, raid6_isal.c,
 *     test_main.c and every out-of-tree consumer (md-kmec's raid_km.c);
 *   - Kbuild adds -include for it, which covers the vendored GFNI kernels;
 *     they include no project header of their own.
 *
 * Because the macro rewrites definitions and call sites alike, source in
 * this tree keeps reading in the plain ISA-L names; only the ELF symbol
 * table changes.
 *
 * Keep in sync with the EXPORT_SYMBOL_GPL sites in ec_base.c, gfni_glue.c
 * and lib_main.c: every exported symbol must appear here.
 */

#ifndef _ISAL_LIB_SYMS_H_
#define _ISAL_LIB_SYMS_H_

/* Scalar base path (ec_base.c) */
#define ec_encode_data_base        isal_lib_ec_encode_data_base
#define ec_encode_data_update_base isal_lib_ec_encode_data_update_base
#define ec_init_tables_base        isal_lib_ec_init_tables_base
#define gf_vect_dot_prod_base      isal_lib_gf_vect_dot_prod_base
#define gf_vect_mad_base           isal_lib_gf_vect_mad_base
#define gf_vect_mul_base           isal_lib_gf_vect_mul_base
#define gf_vect_mul_init_base      isal_lib_gf_vect_mul_init_base

/* CPU feature predicates (gfni_glue.c) */
#define isal_have_avx512_gfni isal_lib_isal_have_avx512_gfni
#define isal_have_gfni        isal_lib_isal_have_gfni

/* Matrix / field helpers (ec_base.c) */
#define gf_gen_cauchy1_matrix isal_lib_gf_gen_cauchy1_matrix
#define gf_gen_rs_matrix      isal_lib_gf_gen_rs_matrix
#define gf_inv                isal_lib_gf_inv
#define gf_invert_matrix      isal_lib_gf_invert_matrix
#define gf_mul                isal_lib_gf_mul

/* GFNI dispatchers (gfni_glue.c) */
#define ec_encode_data_avx2_gfni          isal_lib_ec_encode_data_avx2_gfni
#define ec_encode_data_avx512_gfni        isal_lib_ec_encode_data_avx512_gfni
#define ec_encode_data_update_avx2_gfni   isal_lib_ec_encode_data_update_avx2_gfni
#define ec_encode_data_update_avx512_gfni isal_lib_ec_encode_data_update_avx512_gfni
#define ec_init_tables_gfni               isal_lib_ec_init_tables_gfni
#define gf_vect_dot_prod_gfni             isal_lib_gf_vect_dot_prod_gfni
#define gf_vect_mul_init_gfni             isal_lib_gf_vect_mul_init_gfni

/* Vendored GFNI kernels (gf_*vect_*_gfni.c) */
#define gf_2vect_dot_prod_avx2_gfni   isal_lib_gf_2vect_dot_prod_avx2_gfni
#define gf_2vect_dot_prod_avx512_gfni isal_lib_gf_2vect_dot_prod_avx512_gfni
#define gf_2vect_mad_avx2_gfni        isal_lib_gf_2vect_mad_avx2_gfni
#define gf_2vect_mad_avx512_gfni      isal_lib_gf_2vect_mad_avx512_gfni
#define gf_3vect_dot_prod_avx2_gfni   isal_lib_gf_3vect_dot_prod_avx2_gfni
#define gf_3vect_dot_prod_avx512_gfni isal_lib_gf_3vect_dot_prod_avx512_gfni
#define gf_4vect_dot_prod_avx512_gfni isal_lib_gf_4vect_dot_prod_avx512_gfni
#define gf_5vect_dot_prod_avx512_gfni isal_lib_gf_5vect_dot_prod_avx512_gfni
#define gf_6vect_dot_prod_avx512_gfni isal_lib_gf_6vect_dot_prod_avx512_gfni
#define gf_vect_dot_prod_avx2_gfni    isal_lib_gf_vect_dot_prod_avx2_gfni
#define gf_vect_dot_prod_avx512_gfni  isal_lib_gf_vect_dot_prod_avx512_gfni
#define gf_vect_mad_avx2_gfni         isal_lib_gf_vect_mad_avx2_gfni

#endif /* _ISAL_LIB_SYMS_H_ */
