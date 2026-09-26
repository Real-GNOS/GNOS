/*
 * drm_framebuffer.c - the thing a CRTC scans out. (GPLv2)
 *
 * A framebuffer binds pixels to geometry: this many pixels wide, that many
 * rows, pixels laid out according to that fourcc, stored in these GEM
 * buffers with these pitches and offsets.  It is the one object every
 * scanout path has in common, so most of this file is the two ways of
 * creating one (ADDFB from the old bpp/depth pair, ADDFB2 from a fourcc
 * plus optional modifiers), the ways of asking about one, and tearing one
 * down -- including making sure no plane is still pointed at it.
 *
 * Validation is deliberately strict: a framebuffer that says it is bigger
 * than the memory behind it is a security hole, not an inconvenience, so
 * every pitch/offset/size combination is checked against the backing
 * object before the framebuffer becomes visible.
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

/* From drm_mode_object.c. */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/* --------------------------------------------------------------- lifecycle */

/*
 * Publish @fb: give it an id, put it on the device's framebuffer list and
 * on the owning file's list, and count it.
 */
int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb, const struct drm_framebuffer_funcs *funcs)
{
    uint32_t id = 0;
    int      ret;

    if (dev == NULL || fb == NULL) { return -EINVAL; }

    fb->funcs = funcs;

    spin_lock(&dev->mode_config.fb_lock);
    ret = drm_idr_alloc(&dev->mode_config.fb_idr, fb, 1, 0, &id);
    spin_unlock(&dev->mode_config.fb_lock);
    if (ret != 0) { return ret; }

    ret = drm_mode_object_idr_alloc(dev, &fb->base, DRM_MODE_OBJECT_FB);
    if (ret != 0) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, id);
        spin_unlock(&dev->mode_config.fb_lock);
        return ret;
    }

    fb->id = (int)id;

    ilist_insert_after(&dev->mode_config.fb_list, &fb->head);
    if (fb->file != NULL) { ilist_insert_after(&fb->file->fbs_head, &fb->filp_head); }

    dev->mode_config.num_fb++;

    return 0;
}

/* The reverse: drop the references, leave both lists, forget both ids.
 * The struct itself belongs to the caller. */
void drm_framebuffer_cleanup(struct drm_framebuffer *fb)
{
    struct drm_device *dev;
    int                i;

    if (fb == NULL) { return; }

    dev = fb->base.dev;

    for (i = 0; i < 4; i++) {
        if (fb->obj[i] != NULL) {
            drm_gem_object_put(fb->obj[i]);
            fb->obj[i] = NULL;
        }
    }

    ilist_remove(&fb->head);
    if (fb->file != NULL) {
        ilist_remove(&fb->filp_head);
        fb->file = NULL;
    }

    if (dev != NULL) {
        spin_lock(&dev->mode_config.fb_lock);
        drm_idr_remove(&dev->mode_config.fb_idr, (uint32_t)fb->id);
        spin_unlock(&dev->mode_config.fb_lock);

        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, fb->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_fb > 0) { dev->mode_config.num_fb--; }
    }
}

/*
 * Resolve @id.  User space names framebuffers by their mode-object id --
 * the shared space ADDFB and ADDFB2 allocate from -- so that is looked up
 * first; the framebuffer table, which numbers from its own sequence, is
 * kept as a fallback for internal callers holding an old-style id.
 * No reference is handed out.
 */
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file_priv, uint32_t id)
{
    struct drm_framebuffer *fb;
    struct drm_mode_object *obj;

    if (dev == NULL) { return NULL; }

    obj = drm_mode_object_find(dev, file_priv, id, DRM_MODE_OBJECT_FB);
    if (obj != NULL) {
        fb = container_of(obj, struct drm_framebuffer, base);
        drm_mode_object_put(obj); /* lookup contract: caller gets no ref */
        return fb;
    }

    spin_lock(&dev->mode_config.fb_lock);
    fb = drm_idr_find(&dev->mode_config.fb_idr, id);
    spin_unlock(&dev->mode_config.fb_lock);

    return fb;
}

/* ---------------------------------------------------------------- creation */

/*
 * DRM_IOCTL_MODE_ADDFB.  The old spelling: there is no fourcc, only a
 * bits-per-pixel and a depth, so the format has to be inferred.  Only the
 * combinations that map unambiguously onto a fourcc are accepted.
 */
