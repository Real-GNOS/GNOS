/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_modes.c - display modes: what a monitor can do, in our own words.
 * (GPLv2)
 *
 * A mode is a set of timings: pixel clock, the blanking intervals that
 * tell a monitor where a line and a frame start, and a name.  The kernel
 * keeps them as drm_display_mode (wide integers, a status field, list
 * membership) while user space sees drm_mode_modeinfo (narrow fixed-width
 * fields, no kernel bookkeeping).  Most of this file is the translation
 * between those two, plus the little bit of lifecycle plumbing around
 * allocating, naming and comparing them.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_fourcc.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* Implemented in drm_mode_object.c: hand an object its device-wide id. */
extern int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);

/* A fresh mode, registered with the device so it can be looked up by id. */
struct drm_display_mode *drm_mode_create(struct drm_device *dev)
{
    struct drm_display_mode *mode;

    if (dev == NULL) { return NULL; }

    mode = malloc(sizeof(*mode));
    if (mode == NULL) { return NULL; }
    memset(mode, 0, sizeof(*mode));

    if (drm_mode_object_idr_alloc(dev, &mode->base, DRM_MODE_OBJECT_MODE) != 0) {
        free(mode);
        return NULL;
    }

    return mode;
}

/* The reverse: take it off whatever list it is on, forget its id, free it. */
void drm_mode_destroy(struct drm_device *dev, struct drm_display_mode *mode)
{
    if (dev == NULL || mode == NULL) { return; }

    ilist_remove(&mode->head);

    spin_lock(&dev->mode_config.idr_mutex);
    drm_idr_remove(&dev->mode_config.object_idr, mode->base.id);
    spin_unlock(&dev->mode_config.idr_mutex);

    free(mode);
}

/* Add a mode found by probing to the connector that reported it.  The
 * count records how many connectors claim it, so a mode used by two of
 * them is not freed while the other still lists it. */
void drm_mode_probed_add(struct drm_connector *connector, struct drm_display_mode *mode)
{
    if (connector == NULL || mode == NULL) { return; }

    ilist_insert_after(&connector->modes, &mode->head);
    mode->connector_count++;
}

void drm_mode_copy(struct drm_display_mode *dst, const struct drm_display_mode *src)
{
    if (dst == NULL || src == NULL) { return; }

    memcpy(dst, src, sizeof(*dst));
}

/*
 * Two modes are the same mode when they describe the same picture: the same
 * clock, the same visible area, the same sync polarity and type.  Names are
 * decoration and deliberately not compared -- a driver and a monitor
 * routinely disagree about what to call 1920x1080.
 */
bool drm_mode_equal(const struct drm_display_mode *mode1, const struct drm_display_mode *mode2)
{
    if (mode1 == NULL || mode2 == NULL) { return false; }

    if (mode1->clock != mode2->clock) { return false; }
    if (mode1->hdisplay != mode2->hdisplay || mode1->vdisplay != mode2->vdisplay) { return false; }
    if (mode1->flags != mode2->flags || mode1->type != mode2->type) { return false; }

    return true;
}

/*
 * Kernel view from the UAPI struct.  No id is allocated: modes arriving
 * from user space are candidates to be checked, not objects to be looked
 * up yet.
 */
struct drm_display_mode *drm_convert_umode(const struct drm_mode_modeinfo *umode)
{
    struct drm_display_mode *mode;

    if (umode == NULL) { return NULL; }

    mode = malloc(sizeof(*mode));
    if (mode == NULL) { return NULL; }
    memset(mode, 0, sizeof(*mode));

    mode->clock       = (int)umode->clock;
    mode->hdisplay    = (int)umode->hdisplay;
    mode->hsync_start = (int)umode->hsync_start;
    mode->hsync_end   = (int)umode->hsync_end;
    mode->htotal      = (int)umode->htotal;
    mode->hskew       = (int)umode->hskew;
    mode->vdisplay    = (int)umode->vdisplay;
    mode->vsync_start = (int)umode->vsync_start;
    mode->vsync_end   = (int)umode->vsync_end;
    mode->vtotal      = (int)umode->vtotal;
    mode->vscan       = (int)umode->vscan;
    mode->vrefresh    = (int)umode->vrefresh;
    mode->flags       = umode->flags;
    mode->type        = umode->type;
    mode->status      = MODE_OK;
    mode->connector_count = 0;

    strncpy(mode->name, umode->name, DRM_DISPLAY_MODE_LEN - 1);
    mode->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';

    return mode;
}

/* UAPI view of a kernel mode: drop the bookkeeping, keep the timings. */
void drm_convert_to_umode(struct drm_mode_modeinfo *out, const struct drm_display_mode *in)
{
    if (out == NULL || in == NULL) { return; }

    memset(out, 0, sizeof(*out));

    out->clock       = (__u32)in->clock;
    out->hdisplay    = (__u16)in->hdisplay;
    out->hsync_start = (__u16)in->hsync_start;
    out->hsync_end   = (__u16)in->hsync_end;
    out->htotal      = (__u16)in->htotal;
    out->hskew       = (__u16)in->hskew;
    out->vdisplay    = (__u16)in->vdisplay;
    out->vsync_start = (__u16)in->vsync_start;
    out->vsync_end   = (__u16)in->vsync_end;
    out->vtotal      = (__u16)in->vtotal;
    out->vscan       = (__u16)in->vscan;
    out->vrefresh    = (__u32)in->vrefresh;
    out->flags       = in->flags;
    out->type        = in->type;

    strncpy(out->name, in->name, DRM_DISPLAY_MODE_LEN - 1);
    out->name[DRM_DISPLAY_MODE_LEN - 1] = '\0';
}

/* Print it the way xorg.conf spells a modeline. */
void drm_mode_debug_printmodeline(const struct drm_display_mode *mode)
{
    if (mode == NULL) {
        DRM_DEBUG_KMS("modeline: (null)\n");
        return;
    }

    DRM_DEBUG_KMS("modeline \"%s\": %d %d %d %d %d %d %d %d %d 0x%x 0x%x\n", mode->name, mode->clock, mode->hdisplay,
                  mode->hsync_start, mode->hsync_end, mode->htotal, mode->vdisplay, mode->vsync_start, mode->vsync_end,
                  mode->vtotal, mode->flags, mode->type);
}
