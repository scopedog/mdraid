// SPDX-License-Identifier: GPL-2.0-only
/*
 * isal_lib.ko - shared kernel-side ISA-L primitives.
 *
 * Bundles the patent-clean *_base scalar code (ec_base.c), the
 * GFNI-accelerated dispatchers (gfni_glue.c), and the vendored NASM
 * assembly kernels into one module that raid_isal.ko, isal_test.ko
 * and kmec.ko all depend on.  Before this split each consumer
 * statically linked its own copy (~700 KiB duplicated per module).
 *
 * Public symbols are exported from the .c files where they're
 * defined.  This file's only job is:
 *  - register a module (so insmod/rmmod works) with MODULE_LICENSE
 *    so its EXPORT_SYMBOL_GPLs are accepted by other modules;
 *  - export the asm trampolines from gf_*vect_*_gfni.asm — they're
 *    defined in NASM and kbuild has no in-asm equivalent of
 *    EXPORT_SYMBOL, so we mirror their declarations here in C and
 *    use EXPORT_SYMBOL_GPL.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/export.h>

#include "isa-l_ec.h"

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
extern void
gf_vect_mad_avx2_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
		      unsigned char *src, unsigned char *dest);
extern void
gf_2vect_mad_avx2_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
		       unsigned char *src, unsigned char **dest);
extern void
gf_2vect_mad_avx512_gfni(int len, int vec, int vec_i, unsigned char *gftbls,
			 unsigned char *src, unsigned char **dest);

EXPORT_SYMBOL_GPL(gf_vect_dot_prod_avx2_gfni);
EXPORT_SYMBOL_GPL(gf_2vect_dot_prod_avx2_gfni);
EXPORT_SYMBOL_GPL(gf_3vect_dot_prod_avx2_gfni);
EXPORT_SYMBOL_GPL(gf_vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_2vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_3vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_4vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_5vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_6vect_dot_prod_avx512_gfni);
EXPORT_SYMBOL_GPL(gf_vect_mad_avx2_gfni);
EXPORT_SYMBOL_GPL(gf_2vect_mad_avx2_gfni);
EXPORT_SYMBOL_GPL(gf_2vect_mad_avx512_gfni);

static int __init isal_lib_init(void)
{
	pr_info("isal_lib: loaded (%s%s)\n",
		isal_have_gfni() ? "GFNI" : "no-GFNI",
		isal_have_avx512_gfni() ? " AVX-512" : "");
	return 0;
}

static void __exit isal_lib_exit(void)
{
	pr_info("isal_lib: unloaded\n");
}

module_init(isal_lib_init);
module_exit(isal_lib_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Shared ISA-L primitives (base + GFNI) for raid_isal/kmec");
MODULE_AUTHOR("Hiroshi Nishida");
