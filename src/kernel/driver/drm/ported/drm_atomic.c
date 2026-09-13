/*
 * drm_atomic.c - changing the whole display configuration at once. (GPLv2)
 *
 * The old way of setting up a display was a series of ioctls, each of which
 * had to be legal on its own -- which meant the display passed through
 * states nobody wanted on the way to the one they did.  The atomic
 * interface inverts that: a client describes an entire configuration in one
 * drm_atomic_state, the kernel validates the whole thing, and only then is
 * any of it applied.
 *
 * A state is three parallel arrays (one slot per CRTC, per plane, plus a
 * growable one for connectors) holding the *proposed* state of each object,
 * empty until somebody asks for that object -- so a commit that touches one
 * plane does not have to describe the other nine.
 *
 * Commits are serialised by a ticket lock: each takes the next sequence
 * number and waits its turn, so two racing commits cannot interleave their
 * hardware programming.
 */

#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_devtmpfs.h"
#include "drm_idr.h"
#include "drm_mode.h"
#include "drm_modeset_lock.h"
#include "drm_print.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* ---------------------------------------------------------------- lifecycle */

struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev)
{
    struct drm_atomic_state *state;
    struct drm_mode_config  *config = &dev->mode_config;

    state = malloc(sizeof(*state));
    if (state == NULL) { return NULL; }
    memset(state, 0, sizeof(*state));
    state->dev = dev;

    drm_modeset_acquire_init(&state->acquire_ctx, 0);

    if (config->num_total_plane > 0) {
        state->planes = malloc(sizeof(*state->planes) * config->num_total_plane);
        if (state->planes == NULL) {
            free(state);
            return NULL;
        }
        memset(state->planes, 0, sizeof(*state->planes) * config->num_total_plane);
    }

    if (config->num_crtc > 0) {
        state->crtcs = malloc(sizeof(*state->crtcs) * config->num_crtc);
        if (state->crtcs == NULL) {
            free(state->planes);
            free(state);
            return NULL;
        }
        memset(state->crtcs, 0, sizeof(*state->crtcs) * config->num_crtc);
    }

    return state;
}

void drm_atomic_state_default_clear(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    if (state->planes != NULL) {
        for (i = 0; i < config->num_total_plane; i++) {
            free(state->planes[i].state);
            state->planes[i].state = NULL;
            free(state->planes[i].old_state);
            state->planes[i].old_state = NULL;
            free(state->planes[i].new_state);
            state->planes[i].new_state = NULL;
        }
        free(state->planes);
        state->planes = NULL;
    }

    if (state->crtcs != NULL) {
        for (i = 0; i < config->num_crtc; i++) {
            if (state->crtcs[i].state != NULL) {
                free(state->crtcs[i].state->event);
                free(state->crtcs[i].state);
                state->crtcs[i].state = NULL;
            }
            free(state->crtcs[i].old_state);
            state->crtcs[i].old_state = NULL;
            free(state->crtcs[i].new_state);
            state->crtcs[i].new_state = NULL;
        }
        free(state->crtcs);
        state->crtcs = NULL;
    }

    if (state->connector_states != NULL) {
        for (i = 0; i < state->num_connector; i++) {
            free(state->connector_states[i]);
            state->connector_states[i] = NULL;
        }
        free(state->connector_states);
        state->connector_states = NULL;
    }

    free(state->connectors);
    state->connectors = NULL;

    state->num_connector = 0;
}

void drm_atomic_state_clear(struct drm_atomic_state *state)
{
    drm_atomic_state_default_clear(state);

    state->allow_modeset        = 0;
    state->legacy_cursor_update = 0;
    state->async_update         = 0;
    state->duplicated           = 0;
}

void drm_atomic_state_free(struct drm_atomic_state *state)
{
    if (state == NULL) { return; }

    drm_atomic_state_default_clear(state);
    drm_modeset_acquire_fini(&state->acquire_ctx);
    free(state);
}

/* --------------------------------------------------------- per-object states */

/*
 * The proposed state for @crtc, created on first ask and seeded from its
 * current state so that a client only has to say what it wants changed.
 */
struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct __drm_crtcs_state *entry = &state->crtcs[crtc->index];

    if (entry->state != NULL) { return entry->state; }

    entry->state = malloc(sizeof(*entry->state));
    if (entry->state == NULL) { return NULL; }
    memset(entry->state, 0, sizeof(*entry->state));

    entry->state->crtc = crtc;
    entry->ptr         = crtc;

    if (crtc->state != NULL) { memcpy(entry->state, crtc->state, sizeof(*entry->state)); }

    return entry->state;
}

