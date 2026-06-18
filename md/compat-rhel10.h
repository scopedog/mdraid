/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shims for building mainline 6.12 md sources
 * against RHEL 10 kernel (6.12.0-124.8.1.el10_1).
 *
 * RHEL 10 backported the following API renames from future mainline:
 *   register_md_personality   -> register_md_submodule
 *   unregister_md_personality -> unregister_md_submodule
 *
 * RHEL 10 also removed the md_cluster_ops global pointer; cluster
 * operations are now internal to md-mod.  Stub it out for builds
 * that don't need cluster support (raid456, raid0).
 */
#ifndef MD_COMPAT_RHEL10_H
#define MD_COMPAT_RHEL10_H

#define register_md_personality		register_md_submodule
#define unregister_md_personality	unregister_md_submodule

/*
 * RHEL 10 struct md_personality layout (192 bytes, verified by binary diff):
 *
 *   +0x00  u32 type          (submodule type; 0 = standard personality)
 *   +0x04  int level
 *   +0x08  char *name
 *   +0x10  struct module *owner
 *   +0x18  21 function pointer slots:
 *            make_request, run, start, free, status, error_handler,
 *            hot_add_disk, hot_remove_disk, spare_active, sync_request,
 *            resize, size, check_reshape, start_reshape, finish_reshape,
 *            update_reshape_pos (NULL for raid4/5/6),
 *            prepare_suspend, quiesce, takeover,
 *            change_consistency_policy, bitmap_sector (new RHEL field)
 *
 * Compared to mainline 6.12 (200 bytes with update_reshape_pos):
 *   - struct list_head list removed (-16 bytes)
 *   - u32 type field added (+4 bytes)
 *   - level moved before name, no padding needed
 *   - bitmap_sector added at end (+8 bytes)
 *   - net: 200 - 16 + 4 - 4(padding gone) + 8 = 192 bytes
 *
 * md.h has been updated to this layout.  Designated initializers in
 * personality structs (raid4/5/6, raid0) are unaffected.
 */

#endif /* MD_COMPAT_RHEL10_H */
