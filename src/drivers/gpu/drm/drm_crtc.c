/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_crtc.c - the hardware that reads a framebuffer and makes a signal.
 * (GPLv2)
 *
 * A CRTC is the scanout engine: it reads pixels out of whatever its primary
 * plane points at, at the rate a mode describes, and feeds the result to an
 * encoder.  Everything else in KMS exists to decide three things for it --
 * which mode, which framebuffer, which connectors -- and both SETCRTC and
 * the atomic path end up expressing exactly that.
 *
 * SETCRTC here is deliberately built as an atomic state and committed like
 * any other, rather than programming registers directly: one validation
 * path, one completion path, and no second set of rules to keep in step.
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

/* From drm_mode_object.c / drm_framebuffer.c. */
extern int                     drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);
extern struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file_priv, uint32_t id);

int drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc, struct drm_plane *primary,
                              struct drm_plane *cursor, void *funcs, const char *name)
{
    int ret;

    (void)name;

    if (dev == NULL || crtc == NULL) { return -EINVAL; }

    ret = drm_mode_object_idr_alloc(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);
    if (ret != 0) { return ret; }

    drm_modeset_lock_init(&crtc->mutex);

    memset(&crtc->commit_lock, 0, sizeof(crtc->commit_lock));
    memset(&crtc->spinlock, 0, sizeof(crtc->spinlock));

    crtc->dev            = dev;
    crtc->primary        = primary;
    crtc->cursor         = cursor;
    crtc->legacy_cursor  = NULL;
    crtc->cursor_obj     = NULL;
    crtc->mode_config    = &dev->mode_config;
    crtc->index          = dev->mode_config.num_crtc++;
    crtc->enabled        = false;
    crtc->gamma_size     = 256;
    crtc->gamma_store    = NULL;
    crtc->state          = NULL;
    crtc->commit_state   = NULL;
    crtc->helper_private = funcs;
    crtc->x              = 0;
    crtc->y              = 0;

    memset(&crtc->mode, 0, sizeof(crtc->mode));
    memset(&crtc->saved_mode, 0, sizeof(crtc->saved_mode));

    ilist_insert_after(&dev->mode_config.crtc_list, &crtc->head);

    /* ACTIVE and MODE_ID are what an atomic commit sets on a CRTC; the
     * rest of the standard set is still to come. */
    ret = drm_object_attach_property(&crtc->base, dev->mode_config.prop_active, 0);
    if (ret == 0) { ret = drm_object_attach_property(&crtc->base, dev->mode_config.prop_mode_id, 0); }
    if (ret != 0) {
        drm_crtc_cleanup(crtc);
        return ret;
    }

    return 0;
}

int drm_crtc_create_properties(struct drm_device *dev)
{
    if (dev == NULL) { return -EINVAL; }

    /* The properties a CRTC needs are created once for the device in
     * drm_mode_config_init and attached per CRTC above, so there is
     * nothing left to do here. */
    return 0;
}

/* Record a mode as the CRTC's current one and switch it on. */
void drm_crtc_set_mode_prop_for_crtc(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
    if (crtc == NULL || mode == NULL) { return; }

    memcpy(&crtc->mode, mode, sizeof(crtc->mode));
    crtc->enabled = true;
}

