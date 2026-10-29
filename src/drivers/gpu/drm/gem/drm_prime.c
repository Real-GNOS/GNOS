/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_prime.c - PRIME buffer sharing through real dma-buf descriptors.
 * (GPLv2)
 *
 * A dma-buf is a reference-counted handle on an exported GEM object.  The
 * descriptor a client receives is an anonymous VFS node, so it behaves like
 * any other file: it can be duplicated, passed over a socket, and polled.
 * Closing the last descriptor drops the dma-buf's reference, and with it the
 * exporter's GEM reference once no importer is left.
 *
 * The object keeps a weak back-pointer to its cached export.  It is weak on
 * purpose: the buffer already holds a strong reference to the object, and a
 * strong pointer in the other direction would make the pair uncollectable.
 *
 * GNOS has no per-mapping VMA release hook, so the mapping itself owns no
 * reference: the descriptor does, and a client therefore has to keep the
 * dma-buf fd open for as long as the mapping is used (exactly the rule the
 * dma-buf ABI states).
 */

#include <stddef.h>
#include <stdint.h>

#include "anonfd.h"
#include "drm_device.h"
#include "drm_prime.h"
#include "drm_print.h"
#include "smp.h"
#include "syscall.h"
#include "vfs.h"

extern uint64_t g_hhdm; /* HHDM offset, defined in kernel.c */

struct dma_buf {
    struct drm_gem_object *obj;
    uint32_t               size;
    uint32_t               flags;
    int                    refcount;
};

static spinlock_t dma_buf_lock = {0};

/* The dma-buf backend is anonymous-node based; there is no filesystem to
 * register.  Keep the entry point so device registration can state the
 * dependency explicitly, and make it idempotent. */
void drm_prime_init(void) {}

/* Take a dma-buf reference. */
void dma_buf_get(struct dma_buf *dma_buf)
{
    if (dma_buf == NULL) {
        return;
    }

    spin_lock(&dma_buf_lock);
    dma_buf->refcount++;
    spin_unlock(&dma_buf_lock);
}

/* Drop a dma-buf reference, destroying it and releasing its GEM object at
 * zero.  The weak export pointer is cleared under the same lock that
 * protects the cached lookup. */
void dma_buf_put(struct dma_buf *dma_buf)
{
    struct drm_gem_object *obj;

    if (dma_buf == NULL) {
        return;
    }

    spin_lock(&dma_buf_lock);
    if (dma_buf->refcount > 1) {
        dma_buf->refcount--;
        spin_unlock(&dma_buf_lock);
        return;
    }

    obj = dma_buf->obj;
    if (obj != NULL && obj->export_dma_buf == dma_buf) {
        obj->export_dma_buf = NULL;
    }
    dma_buf->obj = NULL;
    spin_unlock(&dma_buf_lock);

    kfree(dma_buf);

    /* Drop the exporting object's reference outside the dma-buf lock. */
    if (obj != NULL) {
        drm_gem_object_put(obj);
    }
}

/*
 * Export a GEM object as a dma-buf.  Repeated exports share one dma-buf;
 * every caller receives its own reference, which the descriptor owns.
 */
struct dma_buf *drm_gem_prime_export(struct drm_gem_object *obj, uint32_t flags)
{
    struct dma_buf *dma_buf;
    struct dma_buf *cached;

    if (obj == NULL) {
        return NULL;
    }
    if ((flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR)) != 0) {
        return NULL;
    }

    spin_lock(&dma_buf_lock);
    cached = obj->export_dma_buf;
    if (cached != NULL) {
        cached->refcount++;
        spin_unlock(&dma_buf_lock);
        return cached;
    }
    spin_unlock(&dma_buf_lock);

    dma_buf = kmalloc(sizeof(*dma_buf));
    if (dma_buf == NULL) {
        return NULL;
    }
    memset(dma_buf, 0, sizeof(*dma_buf));

    dma_buf->obj      = obj;
    dma_buf->size     = obj->size;
    dma_buf->flags    = flags;
    dma_buf->refcount = 1;
    drm_gem_object_get(obj);

    /* Another exporter may have won the race while the buffer was allocated. */
    spin_lock(&dma_buf_lock);
    cached = obj->export_dma_buf;
    if (cached != NULL) {
        cached->refcount++;
        spin_unlock(&dma_buf_lock);
        dma_buf->obj = NULL;
        kfree(dma_buf);
        drm_gem_object_put(obj);
        return cached;
    }
    obj->export_dma_buf = dma_buf;
    spin_unlock(&dma_buf_lock);

    return dma_buf;
}

/*
 * Import a dma-buf that was exported by @dev itself, returning the original
 * GEM object.  This is the safe path for drivers whose objects embed private
 * state and therefore cannot be represented by the generic import wrapper.
 */
struct drm_gem_object *drm_gem_prime_import_same_dev(struct drm_device *dev,
                                                     struct dma_buf    *dma_buf)
{
    if (dev == NULL || dma_buf == NULL || dma_buf->obj == NULL || dma_buf->obj->dev != dev) {
        return NULL;
    }

    drm_gem_object_get(dma_buf->obj);
    return dma_buf->obj;
}

/*
 * Import a dma-buf for @dev.  Same-device imports return the exporting GEM
 * object.  Foreign buffers get a lightweight wrapper that borrows the
 * exporter's backing memory and keeps the dma-buf alive.
 */
struct drm_gem_object *drm_gem_prime_import_dev(struct drm_device *dev, struct dma_buf *dma_buf)
{
    struct drm_gem_object *obj;

    if (dev == NULL || dma_buf == NULL || dma_buf->obj == NULL) {
        return NULL;
    }

    if (dma_buf->obj->dev == dev) {
        return drm_gem_prime_import_same_dev(dev, dma_buf);
    }

