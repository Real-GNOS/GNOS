/*
 * drm_plane.c - the layers a picture is made of. (GPLv2)
 *
 * A plane is one layer of the final image: a framebuffer, a rectangle of
 * it to read (src, in 16.16 fixed point), and a rectangle of the screen to
 * put it in (dst).  Every display has at least a primary plane -- the one
 * that shows the desktop -- and may have a cursor plane and extra overlay
 * planes that the hardware composites for free.
 *
 * Planes carry most of the atomic properties a client sets, so the
 * registration here attaches the whole standard set: FB_ID, CRTC_ID, the
 * four source and four destination coordinates, zpos, alpha and the
 * immutable type.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_port.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define DRM_S32_MAX ((int32_t)0x7fffffff)

/* From drm_mode_object.c. */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

int drm_plane_init(struct drm_device *dev, struct drm_plane *plane, uint32_t possible_crtcs, void *funcs,
                   const uint32_t *formats, unsigned int format_count, const uint64_t *modifiers,
                   enum drm_plane_type type, const char *name)
{
    int ret;

    (void)modifiers; /* only linear is supported for now */
    (void)name;

    if (dev == NULL || plane == NULL || formats == NULL || format_count == 0) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    if (ret != 0) { return ret; }

    drm_modeset_lock_init(&plane->mutex);

    ilist_insert_after(&dev->mode_config.plane_list, &plane->head);

    plane->dev                   = dev;
    plane->possible_crtcs        = possible_crtcs;
    plane->type                  = type;
    plane->state                 = NULL;
    plane->helper_private        = funcs;
    plane->zpos_property_default = 0;

    plane->format_types = malloc((size_t)format_count * sizeof(uint32_t));
    if (plane->format_types == NULL) {
        ilist_remove(&plane->head);
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);
        return -ENOMEM;
    }
    memcpy(plane->format_types, formats, (size_t)format_count * sizeof(uint32_t));
    plane->format_count = format_count;

    plane->modifiers      = NULL;
    plane->modifier_count = 0;

    plane->name = strdup((name != NULL) ? name : "plane");
    if (plane->name == NULL) {
        free(plane->format_types);
        plane->format_types = NULL;
        ilist_remove(&plane->head);
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);
        return -ENOMEM;
    }

    dev->mode_config.num_plane++;
    dev->mode_config.num_total_plane++;

    /* The standard atomic set.  Alpha defaults to fully opaque and the
     * plane type is immutable, because neither can change after the fact. */
    ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_fb_id, 0);
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_id, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_x, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_y, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_w, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_src_h, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_x, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_y, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_w, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_crtc_h, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_zpos, 0); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_alpha, UINT16_MAX); }
    if (ret == 0) { ret = drm_object_attach_property(&plane->base, dev->mode_config.prop_plane_type, type); }
    if (ret != 0) {
        drm_plane_cleanup(plane);
        return ret;
    }

    return 0;
}

/* DRM_IOCTL_MODE_GETPLANERESOURCES: how many planes, and their ids. */
int drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_plane_res *req = (struct drm_mode_get_plane_res *)data;
    uint32_t                       wanted, total, copying, n = 0;
    uint32_t                      *ids = NULL;
    ilist_node_t                  *node;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    wanted  = req->count_planes;
    total   = (uint32_t)dev->mode_config.num_total_plane;
    copying = (wanted < total) ? wanted : total;

    if (copying != 0) {
        ids = malloc((size_t)total * sizeof(*ids));
        if (ids == NULL) { return -ENOMEM; }

        for (node = dev->mode_config.plane_list.next; node != &dev->mode_config.plane_list; node = node->next) {
            ids[n++] = container_of(node, struct drm_plane, head)->base.id;
        }

        if (req->plane_id_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->plane_id_ptr, ids, (size_t)copying * sizeof(*ids)) != 0) {
            free(ids);
            return -EFAULT;
        }
        free(ids);
    }

    req->count_planes = total;
    return 0;
}

/* DRM_IOCTL_MODE_GETPLANE: what this plane can be wired to, the formats it
 * takes, and what it is showing now. */
int drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_plane *req = (struct drm_mode_get_plane *)data;
    struct drm_mode_object    *obj;
    struct drm_plane          *plane;
    uint32_t                   wanted;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    wanted = req->count_format_types;

    obj = drm_mode_object_find(dev, file_priv, req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (obj == NULL) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);

    req->possible_crtcs = plane->possible_crtcs;
    req->crtc_id        = plane->crtc_id;
    req->fb_id          = plane->fb_id;
    req->gamma_size     = 0;

    if (wanted != 0) {
        uint32_t count = (wanted < plane->format_count) ? wanted : plane->format_count;

        if (req->format_type_ptr == 0
            || copy_to_user((void *)(uintptr_t)req->format_type_ptr, plane->format_types,
                            (size_t)count * sizeof(*plane->format_types)) != 0) {
            drm_mode_object_put(obj);
            return -EFAULT;
        }
    }
    req->count_format_types = plane->format_count;

    drm_mode_object_put(obj);
    return 0;
}