/* DRM_IOCTL_MODE_GETCRTC: the current mode, position and framebuffer. */
int drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc   *req = (struct drm_mode_crtc *)data;
    struct drm_mode_object *obj;
    struct drm_crtc        *crtc;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    dbg_puts("PSETCRTC id=");
    dbg_puts_dec((uint32_t)req->crtc_id);
    dbg_puts(" fb=");
    dbg_puts_dec((uint32_t)req->fb_id);
    dbg_puts(" mv=");
    dbg_puts_dec((uint32_t)req->mode_valid);
    dbg_puts("\n");

    obj = drm_mode_object_find(dev, file_priv, req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (obj == NULL) {
        dbg_puts("PSETCRTC: object_find miss\n");
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    req->fb_id      = (crtc->primary != NULL) ? crtc->primary->fb_id : 0;
    req->x          = (__u32)crtc->x;
    req->y          = (__u32)crtc->y;
    req->gamma_size = crtc->gamma_size;
    req->mode_valid = crtc->enabled ? 1 : 0;

    /* The internal mode carries fields user space has no business seeing,
     * so it is converted rather than copied. */
    req->mode.clock       = (__u32)crtc->mode.clock;
    req->mode.hdisplay    = (__u16)crtc->mode.hdisplay;
    req->mode.hsync_start = (__u16)crtc->mode.hsync_start;
    req->mode.hsync_end   = (__u16)crtc->mode.hsync_end;
    req->mode.htotal      = (__u16)crtc->mode.htotal;
    req->mode.hskew       = (__u16)crtc->mode.hskew;
    req->mode.vdisplay    = (__u16)crtc->mode.vdisplay;
    req->mode.vsync_start = (__u16)crtc->mode.vsync_start;
    req->mode.vsync_end   = (__u16)crtc->mode.vsync_end;
    req->mode.vtotal      = (__u16)crtc->mode.vtotal;
    req->mode.vscan       = (__u16)crtc->mode.vscan;
    req->mode.vrefresh    = (__u32)crtc->mode.vrefresh;
    req->mode.flags       = crtc->mode.flags;
    req->mode.type        = crtc->mode.type;
    strncpy(req->mode.name, crtc->mode.name, DRM_DISPLAY_MODE_LEN - 1);
    req->mode.name[DRM_DISPLAY_MODE_LEN - 1] = '\0';

    drm_mode_object_put(obj);
    return 0;
}

/*
 * DRM_IOCTL_MODE_SETCRTC.  With mode_valid set this is "show this
 * framebuffer, in this mode, on these connectors"; without it, "turn the
 * CRTC off".  The whole thing becomes one atomic commit so that a failure
 * part way through cannot leave the display half configured.
 */
int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc    *req = (struct drm_mode_crtc *)data;
    struct drm_mode_object  *obj;
    struct drm_crtc         *crtc;
    struct drm_framebuffer  *fb   = NULL;
    struct drm_atomic_state *state = NULL;
    struct drm_crtc_state   *crtc_state;
    struct drm_plane_state  *plane_state;
    struct drm_display_mode  mode;
    uint32_t                *connector_ids = NULL;
    int                      ret           = 0;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    dbg_puts("PSETCRTC id=");
    dbg_puts_dec((uint32_t)req->crtc_id);
    dbg_puts(" fb=");
    dbg_puts_dec((uint32_t)req->fb_id);
    dbg_puts(" mv=");
    dbg_puts_dec((uint32_t)req->mode_valid);
    dbg_puts("\n");

    obj = drm_mode_object_find(dev, file_priv, req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (obj == NULL) {
        dbg_puts("PSETCRTC: object_find miss\n");
        return -ENOENT;
    }
    crtc = container_of(obj, struct drm_crtc, base);

    if (req->fb_id != 0) {
        fb = drm_framebuffer_lookup(dev, file_priv, req->fb_id);
        if (fb == NULL) {
            dbg_puts("PSETCRTC: fb lookup miss\n");
            drm_mode_object_put(obj);
            return -ENOENT;
        }
    }

    if (req->mode_valid != 0) {
        /* A mode has to be a mode: a clock, a visible area, and timings
         * that run forwards. */
        if (fb == NULL || req->mode.clock == 0 || req->mode.hdisplay == 0 || req->mode.vdisplay == 0) {
            ret = -EINVAL;
            goto out;
        }
        if (req->mode.hsync_start > req->mode.hsync_end || req->mode.hsync_end > req->mode.htotal) {
            ret = -EINVAL;
            goto out;
        }
        if (req->mode.vsync_start > req->mode.vsync_end || req->mode.vsync_end > req->mode.vtotal) {
            ret = -EINVAL;
            goto out;
        }
        if (req->mode.htotal == 0 || req->mode.vtotal == 0) {
            ret = -EINVAL;
            goto out;
        }
        if (req->mode.hdisplay > dev->mode_config.max_width || req->mode.vdisplay > dev->mode_config.max_height) {
            ret = -EINVAL;
            goto out;
        }
        /* Position plus size has to stay inside signed 32 bits too. */
        if (req->x > DRM_S32_MAX || req->y > DRM_S32_MAX
            || (uint64_t)req->x + req->mode.hdisplay > DRM_S32_MAX
            || (uint64_t)req->y + req->mode.vdisplay > DRM_S32_MAX) {
            ret = -EINVAL;
            goto out;
        }

        if (req->count_connectors > (uint32_t)dev->mode_config.num_connector
            || (req->count_connectors != 0 && req->set_connectors_ptr == 0)) {
            ret = -EINVAL;
            goto out;
        }
        if (req->count_connectors != 0) {
            connector_ids = malloc((size_t)req->count_connectors * sizeof(*connector_ids));
            if (connector_ids == NULL) {
                ret = -ENOMEM;
                goto out;
            }
            if (copy_from_user(connector_ids, (const void *)(uintptr_t)req->set_connectors_ptr,
                               (size_t)req->count_connectors * sizeof(*connector_ids)) != 0) {
                ret = -EFAULT;
                goto out;
            }
        }

        memset(&mode, 0, sizeof(mode));
        mode.clock       = (int)req->mode.clock;
        mode.hdisplay    = (int)req->mode.hdisplay;
        mode.hsync_start = (int)req->mode.hsync_start;
        mode.hsync_end   = (int)req->mode.hsync_end;
        mode.htotal      = (int)req->mode.htotal;
        mode.hskew       = (int)req->mode.hskew;
        mode.vdisplay    = (int)req->mode.vdisplay;
        mode.vsync_start = (int)req->mode.vsync_start;
        mode.vsync_end   = (int)req->mode.vsync_end;
        mode.vtotal      = (int)req->mode.vtotal;
        mode.vscan       = (int)req->mode.vscan;
        mode.vrefresh    = (int)req->mode.vrefresh;
        mode.flags       = req->mode.flags;
        mode.type        = req->mode.type;
        mode.status      = MODE_OK;
        strncpy(mode.name, req->mode.name, DRM_DISPLAY_MODE_LEN - 1);
    } else if (req->count_connectors != 0) {
        /* Switching off while naming connectors makes no sense. */
        ret = -EINVAL;
        goto out;
    }

    state = drm_atomic_state_alloc(dev);
    if (state == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    state->allow_modeset = 1;
    state->file_priv     = file_priv;

    crtc_state = drm_atomic_get_crtc_state(state, crtc);
    if (crtc_state == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    crtc_state->active         = req->mode_valid;
    crtc_state->enable         = req->mode_valid;
    crtc_state->active_changed = (crtc_state->active != 0) != (crtc->enabled != 0);
    crtc_state->mode_changed   = true;
    if (req->mode_valid != 0) { crtc_state->mode = mode; }

    /* The primary plane is what actually shows the framebuffer, so it gets
     * the source rectangle (the whole buffer, 16.16) and the destination
     * (the mode's size at the CRTC's position). */
    if (crtc->primary != NULL) {
        plane_state = drm_atomic_get_plane_state(state, crtc->primary);
        if (plane_state == NULL) {
            ret = -ENOMEM;
            goto out;
        }

        plane_state->crtc = (req->mode_valid != 0) ? crtc : NULL;
        plane_state->fb   = (req->mode_valid != 0) ? fb : NULL;
        plane_state->src  = (struct drm_rect) {0, 0, (req->mode_valid != 0) ? (int32_t)(fb->width << 16) : 0,
                                               (req->mode_valid != 0) ? (int32_t)(fb->height << 16) : 0};
        plane_state->dst  = (struct drm_rect) {(int32_t)req->x, (int32_t)req->y,
                                               (req->mode_valid != 0) ? (int32_t)(req->x + mode.hdisplay) : 0,
                                               (req->mode_valid != 0) ? (int32_t)(req->y + mode.vdisplay) : 0};
        crtc_state->planes_changed = true;
    }

    /* Every named connector must exist, and must be named only once. */
    for (uint32_t i = 0; i < req->count_connectors; i++) {
        struct drm_mode_object *conn_obj;

        for (uint32_t j = 0; j < i; j++) {
            if (connector_ids[i] == connector_ids[j]) {
                ret = -EINVAL;
                goto out;
            }
        }

        conn_obj = drm_mode_object_find(dev, file_priv, connector_ids[i], DRM_MODE_OBJECT_CONNECTOR);
        if (conn_obj == NULL) {
            ret = -ENOENT;
            goto out;
        }
        drm_mode_object_put(conn_obj);
    }

    /* Rewire the connectors: away from this CRTC if they were on it and
     * are not named, onto it if they are. */
    for (ilist_node_t *node = dev->mode_config.connector_list.next; node != &dev->mode_config.connector_list;
         node = node->next) {
        struct drm_connector       *connector = container_of(node, struct drm_connector, head);
        struct drm_connector_state *conn_state;
        bool                        selected = false;

        for (uint32_t i = 0; i < req->count_connectors; i++) {
            if (connector_ids[i] == connector->base.id) {
                selected = true;
                break;
            }
        }

        if (!selected && (connector->state == NULL || connector->state->crtc != crtc)) { continue; }

        conn_state = drm_atomic_get_connector_state(state, connector);
        if (conn_state == NULL) {
            ret = -ENOMEM;
            goto out;
        }
        conn_state->crtc               = selected ? crtc : NULL;
        conn_state->crtc_changed       = true;
        crtc_state->connectors_changed = true;
    }

    ret = drm_atomic_commit(state);
    if (ret == 0) { state = NULL; /* ownership passed to the commit */ }

out:
    if (state != NULL) { drm_atomic_state_free(state); }
    free(connector_ids);
    drm_mode_object_put(obj);
    return ret;
}

void drm_crtc_cleanup(struct drm_crtc *crtc)
{
    struct drm_device *dev;

    if (crtc == NULL) { return; }

    dev = crtc->dev;

    ilist_remove(&crtc->head);

    if (dev != NULL) {
        spin_lock(&dev->mode_config.idr_mutex);
        drm_idr_remove(&dev->mode_config.object_idr, crtc->base.id);
        spin_unlock(&dev->mode_config.idr_mutex);

        if (dev->mode_config.num_crtc > 0) { dev->mode_config.num_crtc--; }
    }

    free(crtc->gamma_store);
    crtc->gamma_store = NULL;

    if (crtc->cursor_obj != NULL) {
        drm_gem_object_put(crtc->cursor_obj);
        crtc->cursor_obj = NULL;
    }

    if (crtc->base.properties != NULL) {
        drm_property_set_destroy(crtc->base.properties);
        free(crtc->base.properties);
        crtc->base.properties = NULL;
    }
}

/* ------------------------------------------------------------------ gamma */

/* Software display: there is no hardware LUT, but clients (and modesetting's
 * initial CRTC restore) still expect the ioctls to work, so the tables are
 * stored per CRTC and served back.  Xorg diffs against its saved table, so
 * an accurate echo is what keeps it from re-programming every repaint. */
#define GAMMA_ENTRIES 256

typedef struct {
    uint32_t id;                       /* crtc object id, 0 = unused */
    uint16_t red[GAMMA_ENTRIES];
    uint16_t green[GAMMA_ENTRIES];
    uint16_t blue[GAMMA_ENTRIES];
} gamma_lut_t;

static gamma_lut_t       g_gamma_luts[4];
static spinlock_t        g_gamma_lock;

static gamma_lut_t *gamma_lut_for(struct drm_device *dev, uint32_t crtc_id)
{
    gamma_lut_t *slot = NULL;

    for (int i = 0; i < 4; i++) {
        if (g_gamma_luts[i].id == crtc_id)
            return &g_gamma_luts[i];
        if (slot == NULL && g_gamma_luts[i].id == 0)
            slot = &g_gamma_luts[i];
    }
    if (slot == NULL)
        return NULL;
    slot->id = crtc_id;
    return slot;
}

static int gamma_copy_in(gamma_lut_t *dst, const struct drm_mode_crtc_lut *req)
{
    if (req->gamma_size != GAMMA_ENTRIES)
        return -EINVAL;
    if (req->red == 0 || req->green == 0 || req->blue == 0)
        return -EINVAL;
    if (copy_from_user(dst->red, (const void *)(uintptr_t)req->red,
                       sizeof(dst->red)) != 0 ||
        copy_from_user(dst->green, (const void *)(uintptr_t)req->green,
                       sizeof(dst->green)) != 0 ||
        copy_from_user(dst->blue, (const void *)(uintptr_t)req->blue,
                       sizeof(dst->blue)) != 0)
        return -EFAULT;
    return 0;
}

/*
 * DRM_IOCTL_MODE_SETGAMMA.  Validates against the CRTC's advertised
 * gamma_size (256) and stores the table.
 */
int drm_mode_setgamma_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc_lut *req = (struct drm_mode_crtc_lut *)data;
    struct drm_mode_object   *obj;
    struct drm_crtc          *crtc;
    gamma_lut_t              *lut;
    int                       ret;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (obj == NULL) { return -ENOENT; }
    crtc = container_of(obj, struct drm_crtc, base);

    if (req->gamma_size != (uint32_t)crtc->gamma_size) {
        drm_mode_object_put(obj);
        return -EINVAL;
    }

    spin_lock(&g_gamma_lock);
    lut = gamma_lut_for(dev, req->crtc_id);
    ret = (lut != NULL) ? gamma_copy_in(lut, req) : -ENOMEM;
    spin_unlock(&g_gamma_lock);

    drm_mode_object_put(obj);
    return ret;
}

/*
 * DRM_IOCTL_MODE_GETGAMMA.  Returns the stored table, or an identity ramp
 * if the CRTC's gamma was never programmed -- that is what the hardware
 * would have been showing.
 */
int drm_mode_getgamma_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_mode_crtc_lut *req = (struct drm_mode_crtc_lut *)data;
    struct drm_mode_object   *obj;
    struct drm_crtc          *crtc;
    gamma_lut_t              *lut;

    (void)file_priv;

    if (dev == NULL || req == NULL) { return -EINVAL; }

    obj = drm_mode_object_find(dev, file_priv, req->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (obj == NULL) { return -ENOENT; }
    crtc = container_of(obj, struct drm_crtc, base);

    if (req->gamma_size != (uint32_t)crtc->gamma_size ||
        req->red == 0 || req->green == 0 || req->blue == 0) {
        drm_mode_object_put(obj);
        return -EINVAL;
    }

    spin_lock(&g_gamma_lock);
    lut = gamma_lut_for(dev, req->crtc_id);
    if (lut == NULL || lut->red[0] == 0) {
        /* Identity ramp: entry i reads i scaled to 16 bit. */
        for (int i = 0; i < GAMMA_ENTRIES; i++) {
            uint16_t v = (uint16_t)((i * 0xFFFF) / (GAMMA_ENTRIES - 1));
            req->gamma_size = (uint32_t)GAMMA_ENTRIES;
            /* build the ramp in the stored slot so future reads are cheap */
            if (lut == NULL)
                break;
            lut->red[i] = lut->green[i] = lut->blue[i] = v;
        }
        if (lut != NULL) {
            if (copy_to_user((void *)(uintptr_t)req->red, lut->red,
                             sizeof(lut->red)) != 0 ||
                copy_to_user((void *)(uintptr_t)req->green, lut->green,
                             sizeof(lut->green)) != 0 ||
                copy_to_user((void *)(uintptr_t)req->blue, lut->blue,
                             sizeof(lut->blue)) != 0) {
                spin_unlock(&g_gamma_lock);
                drm_mode_object_put(obj);
                return -EFAULT;
            }
        }
        spin_unlock(&g_gamma_lock);
        drm_mode_object_put(obj);
        return 0;
    }

    if (copy_to_user((void *)(uintptr_t)req->red, lut->red,
                     sizeof(lut->red)) != 0 ||
        copy_to_user((void *)(uintptr_t)req->green, lut->green,
                     sizeof(lut->green)) != 0 ||
        copy_to_user((void *)(uintptr_t)req->blue, lut->blue,
                     sizeof(lut->blue)) != 0) {
        spin_unlock(&g_gamma_lock);
        drm_mode_object_put(obj);
        return -EFAULT;
    }
    spin_unlock(&g_gamma_lock);
    drm_mode_object_put(obj);
    return 0;
}
