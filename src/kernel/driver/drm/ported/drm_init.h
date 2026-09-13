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

/* VFS callbacks for devtmpfs (kept for symmetry; GNOS has no per-open
 * directory callbacks, so they are inert). */
void drm_vfs_open_cb(void *parent, const char *name, void *node);
void drm_vfs_close_cb(void *current);

/* Per-open callbacks used by tmpfs/devtmpfs. DRM state is attached to each
 * file descriptor, never to the shared directory node. */
int     drm_dev_open(void *node, uint64_t flags, void **private_data);
void    drm_dev_release(void *node, void *private_data);
int     drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);
int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
int     drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
void   *drm_dev_file_mmap(void *ctx, void *private_data, uint64_t offset, uint64_t size, int flags, struct vm_area *vma);

/* Simpler per-file callbacks (used where no ctx/private_data split exists). */
int64_t drm_dev_read(void *file, void *addr, size_t offset, size_t size);
size_t  drm_dev_write(void *file, const void *addr, size_t offset, size_t size);
int     drm_dev_ioctl(void *file, size_t req, void *arg);
int     drm_dev_poll(void *file, size_t events);
void   *drm_dev_mmap(void *file, size_t offset, size_t size, int flags);

/* Software scanout hooks (called from the timer / refresh thread). */
void drm_dummy_refresh(void);
void drm_dummy_draw_cursor(uint32_t *dst, uint32_t dw, uint32_t dh, uint32_t dstep);

#endif /* INCLUDE_DRM_DRM_INIT_H_ */
