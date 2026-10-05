/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fatfs_vfs.h — a FAT volume (through FatFs) as a VFS mount. (GPLv2)
 *
 * One volume at a time, exactly like the block-device ext2 mount: the
 * device rides in through the port layer (src/fs/fatfs/diskio.c) and the
 * paths below the mount point are answered by FatFs.
 */
#ifndef GNUCOS_FATFS_VFS_H
#define GNUCOS_FATFS_VFS_H

#include "vfs.h" /* vfs_node_t, vfs_ops_t */

struct vfs_node;

/* Mount the FAT volume on `dev` at `path`.  `sectors` is the device
 * capacity in 512-byte units.  Returns 0 or a negative errno. */
int fatfs_mount_bdev(const char *path, struct vfs_node *dev, uint64_t sectors);
/* Unmount: forget the volume and release the device. */
int fatfs_umount(const char *path);

/* Routing and resolution, called from vfs.c's path walk. */
int fatfs_route(const char *abs, char *rel);
int fatfs_resolve(const char *rel, struct vfs_node *out);

/* The node operations vfs.c installs on resolved FAT nodes. */
extern const vfs_ops_t *const fatfs_file_ops(void);
extern const vfs_ops_t *const fatfs_dir_ops(void);

/* File management, the VFS-side equivalents of the syscalls below a mount. */
int fatfs_unlink(const char *rel);
int fatfs_mkdir(const char *rel);
int fatfs_rename(const char *from, const char *to);
int fatfs_statfs(uint64_t *total, uint64_t *free);

#endif
