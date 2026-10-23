/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_init.h — DRM subsystem bootstrap interface.
 *
 * GPLv2 — Copyright 2026 GNOS contributors.
 *
 * Exposes the entry points that bring up the built-in DRM device, the
 * device-list lookups, and the VFS callbacks that bind /dev/dri nodes to
 * the DRM core.
 */

#ifndef INCLUDE_DRM_DRM_INIT_H_
#define INCLUDE_DRM_DRM_INIT_H_

#include "drm_device.h"

struct vm_area;

/* Bring up the DRM class.  Call once after VFS/devtmpfs exist. */
int drm_init(void);

/* Create the fallback software device (card0 + renderD128) and its KMS
 * pipeline.  Skipped when a hardware driver has already registered. */
int drm_init_fallback(void);

/* Look up registered devices.  drm_get_singleton() returns the first
 * registered device, or NULL before init. */
struct drm_device *drm_get_singleton(void);
struct drm_device *drm_get_device_by_minor(int type, int index);
void               drm_device_list_add(struct drm_device *dev);
void               drm_device_list_remove(struct drm_device *dev);

/* Per-open callbacks used by tmpfs/devtmpfs.  DRM state is attached to each
 * file descriptor, never to the shared directory node. */
int     drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);
int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset,
                          size_t size);
int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr,
                           size_t offset, size_t size);
int     drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
void   *drm_dev_file_mmap(void *ctx, void *private_data, uint64_t offset, uint64_t size, int flags,
                          struct vm_area *vma);

#endif /* INCLUDE_DRM_DRM_INIT_H_ */