/*
 * DRM_IOCTL_MODE_SETPLANE: point a plane at a framebuffer (or at nothing,
 * when both ids are zero).  Rather than poking the hardware directly, this
 * builds a one-plane atomic state and commits it, so the same checks and
 * the same completion path as MODE_ATOMIC apply.
 */
int drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_set_plane *req = (struct drm_mode_set_plane *)data;
    struct drm_mode_object    *obj;
    struct drm_plane          *plane;
    struct drm_crtc           *crtc = NULL;
    struct drm_framebuffer    *fb   = NULL;
    struct drm_atomic_state   *state;
    struct drm_plane_state    *plane_state;
    struct drm_crtc_state     *crtc_state = NULL;
    int                        ret        = 0;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, req->plane_id, DRM_MODE_OBJECT_PLANE);
    if (obj == NULL) { return -ENOENT; }
    plane = container_of(obj, struct drm_plane, base);

    /* A plane with a framebuffer but no CRTC (or the other way round) is
     * meaningless: either show something somewhere, or show nothing. */
    if ((req->crtc_id != 0) != (req->fb_id != 0)) {
        ret = -EINVAL;
        goto out;
    }

    if (req->fb_id != 0) {
        struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, file_priv, req->crtc_id, DRM_MODE_OBJECT_CRTC);

        if (crtc_obj == NULL) {
            ret = -ENOENT;
            goto out;
        }
        crtc = container_of(crtc_obj, struct drm_crtc, base);
        drm_mode_object_put(crtc_obj);

        fb = drm_framebuffer_lookup(dev, file_priv, req->fb_id);
        if (fb == NULL) {
            ret = -ENOENT;
            goto out;
        }

        /* Keep every rectangle inside signed 32 bits, including the far
         * corner: a source or destination of 2^31-1 plus a width is not a
         * rectangle, it is an overflow. */
        if (req->src_w > DRM_S32_MAX || req->src_h > DRM_S32_MAX || req->crtc_w > DRM_S32_MAX
            || req->crtc_h > DRM_S32_MAX
            || (int64_t)(int32_t)req->src_x + req->src_w > DRM_S32_MAX
            || (int64_t)(int32_t)req->src_y + req->src_h > DRM_S32_MAX
            || (int64_t)req->crtc_x + req->crtc_w > DRM_S32_MAX
            || (int64_t)req->crtc_y + req->crtc_h > DRM_S32_MAX) {
            ret = -EINVAL;
            goto out;
        }
    }

    state = drm_atomic_state_alloc(dev);
    if (state == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    state->file_priv = file_priv;

    plane_state = drm_atomic_get_plane_state(state, plane);
    if (plane_state == NULL) {
        drm_atomic_state_free(state);
        ret = -ENOMEM;
        goto out;
    }

    plane_state->crtc = crtc;
    plane_state->fb   = fb;
    plane_state->src  = (struct drm_rect) {(int32_t)req->src_x, (int32_t)req->src_y,
                                           (int32_t)(req->src_x + req->src_w), (int32_t)(req->src_y + req->src_h)};
    plane_state->dst  = (struct drm_rect) {req->crtc_x, req->crtc_y, req->crtc_x + (int32_t)req->crtc_w,
                                           req->crtc_y + (int32_t)req->crtc_h};

    /* The CRTC has to re-evaluate its planes, both when we bind to it and
     * when we let go of the one we were bound to. */
    if (crtc != NULL) {
        crtc_state = drm_atomic_get_crtc_state(state, crtc);
    } else if (plane->state != NULL && plane->state->crtc != NULL) {
        crtc_state = drm_atomic_get_crtc_state(state, plane->state->crtc);
    }

    if (crtc_state != NULL) {
        crtc_state->planes_changed = true;
    } else if (crtc != NULL || (plane->state != NULL && plane->state->crtc != NULL)) {
        drm_atomic_state_free(state);
        ret = -ENOMEM;
        goto out;
    }

    ret = drm_atomic_commit(state);
    if (ret != 0) { drm_atomic_state_free(state); }

out:
    drm_mode_object_put(obj);
    return ret;
}

void drm_plane_cleanup(struct drm_plane *plane)
{
    struct drm_device *dev;

    if (plane == NULL) { return; }

    dev = plane->dev;

    ilist_remove(&plane->head);

    if (dev != NULL) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, plane->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_plane > 0) { dev->mode_config.num_plane--; }
        if (dev->mode_config.num_total_plane > 0) { dev->mode_config.num_total_plane--; }
    }

    free(plane->format_types);
    plane->format_types = NULL;
    plane->format_count = 0;

    free(plane->modifiers);
    plane->modifiers      = NULL;
    plane->modifier_count = 0;

    free(plane->name);
    plane->name = NULL;

    if (plane->base.properties != NULL) {
        drm_property_set_destroy(plane->base.properties);
        free(plane->base.properties);
        plane->base.properties = NULL;
    }
}
