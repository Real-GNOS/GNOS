/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_prime.h - PRIME buffer sharing through real dma-buf descriptors.
 * (GPLv2)
 *
 * A dma-buf is a reference-counted handle on an exported GEM object.  The
 * descriptor a client receives is an anonymous VFS node; closing the last
 * descriptor drops the dma-buf's reference, and with it the exporter's GEM
 * reference once no importer is left.
 */

#ifndef INCLUDE_DRM_DRM_PRIME_H_
#define INCLUDE_DRM_DRM_PRIME_H_

#include <stdint.h>

#include "drm_device.h"

struct dma_buf;

/* Install the dma-buf backend.  Idempotent; called when a device is
 * registered, before user space can issue a PRIME ioctl. */
void drm_prime_init(void);

/* Take / drop a dma-buf reference. */
void dma_buf_get(struct dma_buf *dma_buf);
void dma_buf_put(struct dma_buf *dma_buf);

/* Export a GEM object, reusing the object's cached export when there is one.
 * Every successful call returns a reference owned by the caller. */
struct dma_buf *drm_gem_prime_export(struct drm_gem_object *obj, uint32_t flags);

/*
 * Import a dma-buf for @dev.  A same-device import returns the exporting GEM
 * object; a foreign buffer gets a lightweight wrapper that borrows the
 * exporter's backing memory and holds its own dma-buf reference.
 */
struct drm_gem_object *drm_gem_prime_import_dev(struct drm_device *dev, struct dma_buf *dma_buf);

/*
 * Import only dma-bufs exported by @dev itself.  Drivers whose GEM objects
 * embed driver-private state must use this instead of accepting a generic
 * wrapper that lacks their object layout.
 */
struct drm_gem_object *drm_gem_prime_import_same_dev(struct drm_device *dev,
                                                     struct dma_buf *dma_buf);

#endif /* INCLUDE_DRM_DRM_PRIME_H_ */
