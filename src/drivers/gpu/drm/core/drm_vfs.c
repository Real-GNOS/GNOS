/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_vfs.c - the per-file side of the GNOS DRM character devices. (GPLv2)
 *
 * GNOS has no per-open VFS callback: a /dev/dri node is shared by every
 * process that opens it.  The DRM core still needs one struct drm_file for
 * its handle table and event queue, so the bridge keeps a singleton file
 * (created lazily by drm_get_file()) and these callbacks forward into it.
 *
 * The one place the distinction matters is mmap: the offset handed out by
 * MAP_DUMB identifies the buffer, and the backing memory is physically
 * contiguous, so the callback resolves the offset and returns the physical
 * address the page tables need.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_devtmpfs.h"
#include "drm_init.h"
#include "pmm.h"
#include "vfs.h"

extern int  drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset);
extern unsigned int drm_poll(struct drm_file *file_priv, unsigned int events);

/* read() on a DRM node dequeues events: it blocks while there are none and
 * returns 0 once the file is being closed, which is what libdrm expects. */
static int64_t drm_dev_read(void *file, void *addr, size_t offset, size_t size)
{
    return (int64_t)drm_read((struct drm_file *)file, (char *)addr, size, &offset);
}

static size_t drm_dev_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;

    return 0;
}

static int drm_dev_ioctl(void *file, size_t req, void *arg)
{
    struct drm_file   *file_priv = (struct drm_file *)file;
    struct drm_device *dev;

    if (file_priv == NULL) {
        return -ENODEV;
    }

    dev = drm_get_singleton();
    if (dev == NULL) {
        return -ENODEV;
    }

    return drm_ioctl(dev, (unsigned int)req, arg, file_priv);
}

static int drm_dev_poll(void *file, size_t events)
{
    /* Real readiness: readable only while a drm event (vblank/page flip)
     * is queued for this open file description. */
    return (int)drm_poll((struct drm_file *)file, (unsigned int)events);
}

int drm_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    (void)ctx;
    (void)flags;

    return drm_dev_ioctl(private_data, req, arg);
}

int64_t drm_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset,
                          size_t size)
{
    (void)ctx;
    (void)flags;

    return drm_dev_read(private_data, addr, offset, size);
}

int64_t drm_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr,
                           size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;

    return (int64_t)drm_dev_write(private_data, addr, offset, size);
}

int drm_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;

    return drm_dev_poll(private_data, events);
}

void *drm_dev_file_mmap(void *ctx, void *private_data, uint64_t offset, uint64_t size, int flags,
                        struct vm_area *vma)
{
    struct drm_device     *dev       = (struct drm_device *)ctx;
    struct drm_file       *file_priv = (struct drm_file *)private_data;
    struct drm_gem_object *obj;
    void                  *result;

    (void)size;
    (void)flags;
    (void)vma;

    if (dev == NULL) {
        dev = drm_get_singleton();
    }
    if (dev == NULL || file_priv == NULL) {
        return NULL;
    }

    obj = drm_gem_object_lookup_by_offset(file_priv, offset);
    if (obj == NULL || obj->backing == NULL) {
        if (obj != NULL) {
            drm_gem_object_put(obj);
        }
        return NULL;
    }

    /* GNOS has no VMA release hook to hand this reference to, so the lookup
     * reference is dropped here.  The client's GEM handle (or, for a PRIME
     * import, the dma-buf) keeps the object alive while the mapping is in
     * use. */
    result = obj->backing;
    drm_gem_object_put(obj);
    return result;
}
