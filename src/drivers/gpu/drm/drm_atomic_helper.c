/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_atomic_helper.c - the steps a driver would otherwise write itself.
 * (GPLv2)
 *
 * drm_atomic.c decides *whether* a configuration may be applied; this file
 * is the collection of ordinary steps for applying one, in the order that
 * suits most hardware: take down what is going away, program the planes,
 * bring up what is arriving, wait for the frames to actually happen, then
 * release the old state.
 *
 * A driver with unusual requirements overrides the pieces it cares about
 * and keeps the rest.  Several of them are no-ops here because this kernel
 * has no vblank interrupt to wait on and no per-commit work to set up.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* Implemented in drm_atomic.c. */
extern struct drm_crtc_state      *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern struct drm_plane_state     *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane);
extern struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector);
extern int                         drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern int                         drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern void                        drm_atomic_state_free(struct drm_atomic_state *state);

/*
 * Work out what this commit is going to disturb.  Turning a CRTC on or off
 * necessarily re-times it, and moving a connector between CRTCs disturbs
 * both the one it left and the one it joined.
 */
int drm_atomic_helper_check_modeset(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc_state *crtc_state = state->crtcs[i].state;

        if (crtc_state == NULL) { continue; }

        if (crtc_state->active_changed) { crtc_state->mode_changed = true; }

        if (crtc_state->mode_changed) { DRM_DEBUG_KMS("CRTC %d: mode changed\n", i); }
        if (crtc_state->active_changed) {
            DRM_DEBUG_KMS("CRTC %d: active changed to %s\n", i, crtc_state->active ? "on" : "off");
        }
    }

    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];
        struct drm_connector       *connector  = state->connectors[i];
        struct drm_crtc            *old_crtc;
        struct drm_crtc            *new_crtc;

        if (conn_state == NULL || connector == NULL || connector->state == NULL) { continue; }
        if (conn_state->crtc == connector->state->crtc) { continue; }

        old_crtc = connector->state->crtc;
        new_crtc = conn_state->crtc;

        conn_state->crtc_changed = true;

        if (old_crtc != NULL) {
            struct drm_crtc_state *old_crtc_state = drm_atomic_get_crtc_state(state, old_crtc);

            if (old_crtc_state != NULL) { old_crtc_state->connectors_changed = true; }
        }

        if (new_crtc != NULL) {
            struct drm_crtc_state *new_crtc_state = drm_atomic_get_crtc_state(state, new_crtc);

            if (new_crtc_state != NULL) { new_crtc_state->connectors_changed = true; }
        }
    }

    return 0;
}

/* Every plane's framebuffer has to be a format that plane can scan out. */
int drm_atomic_helper_check_planes(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    if (state->planes == NULL) { return 0; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *entry       = &state->planes[i];
        struct drm_plane_state    *plane_state = entry->state;

        if (plane_state == NULL) { continue; }

        if (plane_state->fb != NULL) {
            struct drm_plane *plane = entry->ptr;
            bool              format_ok = false;

            if (plane == NULL || plane->format_types == NULL || plane->format_count == 0) {
                DRM_ERROR("Plane %d: no format list\n", i);
                return -EINVAL;
            }

            for (unsigned int j = 0; j < plane->format_count; j++) {
                if (plane->format_types[j] == plane_state->fb->format) {
                    format_ok = true;
                    break;
                }
            }

            if (!format_ok) {
                DRM_ERROR("Plane %d: format 0x%x not supported\n", i, plane_state->fb->format);
                return -EINVAL;
            }
        }

        plane_state->visible = (plane_state->fb != NULL && plane_state->crtc != NULL);
    }

    return 0;
}

void drm_atomic_helper_commit_modeset_disables(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *entry      = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = entry->state;

        if (crtc_state == NULL || entry->ptr == NULL) { continue; }

        if (crtc_state->active_changed && !crtc_state->active) {
            entry->ptr->enabled = false;
            DRM_DEBUG_KMS("CRTC %d: disabled\n", i);
        }
    }
}

void drm_atomic_helper_commit_modeset_enables(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *entry      = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = entry->state;

        if (crtc_state == NULL || entry->ptr == NULL) { continue; }

        if (crtc_state->active_changed && crtc_state->active) {
            entry->ptr->enabled = true;

            if (crtc_state->mode_changed) { memcpy(&entry->ptr->mode, &crtc_state->mode, sizeof(crtc_state->mode)); }

            DRM_DEBUG_KMS("CRTC %d: enabled\n", i);
        }
    }
}

void drm_atomic_helper_commit_planes(struct drm_device *dev, struct drm_atomic_state *state, uint32_t flags)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    (void)flags;

    if (state->planes == NULL) { return; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *entry       = &state->planes[i];
        struct drm_plane_state    *plane_state = entry->state;

        if (plane_state == NULL || entry->ptr == NULL) { continue; }
        if (plane_state->fb == NULL) { continue; }

        if (entry->ptr->state != NULL) {
            entry->ptr->state->fb      = plane_state->fb;
            entry->ptr->state->crtc    = plane_state->crtc;
            entry->ptr->state->src     = plane_state->src;
            entry->ptr->state->dst     = plane_state->dst;
            entry->ptr->state->visible = plane_state->visible;
        }
    }
}

int drm_atomic_helper_setup_commit(struct drm_atomic_state *state, bool nonblocking)
{
    (void)state;
    (void)nonblocking;

    /* No completion bookkeeping to prepare: events are armed in
     * drm_atomic.c when the commit runs. */
    return 0;
}

void drm_atomic_helper_wait_for_vblanks(struct drm_device *dev, struct drm_atomic_state *state)
{
    (void)dev;
    (void)state;

    /* Nothing to wait for without a real vblank interrupt. */
}

int drm_atomic_helper_wait_for_flip_done(struct drm_device *dev, struct drm_atomic_state *state)
{
    (void)dev;
    (void)state;

    return 0;
}

/* Release the before-and-after plane states this commit was carrying. */
void drm_atomic_helper_cleanup_planes(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    if (state->planes == NULL) { return; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *entry = &state->planes[i];

        free(entry->old_state);
        entry->old_state = NULL;

        free(entry->new_state);
        entry->new_state = NULL;
    }
}

/* The order most hardware wants: off, reprogram, on, settle, tidy up. */
void drm_atomic_helper_commit_tail(struct drm_atomic_state *state)
{
    struct drm_device *dev = state->dev;

    drm_atomic_helper_commit_modeset_disables(dev, state);
    drm_atomic_helper_commit_planes(dev, state, 0);
    drm_atomic_helper_commit_modeset_enables(dev, state);
    drm_atomic_helper_wait_for_vblanks(dev, state);
    drm_atomic_helper_cleanup_planes(dev, state);
}
