/* SPDX-License-Identifier: GPL-2.0 */
/*
 * iso9660.h — read-only ISO9660 volumes (CD-ROM), as a VFS mount. (GPLv2)
 * Layout reference: Linux fs/isofs (PVD at LBA 16, 2048-byte sectors).
 */
#ifndef GNUCOS_ISO9660_H
#define GNUCOS_ISO9660_H

#include "vfs.h"

/* Mount the ISO9660 volume on `dev` at `path`.  `sectors` is the device
 * capacity in 2048-byte units.  Returns 0 or a negative errno. */
int iso9660_mount_bdev(const char *path, struct vfs_node *dev, uint64_t sectors);
int iso9660_umount(const char *path);
int iso9660_route(const char *abs, char *rel);
int iso9660_resolve(const char *rel, struct vfs_node *out);
extern const vfs_ops_t *const iso9660_file_ops(void);
extern const vfs_ops_t *const iso9660_dir_ops(void);
int                           iso9660_statfs(uint64_t *total, uint64_t *free);

#endif