    obj = kmalloc(sizeof(*obj));
    if (obj == NULL) {
        DRM_ERROR("Prime: out of memory allocating import wrapper\n");
        return NULL;
    }
    memset(obj, 0, sizeof(*obj));

    drm_gem_object_init(dev, obj, dma_buf->size);
    obj->imported       = true;
    obj->import_dma_buf = dma_buf;
    obj->backing        = dma_buf->obj->backing;
    dma_buf_get(dma_buf);

    if (dma_buf->size != 0 && drm_gem_create_mmap_offset(obj) != 0) {
        DRM_ERROR("Prime: mmap offset allocation failed for imported buffer\n");
        dma_buf_put(dma_buf);
        obj->import_dma_buf = NULL;
        kfree(obj);
        return NULL;
    }

    return obj;
}

/* Drop the descriptor's dma-buf reference when the last fd is closed. */
static void dma_buf_node_release(vfs_node_t *node)
{
    struct dma_buf *dma_buf = (struct dma_buf *)node->priv;

    node->priv = NULL;
    if (dma_buf != NULL) {
        dma_buf_put(dma_buf);
    }
}

/*
 * Map the export's backing memory.  The pages were allocated page-aligned
 * through the PMM and are HHDM-mapped, so the physical address is the
 * kernel-virtual one minus the direct-map base.
 */
static int dma_buf_node_mmap(vfs_node_t *node, uint64_t offset, uint64_t *phys, uint64_t *size)
{
    struct dma_buf *dma_buf = (struct dma_buf *)node->priv;

    if (dma_buf == NULL || dma_buf->obj == NULL || dma_buf->obj->backing == NULL) {
        return -E_INVAL;
    }
    if (offset >= dma_buf->size) {
        return -E_INVAL;
    }

    *phys = ((uint64_t)(uintptr_t)dma_buf->obj->backing + offset) - g_hhdm;
    if (size != NULL) {
        *size = dma_buf->size - offset;
    }
    return 0;
}

static int dma_buf_node_poll(vfs_node_t *node, int16_t events, int16_t *revents)
{
    (void)node;

    /* A dma-buf has no data stream of its own: both directions are always
     * ready, as they are for an ordinary file. */
    if (revents != NULL) {
        *revents = (int16_t)(events & (POLLIN | POLLOUT));
    }
    return 0;
}

static const vfs_ops_t g_dma_buf_ops = {
    .mmap    = dma_buf_node_mmap,
    .poll    = dma_buf_node_poll,
    .release = dma_buf_node_release,
};

/*
 * DRM_IOCTL_PRIME_HANDLE_TO_FD: export a GEM handle as a real dma-buf file
 * descriptor owned by the calling process.
 */
int drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle,
                               uint32_t flags, int *prime_fd)
{
    struct drm_gem_object *obj;
    struct dma_buf        *dma_buf;
    int                    h;
    int                    fd;
    int                    ret;

    (void)dev;

    if (file_priv == NULL || prime_fd == NULL) {
        return -EINVAL;
    }
    if ((flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR)) != 0) {
        return -EINVAL;
    }

    obj = drm_gem_object_lookup(file_priv, handle);
    if (obj == NULL) {
        return -ENOENT;
    }

    dma_buf = drm_gem_prime_export(obj, flags);
    drm_gem_object_put(obj);
    if (dma_buf == NULL) {
        return -ENOMEM;
    }

    h = vfs_anon_open(VFS_ANON, &g_dma_buf_ops, dma_buf, (flags & DRM_RDWR) ? O_RDWR : O_RDONLY);
    if (h < 0) {
        dma_buf_put(dma_buf);
        return h;
    }

    ret = vfs_anon_set_size(h, dma_buf->size);
    if (ret != 0) {
        vfs_file_unref(h); /* its release drops the dma-buf reference */
        return ret;
    }

    /* The node owns the export reference once it is installed; anon_bind()
     * releases the handle itself when the fd table is full. */
    fd = anon_bind(h, (flags & DRM_CLOEXEC) != 0);
    if (fd < 0) {
        return fd;
    }

    *prime_fd = fd;
    return 0;
}

/*
 * DRM_IOCTL_PRIME_FD_TO_HANDLE: import a dma-buf descriptor into @dev and
 * create a GEM handle for it in the calling file.
 */
int drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv, int prime_fd,
                               uint32_t *handle)
{
    const vfs_node_t      *node;
    struct drm_gem_object *obj;
    struct dma_buf        *dma_buf;
    int                    h;
    int                    ret;

    if (dev == NULL || file_priv == NULL || handle == NULL) {
        return -EINVAL;
    }
    if (prime_fd < 0) {
        return -EBADF;
    }

    h = fd_handle(prime_fd);
    if (h < 0) {
        return -EBADF;
    }

    /* Hold the open file while the node is inspected: another thread may
     * close this descriptor between the lookup and the dma-buf get. */
    vfs_file_ref(h);

    node = vfs_file_node(h);
    if (node == NULL || node->kind != VFS_ANON || node->ops != &g_dma_buf_ops ||
        node->priv == NULL) {
        vfs_file_unref(h);
        return -EINVAL;
    }

    dma_buf = (struct dma_buf *)node->priv;
    dma_buf_get(dma_buf);
    vfs_file_unref(h);

    if (dev->driver != NULL && dev->driver->gem_prime_import != NULL) {
        obj = dev->driver->gem_prime_import(dev, dma_buf);
    } else {
        obj = drm_gem_prime_import_dev(dev, dma_buf);
    }

    dma_buf_put(dma_buf);
    if (obj == NULL) {
        return -EINVAL;
    }

    ret = drm_gem_handle_create(file_priv, obj, handle);
    drm_gem_object_put(obj);
    return ret;
}
