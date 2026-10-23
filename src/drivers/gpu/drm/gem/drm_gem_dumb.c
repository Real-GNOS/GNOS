/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_gem_dumb.c - dumb buffer allocation for /dev/dri. (GPLv2)
 *
 * A dumb buffer is a page-aligned GEM object the client may map; it needs no
 * driver-private layout, so the implementation is shared by every device.
 * MAP_DUMB returns the object's private mmap offset, which is what the DRM
 * node's mmap path uses to find the object again.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_mode.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"

/* DRM_IOCTL_MODE_CREATE_DUMB: make a buffer of a given width, height and
 * depth.  The backing store is page-aligned because it gets mapped straight
 * into user space. */
int drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev,
                        struct drm_mode_create_dumb *args)
{
    struct drm_gem_object *obj;
    uint32_t               handle;
    size_t                 size;
    int                    ret;

    if (file_priv == NULL || dev == NULL || args == NULL) {
        return -EINVAL;
    }
    if (args->width == 0 || args->height == 0 || args->bpp == 0) {
        return -EINVAL;
    }
    if (args->bpp % 8 != 0) {
        return -EINVAL;
    }

    args->pitch = args->width * (args->bpp / 8);
    size        = (size_t)args->pitch * args->height;
    args->size  = (uint64_t)size;

    obj = malloc(sizeof(*obj));
    if (obj == NULL) {
        return -ENOMEM;
    }
    memset(obj, 0, sizeof(*obj));

    drm_gem_object_init(dev, obj, size);

    /* Assigned here rather than at mmap time so the offset is stable for
     * the life of the buffer: a client may ask for it more than once. */
    ret = drm_gem_create_mmap_offset(obj);
    if (ret != 0) {
        DRM_ERROR("Dumb_create: mmap offset exhausted\n");
        free(obj);
        return ret;
    }

    if (size > 0) {
        obj->backing = aligned_alloc(4096, size);
        if (obj->backing == NULL) {
            drm_gem_free_mmap_offset(obj);
            free(obj);
            return -ENOMEM;
        }
        memset(obj->backing, 0, size);
    }

    ret = drm_gem_handle_create(file_priv, obj, &handle);
    if (ret < 0) {
        aligned_free(obj->backing);
        drm_gem_free_mmap_offset(obj);
        free(obj);
        return ret;
    }

    args->handle = handle;

    /* The handle owns the object from here on. */
    drm_gem_object_put(obj);

    return 0;
}

/* DRM_IOCTL_MODE_MAP_DUMB: hand back the offset the client should mmap. */
int drm_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle,
                            uint64_t *offset)
{
    struct drm_gem_object *obj;

    (void)dev;

    obj = drm_gem_object_lookup(file_priv, handle);
    if (obj == NULL) {
        return -ENOENT;
    }

    *offset = obj->mmap_offset;

    drm_gem_object_put(obj);
    return 0;
}

/* DRM_IOCTL_MODE_DESTROY_DUMB: a dumb buffer is an ordinary GEM object, so
 * dropping its handle is the whole of it. */
int drm_gem_dumb_destroy(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle)
{
    (void)dev;

    return drm_gem_handle_delete(file_priv, handle);
}