int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd *req = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;
    struct drm_gem_object  *obj;
    uint32_t                format;
    uint32_t                bytes_per_pixel;
    int                     ret;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    if (req->bpp == 32 && req->depth == 24) {
        format = DRM_FORMAT_XRGB8888;
    } else if (req->bpp == 32 && req->depth == 32) {
        format = DRM_FORMAT_ARGB8888;
    } else if (req->bpp == 24 && req->depth == 24) {
        format = DRM_FORMAT_RGB888;
    } else if (req->bpp == 16 && req->depth == 16) {
        format = DRM_FORMAT_RGB565;
    } else if (req->bpp == 16 && req->depth == 15) {
        format = DRM_FORMAT_XRGB1555;
    } else if (req->bpp == 8 && req->depth == 8) {
        format = DRM_FORMAT_C8;
    } else {
        return -EINVAL;
    }

    if (req->width == 0 || req->height == 0) { return -EINVAL; }
    if (req->width > dev->mode_config.max_width || req->height > dev->mode_config.max_height) { return -EINVAL; }
    if (req->handle == 0) { return -EINVAL; }

    bytes_per_pixel = req->bpp / 8;
    if (req->pitch < req->width * bytes_per_pixel) { return -EINVAL; }

    obj = drm_gem_object_lookup(file_priv, req->handle);
    if (obj == NULL) { return -ENOENT; }
    if (obj->size < (size_t)req->pitch * req->height) {
        drm_gem_object_put(obj);
        return -EINVAL;
    }

    fb = malloc(sizeof(*fb));
    if (fb == NULL) {
        drm_gem_object_put(obj);
        return -ENOMEM;
    }
    memset(fb, 0, sizeof(*fb));

    fb->format      = format;
    fb->modifier    = DRM_FORMAT_MOD_LINEAR;
    fb->width       = req->width;
    fb->height      = req->height;
    fb->pitches[0]  = req->pitch;
    fb->obj[0]      = obj;
    fb->file        = file_priv;

    ret = drm_framebuffer_init(dev, fb, (dev->driver != NULL) ? dev->driver->fb_funcs : NULL);
    if (ret != 0) {
        drm_gem_object_put(obj);
        free(fb);
        return ret;
    }

    req->fb_id = (__u32)fb->base.id;
    return 0;
}

/*
 * DRM_IOCTL_MODE_ADDFB2.  The modern spelling: the caller names the format
 * directly and may supply up to four buffers for planar layouts.  This
 * driver scans out a single linear RGB plane, so multi-plane and tiled
 * layouts are refused rather than half-supported -- a framebuffer we
 * cannot describe correctly is worse than an error.
 */
int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd2 *req = (struct drm_mode_fb_cmd2 *)data;
    struct drm_framebuffer  *fb;
    struct drm_gem_object   *obj;
    uint32_t                 min_pitch;
    int                      i;
    int                      ret;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    if (req->pixel_format == DRM_FORMAT_INVALID) { return -EINVAL; }
    if ((req->flags & ~(DRM_MODE_FB_INTERLACED | DRM_MODE_FB_MODIFIERS)) != 0) { return -EINVAL; }
    if (req->pixel_format != DRM_FORMAT_XRGB8888 && req->pixel_format != DRM_FORMAT_ARGB8888) { return -EINVAL; }

    if (req->width == 0 || req->height == 0) { return -EINVAL; }
    if (req->width > dev->mode_config.max_width || req->height > dev->mode_config.max_height) { return -EINVAL; }

    min_pitch = req->width * 4;
    if (req->pitches[0] < min_pitch) { return -EINVAL; }

    if ((req->flags & DRM_MODE_FB_MODIFIERS) != 0) {
        if (req->modifier[0] != DRM_FORMAT_MOD_LINEAR) { return -EINVAL; }
        for (i = 1; i < 4; i++) {
            if (req->handles[i] != 0 || req->pitches[i] != 0 || req->offsets[i] != 0 || req->modifier[i] != 0) {
                return -EINVAL;
            }
        }
    } else {
        /* Without the flag, Linux ignores modifier[] entirely. */
        req->modifier[0] = DRM_FORMAT_MOD_LINEAR;
    }

    fb = malloc(sizeof(*fb));
    if (fb == NULL) { return -ENOMEM; }
    memset(fb, 0, sizeof(*fb));

    fb->format   = req->pixel_format;
    fb->modifier = req->modifier[0];
    fb->width    = req->width;
    fb->height   = req->height;
    fb->file     = file_priv;

    for (i = 0; i < 4; i++) {
        fb->pitches[i] = req->pitches[i];
        fb->offsets[i] = req->offsets[i];
    }

    ret = 0;
    for (i = 0; i < 1; i++) { /* single plane */
        uint32_t handle = req->handles[i];

        if (handle == 0) {
            ret = -EINVAL;
            goto out_release;
        }

        obj = drm_gem_object_lookup(file_priv, handle);
        if (obj == NULL) {
            ret = -ENOENT;
            goto out_release;
        }

        /* Offsets and pitch both have to fit inside the buffer, and the
         * arithmetic is done wide so a hostile pitch cannot wrap. */
        if (req->offsets[i] > obj->size
            || (req->height > 0
                && ((uint64_t)req->pitches[i] * (req->height - 1) + min_pitch > obj->size - req->offsets[i]))) {
            drm_gem_object_put(obj);
            ret = -EINVAL;
            goto out_release;
        }

        fb->obj[i] = obj;
    }

    ret = drm_framebuffer_init(dev, fb, (dev->driver != NULL) ? dev->driver->fb_funcs : NULL);
    if (ret != 0) { goto out_release; }

    req->fb_id = (__u32)fb->base.id;
    return 0;