struct drm_plane_state *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane)
{
    struct drm_mode_config    *config = &state->dev->mode_config;
    struct __drm_planes_state *entry;
    ilist_node_t              *node;
    int                        idx = -1;
    int                        i   = 0;

    /* Planes are addressed by their position on the device's plane list. */
    for (node = config->plane_list.next; node != &config->plane_list; node = node->next, i++) {
        if (container_of(node, struct drm_plane, head) == plane) {
            idx = i;
            break;
        }
    }

    if (idx < 0 || idx >= config->num_total_plane) { return NULL; }

    entry = &state->planes[idx];

    if (entry->state != NULL) { return entry->state; }

    entry->state = malloc(sizeof(*entry->state));
    if (entry->state == NULL) { return NULL; }
    memset(entry->state, 0, sizeof(*entry->state));

    entry->state->plane = plane;
    entry->ptr          = plane;

    if (plane->state != NULL) { memcpy(entry->state, plane->state, sizeof(*entry->state)); }

    return entry->state;
}

struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector)
{
    struct drm_connector       **connectors;
    struct drm_connector_state **states;
    size_t                       count;
    int                          i;

    for (i = 0; i < state->num_connector; i++) {
        if (state->connectors[i] == connector) { return state->connector_states[i]; }
    }

    count = (size_t)state->num_connector + 1;

    connectors = realloc(state->connectors, sizeof(*connectors) * count);
    if (connectors == NULL) { return NULL; }
    state->connectors = connectors;

    states = realloc(state->connector_states, sizeof(*states) * count);
    if (states == NULL) {
        /* Do not leave the connector array claiming a slot whose state does
         * not exist -- shrink it back to what is actually populated. */
        state->connectors = realloc(state->connectors, sizeof(*connectors) * (size_t)state->num_connector);
        return NULL;
    }
    state->connector_states = states;

    state->connectors[state->num_connector] = connector;

    state->connector_states[state->num_connector] = malloc(sizeof(*state->connector_states[0]));
    if (state->connector_states[state->num_connector] == NULL) { return NULL; }
    memset(state->connector_states[state->num_connector], 0, sizeof(*state->connector_states[0]));
    state->connector_states[state->num_connector]->connector = connector;

    if (connector->state != NULL) {
        memcpy(state->connector_states[state->num_connector], connector->state, sizeof(*state->connector_states[0]));
    }

    state->num_connector++;
    return state->connector_states[state->num_connector - 1];
}

/* ---------------------------------------------------------------- helpers */

/* Pull in every plane that could end up on @crtc, so that turning a CRTC
 * off or changing its mode is checked against all of them. */
int drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_mode_config *config   = &state->dev->mode_config;
    uint32_t                crtc_bit = 1U << crtc->index;
    ilist_node_t           *node;

    for (node = config->plane_list.next; node != &config->plane_list; node = node->next) {
        struct drm_plane *plane = container_of(node, struct drm_plane, head);

        if ((plane->possible_crtcs & crtc_bit) != 0 && drm_atomic_get_plane_state(state, plane) == NULL) {
            return -ENOMEM;
        }
    }

    return 0;
}

int drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_mode_config *config = &state->dev->mode_config;
    ilist_node_t           *node;

    (void)crtc;

    for (node = config->connector_list.next; node != &config->connector_list; node = node->next) {
        struct drm_connector *connector = container_of(node, struct drm_connector, head);

        if (drm_atomic_get_connector_state(state, connector) == NULL) { return -ENOMEM; }
    }

    return 0;
}

/* -------------------------------------------------------------- validation */

/*
 * Say whether @state is legal.  Nothing here touches hardware: the point is
 * that a rejected commit leaves the display exactly as it was, instead of
 * half changed.
 */
