/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shim header so the vendored, byte-identical ec_base.c (synced verbatim
 * from the canonical isa-l tree via sync-ec-to-consumers.sh) can include
 * "erasure_code.h" unchanged. mdraid's public EC surface lives in the
 * minimal-subset isa-l_ec.h; this just forwards to it.
 *
 * Consumer-local glue: NOT part of the synced EC set (see GFNI_VENDORED.md).
 */

#ifndef _ISA_L_ERASURE_CODE_SHIM_H_
#define _ISA_L_ERASURE_CODE_SHIM_H_

#include "isa-l_ec.h"

#endif /* _ISA_L_ERASURE_CODE_SHIM_H_ */