out_release:
    for (i = 0; i < 4; i++) {
        if (fb->obj[i] != NULL) {
            drm_gem_object_put(fb->obj[i]);
            fb->obj[i] = NULL;
        }
    }
    free(fb);
    return ret;
}

/* --------------------------------------------------------------- teardown */

/*
 * DRM_IOCTL_MODE_RMFB.  Nothing may be scanning out of the framebuffer
 * when it goes: every plane still pointing at it is detached, and a
 * primary plane that is live gets a page flip to nothing first -- that
 * is the driver's chance to stop the hardware before the memory is freed.
 */
int drm_mode_rmfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    uint32_t                fb_id = *(uint32_t *)data;
    struct drm_framebuffer *fb;
    ilist_node_t           *node;

    (void)file_priv;

    if (dev == NULL || data == NULL) { return -EINVAL; }

    fb = drm_framebuffer_lookup(dev, file_priv, fb_id);
    if (fb == NULL) { return -ENOENT; }

    for (node = dev->mode_config.plane_list.next; node != &dev->mode_config.plane_list; node = node->next) {
        struct drm_plane *plane = container_of(node, struct drm_plane, head);

        if (plane->state == NULL || plane->state->fb != fb) { continue; }

        if (plane->state->crtc != NULL && plane == plane->state->crtc->primary) {
            struct drm_crtc              *crtc    = plane->state->crtc;
            struct drm_crtc_helper_funcs *helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            int                           ret;

            if (helpers == NULL || helpers->page_flip == NULL) { return -EBUSY; }
            ret = helpers->page_flip(crtc, NULL, NULL, 0);
            if (ret != 0) { return ret; }
        }

        plane->state->fb   = NULL;
        plane->state->crtc = NULL;
        plane->fb_id       = 0;
        plane->crtc_id     = 0;
    }

    drm_framebuffer_cleanup(fb);
    free(fb);

    return 0;
}

/* ------------------------------------------------------------------ queries */

/* DRM_IOCTL_MODE_GETFB: the old spelling, so bpp/depth come back instead
 * of a fourcc. */
int drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_cmd *req = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    fb = drm_framebuffer_lookup(dev, file_priv, req->fb_id);
    if (fb == NULL) { return -ENOENT; }

    req->width  = fb->width;
    req->height = fb->height;
    req->pitch  = fb->pitches[0];

    switch (fb->format) {
        case DRM_FORMAT_XRGB8888: req->bpp = 32; req->depth = 24; break;
        case DRM_FORMAT_ARGB8888: req->bpp = 32; req->depth = 32; break;
        case DRM_FORMAT_RGB888:   req->bpp = 24; req->depth = 24; break;
        case DRM_FORMAT_RGB565:   req->bpp = 16; req->depth = 16; break;
        case DRM_FORMAT_XRGB1555: req->bpp = 16; req->depth = 15; break;
        case DRM_FORMAT_C8:       req->bpp = 8;  req->depth = 8;  break;
        default:                  req->bpp = 32; req->depth = 24; break;
    }

    req->handle = 0;
    if (fb->obj[0] != NULL && drm_gem_handle_create(file_priv, fb->obj[0], &req->handle) != 0) { return -ENOMEM; }

    return 0;
}