int drm_atomic_check_only(struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &state->dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc_state *crtc_state = state->crtcs[i].state;

        if (crtc_state == NULL) { continue; }

        /* Changing a mode or the active flag is a modeset, and a client
         * that did not ask for one may not cause one. */
        if (!state->allow_modeset && (crtc_state->mode_changed || crtc_state->active_changed)) { return -EINVAL; }

        if (crtc_state->active && crtc_state->mode.clock == 0 && crtc_state->mode.hdisplay == 0) {
            DRM_ERROR("CRTC %d: active but no mode set\n", i);
            return -EINVAL;
        }
    }

    if (state->planes != NULL) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct drm_plane_state *plane_state = state->planes[i].state;

            if (plane_state == NULL) { continue; }

            if (plane_state->fb != NULL) {
                bool format_ok = false;

                if (plane_state->plane->format_types == NULL || plane_state->plane->format_count == 0) {
                    DRM_ERROR("Plane %d: fb set but no format list\n", i);
                    return -EINVAL;
                }

                for (unsigned int j = 0; j < plane_state->plane->format_count; j++) {
                    if (plane_state->plane->format_types[j] == plane_state->fb->format) {
                        format_ok = true;
                        break;
                    }
                }
                if (!format_ok) {
                    DRM_ERROR("Plane %d: incompatible fb format\n", i);
                    return -EINVAL;
                }
            }

            /* A plane shows something somewhere, or nothing at all. */
            if ((plane_state->fb != NULL) != (plane_state->crtc != NULL)) { return -EINVAL; }

            plane_state->visible = (plane_state->fb != NULL && plane_state->crtc != NULL);

            if (plane_state->fb != NULL) {
                int64_t fb_w = (int64_t)plane_state->fb->width << 16;
                int64_t fb_h = (int64_t)plane_state->fb->height << 16;

                /* Source is 16.16 fixed point and must lie inside the
                 * buffer; destination must have a positive area. */
                if (plane_state->src.x1 < 0 || plane_state->src.y1 < 0
                    || plane_state->src.x2 <= plane_state->src.x1 || plane_state->src.y2 <= plane_state->src.y1
                    || plane_state->src.x2 > fb_w || plane_state->src.y2 > fb_h
                    || plane_state->dst.x2 <= plane_state->dst.x1
                    || plane_state->dst.y2 <= plane_state->dst.y1) {
                    return -EINVAL;
                }

                if ((plane_state->plane->possible_crtcs & (1U << plane_state->crtc->index)) == 0) { return -EINVAL; }
            }

            if (plane_state->crtc != NULL && plane_state->crtc->index >= config->num_crtc) {
                DRM_ERROR("Plane %d: invalid CRTC index\n", i);
                return -EINVAL;
            }
        }
    }

    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];

        if (conn_state == NULL) { continue; }

        if (conn_state->crtc != NULL && conn_state->crtc->index >= config->num_crtc) {
            DRM_ERROR("Connector %d: invalid CRTC\n", i);
            return -EINVAL;
        }
    }

    return 0;
}

/* ----------------------------------------------------------------- commit */

/*
 * Apply @state: check it, arm the completion events, program the hardware,
 * then publish the new software state.  Order matters -- the hardware is
 * told first so that what a client sees afterwards is what is on screen,
 * and events are allocated before anything can fail.
 */
static int drm_atomic_commit_tail(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     ret;
    int                     i;

    ret = drm_atomic_check_only(state);
    if (ret < 0) { return ret; }

    if (state->page_flip_event) {
        int armed = 0;

        for (i = 0; i < config->num_crtc; i++) {
            struct drm_crtc_state *crtc_state = state->crtcs[i].state;
            struct drm_crtc       *crtc;

            if (crtc_state == NULL) { continue; }

            crtc = state->crtcs[i].ptr;

            crtc_state->event = malloc(sizeof(*crtc_state->event));
            if (crtc_state->event == NULL) { return -ENOMEM; }
            memset(crtc_state->event, 0, sizeof(*crtc_state->event));

            crtc_state->event->dev               = dev;
            crtc_state->event->file_priv         = state->file_priv;
            crtc_state->event->crtc              = crtc;
            crtc_state->event->pipe              = crtc->index;
            crtc_state->event->event.base.type   = DRM_EVENT_FLIP_COMPLETE;
            crtc_state->event->event.base.length = sizeof(crtc_state->event->event);
            crtc_state->event->event.user_data   = state->user_data;
            crtc_state->event->event.crtc_id     = crtc->base.id;
            armed++;
        }

        if (armed == 0) { return -EINVAL; }
    }

    /* Scanout first: flip the primary plane if its framebuffer changed, and
     * give the driver the chance to switch a CRTC off. */
    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc            *crtc       = state->crtcs[i].ptr;
        struct drm_crtc_state      *crtc_state = state->crtcs[i].state;
        struct drm_plane_state     *primary_state = NULL;
        struct drm_crtc_helper_funcs *helpers;
        int                          p;

        if (crtc == NULL || crtc_state == NULL) { continue; }

        for (p = 0; p < config->num_total_plane; p++) {
            if (state->planes[p].ptr == crtc->primary) {
                primary_state = state->planes[p].state;
                break;
            }
        }
        if (primary_state == NULL && crtc->primary != NULL) { primary_state = crtc->primary->state; }

        if (crtc_state->active && primary_state != NULL
            && (crtc->primary->state == NULL || primary_state->fb != crtc->primary->state->fb)) {
            helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (helpers == NULL || helpers->page_flip == NULL) { return -ENOSYS; }
            ret = helpers->page_flip(crtc, primary_state->fb, NULL, 0);
            if (ret != 0) { return ret; }
        }

        if (!crtc_state->active && crtc->enabled && crtc->helper_private != NULL) {
            helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (helpers->atomic_disable != NULL) { helpers->atomic_disable(crtc, crtc->state); }
        }
    }

    /* Publish: CRTCs first, then planes, then which connector goes where. */
    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *entry       = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state  = entry->state;

        if (crtc_state == NULL || entry->ptr == NULL) { continue; }

        if (crtc_state->active_changed) { entry->ptr->enabled = crtc_state->active; }

        if (crtc_state->mode_changed && crtc_state->active) {
            memcpy(&entry->ptr->mode, &crtc_state->mode, sizeof(crtc_state->mode));
        }

        if (entry->ptr->state != NULL) {
            /* The completion event belongs to this commit, not to the
             * object's long-lived state, so it is moved aside first. */
            struct drm_pending_vblank_event *event = crtc_state->event;

            memcpy(entry->ptr->state, crtc_state, sizeof(*crtc_state));
            entry->ptr->state->event = NULL;
            crtc_state->event        = event;
        }
    }

    if (state->planes != NULL) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct __drm_planes_state *entry       = &state->planes[i];
            struct drm_plane_state    *plane_state = entry->state;

            if (plane_state == NULL || entry->ptr == NULL) { continue; }

            if (entry->ptr->state != NULL) {
                entry->ptr->state->fb               = plane_state->fb;
                entry->ptr->state->crtc             = plane_state->crtc;
                entry->ptr->state->src              = plane_state->src;
                entry->ptr->state->dst              = plane_state->dst;
                entry->ptr->state->visible          = plane_state->visible;
                entry->ptr->state->rotation         = plane_state->rotation;
                entry->ptr->state->alpha            = plane_state->alpha;
                entry->ptr->state->zpos             = plane_state->zpos;
                entry->ptr->state->pixel_blend_mode = plane_state->pixel_blend_mode;
            }

            entry->ptr->fb_id   = (plane_state->fb != NULL) ? plane_state->fb->base.id : 0;
            entry->ptr->crtc_id = (plane_state->crtc != NULL) ? plane_state->crtc->base.id : 0;
        }
    }

    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];
        struct drm_connector       *connector  = state->connectors[i];

        if (conn_state == NULL || connector == NULL) { continue; }

        if (conn_state->crtc_changed && connector->state != NULL) { connector->state->crtc = conn_state->crtc; }
    }

    /* Last: switch CRTCs on and hand out the completion events -- either
     * armed for the next frame, or sent now if there is no vblank to wait
     * for. */
    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc              *crtc       = state->crtcs[i].ptr;
        struct drm_crtc_state        *crtc_state = state->crtcs[i].state;
        struct drm_crtc_helper_funcs *helpers;

        if (crtc == NULL || crtc_state == NULL) { continue; }

        if (crtc_state->active && crtc_state->active_changed && crtc->helper_private != NULL) {
            helpers = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (helpers->atomic_enable != NULL) { helpers->atomic_enable(crtc, crtc->state); }
        }

        if (crtc_state->event != NULL) {
            if (crtc->enabled && drm_crtc_vblank_get(crtc) == 0) {
                crtc_state->event->vblank_ref = true;
                crtc_state->event->sequence   = (uint64_t)drm_crtc_vblank_count(crtc) + 1;
                drm_crtc_arm_vblank_event(crtc, crtc_state->event);
            } else {
                drm_crtc_send_vblank_event(crtc, crtc_state->event);
            }
            crtc_state->event = NULL;
        }
    }

    drm_atomic_state_free(state);
    return 0;
}