/* DRM_IOCTL_MODE_GETFB2: the same, in fourcc and per-plane terms.  Every
 * buffer gets a handle in the calling file, so a failure part way through
 * has to give back the ones already handed out. */
int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_get_fb2 *req = (struct drm_mode_get_fb2 *)data;
    struct drm_framebuffer  *fb;
    int                      i, j;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    fb = drm_framebuffer_lookup(dev, file_priv, req->fb_id);
    if (fb == NULL) { return -ENOENT; }

    req->width        = fb->width;
    req->height       = fb->height;
    req->pixel_format = fb->format;
    req->flags        = DRM_MODE_FB_MODIFIERS;

    for (i = 0; i < 4; i++) {
        req->handles[i]  = 0;
        req->modifier[i] = (i == 0) ? fb->modifier : 0;
        req->pitches[i]  = fb->pitches[i];
        req->offsets[i]  = fb->offsets[i];

        if (fb->obj[i] == NULL) { continue; }
        if (drm_gem_handle_create(file_priv, fb->obj[i], &req->handles[i]) != 0) {
            for (j = 0; j < i; j++) {
                if (req->handles[j] != 0) { drm_gem_handle_delete(file_priv, req->handles[j]); }
            }
            return -ENOMEM;
        }
    }

    return 0;
}

/*
 * DRM_IOCTL_MODE_DIRTYFB: "these rectangles changed".  The annotation
 * variants (copy/fill) are validated here -- a copy annotation arrives as
 * pairs of equal-sized rectangles -- and the whole thing is handed to the
 * framebuffer's dirty callback.
 */
int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_fb_dirty_cmd *req = (struct drm_mode_fb_dirty_cmd *)data;
    struct drm_framebuffer       *fb;
    struct drm_clip_rect         *clips = NULL;
    unsigned int                  flags;
    int                           ret = 0;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    fb = drm_framebuffer_lookup(dev, file_priv, req->fb_id);
    if (fb == NULL) { return -ENOENT; }

    /* Either both a count and a pointer, or neither. */
    if ((req->num_clips == 0) != (req->clips_ptr == 0)) { return -EINVAL; }

    flags = req->flags & DRM_MODE_FB_DIRTY_FLAGS;
    if ((flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) != 0 && (req->num_clips & 1U) != 0) { return -EINVAL; }

    if (req->num_clips != 0) {
        if (req->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS) { return -EINVAL; }

        clips = malloc((size_t)req->num_clips * sizeof(*clips));
        if (clips == NULL) { return -ENOMEM; }
        if (copy_from_user(clips, (const void *)(uintptr_t)req->clips_ptr, (size_t)req->num_clips * sizeof(*clips)) != 0) {
            free(clips);
            return -EFAULT;
        }

        if ((flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) != 0) {
            for (uint32_t i = 0; i < req->num_clips; i += 2) {
                unsigned int src_w = clips[i].x2 - clips[i].x1;
                unsigned int src_h = clips[i].y2 - clips[i].y1;
                unsigned int dst_w = clips[i + 1].x2 - clips[i + 1].x1;
                unsigned int dst_h = clips[i + 1].y2 - clips[i + 1].y1;

                if (clips[i].x2 < clips[i].x1 || clips[i].y2 < clips[i].y1
                    || clips[i + 1].x2 < clips[i + 1].x1 || clips[i + 1].y2 < clips[i + 1].y1
                    || src_w != dst_w || src_h != dst_h) {
                    free(clips);
                    return -EINVAL;
                }
            }
        }
    }

    if (fb->funcs != NULL && fb->funcs->dirty != NULL) {
        ret = fb->funcs->dirty(fb, file_priv, flags, req->color, clips, req->num_clips);
    } else {
        ret = -ENOSYS;
    }

    free(clips);
    return ret;
}