/*
 * Commits take a number and run in that order: waiting here is what stops
 * two clients from interleaving their register writes.
 */
static void drm_atomic_commit_wait_turn(struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &state->dev->mode_config;

    spin_lock(&config->commit_queue_lock);
    if (state->commit_seq == 0) { state->commit_seq = ++config->commit_queue_next; }
    while (state->commit_seq != config->commit_queue_done + 1) {
        wait_queue_prepare(&config->commit_queue_wait);
        spin_unlock(&config->commit_queue_lock);
        wait_queue_sleep();
        spin_lock(&config->commit_queue_lock);
    }
    spin_unlock(&config->commit_queue_lock);
}

static void drm_atomic_commit_finish_turn(struct drm_device *dev)
{
    spin_lock(&dev->mode_config.commit_queue_lock);
    dev->mode_config.commit_queue_done++;
    spin_unlock(&dev->mode_config.commit_queue_lock);
    wait_queue_wake_all(&dev->mode_config.commit_queue_wait);
}

int drm_atomic_commit(struct drm_atomic_state *state)
{
    struct drm_device *dev;
    int                ret;

    if (state == NULL || state->dev == NULL) { return -EINVAL; }

    dev = state->dev;

    if (state->commit_seq == 0) {
        spin_lock(&dev->mode_config.commit_queue_lock);
        if (dev->mode_config.commit_queue_next != dev->mode_config.commit_queue_done) {
            spin_unlock(&dev->mode_config.commit_queue_lock);
            return -EBUSY;
        }
        state->commit_seq = ++dev->mode_config.commit_queue_next;
        spin_unlock(&dev->mode_config.commit_queue_lock);
    }

    drm_atomic_commit_wait_turn(state);
    ret = drm_atomic_commit_tail(state);
    drm_atomic_commit_finish_turn(dev);
    return ret;
}

/* ------------------------------------------------------- non-blocking commit */

static void drm_atomic_nonblock_worker(void *arg)
{
    struct drm_atomic_state *state     = (struct drm_atomic_state *)arg;
    struct drm_device       *dev       = state->dev;
    struct drm_file         *file_priv = state->file_priv;
    int                      ret       = drm_atomic_commit(state);

    if (ret != 0) { drm_atomic_state_free(state); }

    if (file_priv != NULL) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_refs != 0) { file_priv->event_refs--; }
        spin_unlock(&file_priv->event_lock);
        wait_queue_wake_all(&file_priv->event_wait);
    }

    drm_dev_put(dev);
}

/*
 * Hand @state to a worker thread and return at once.  The checks run here
 * rather than on the worker, so a client gets told immediately when its
 * configuration is impossible -- that is the whole point of asking for a
 * non-blocking commit and still wanting an answer.
 */
int drm_atomic_nonblocking_commit(struct drm_atomic_state *state)
{
    struct drm_mode_config *config;
    struct drm_file        *file_priv;
    proc_t                 *worker;
    int                     ret;

    if (state == NULL || state->dev == NULL) { return -EINVAL; }

    ret = drm_atomic_check_only(state);
    if (ret != 0) { return ret; }

    config    = &state->dev->mode_config;
    file_priv = state->file_priv;

    if (file_priv != NULL) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_closing) {
            spin_unlock(&file_priv->event_lock);
            return -ENOENT;
        }
        file_priv->event_refs++;
        spin_unlock(&file_priv->event_lock);
    }

    if (!drm_dev_get(state->dev)) {
        if (file_priv != NULL) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
        }
        return -ENODEV;
    }

    spin_lock(&config->commit_queue_lock);
    if (config->commit_queue_next != config->commit_queue_done) {
        spin_unlock(&config->commit_queue_lock);
        drm_dev_put(state->dev);
        if (file_priv != NULL) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
            wait_queue_wake_all(&file_priv->event_wait);
        }
        return -EBUSY;
    }

    state->commit_seq = ++config->commit_queue_next;
    worker            = kthread_create("drm-atomic", drm_atomic_nonblock_worker, state);
    if (worker == NULL) { config->commit_queue_next--; }
    spin_unlock(&config->commit_queue_lock);

    if (worker == NULL) {
        drm_dev_put(state->dev);
        if (file_priv != NULL) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
            wait_queue_wake_all(&file_priv->event_wait);
        }
        return -ENOMEM;
    }

    return 0;
}
