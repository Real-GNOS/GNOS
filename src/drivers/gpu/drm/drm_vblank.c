/*
 * drm_vblank.c - counting frames, and telling people when one happened.
 * (GPLv2)
 *
 * Everything that has to be timed to the display hangs off this: a
 * compositor waiting until the monitor is between frames before it swaps,
 * a page flip that must not tear, an animation that wants to know how many
 * frames have gone by.  The primitive is a counter per CRTC that ticks once
 * per vertical blanking interval, plus a queue of events each stamped with
 * the count it should fire at.
 *
 * There is no vblank interrupt to hook here -- this kernel drives the count
 * from the clock (drm_vblank_tick, at the nominal 60 Hz period), which is
 * close enough for clients that only need to pace themselves, and a driver
 * with real hardware can call drm_handle_vblank instead.
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

/* One per CRTC: the counter, the clocks and the outstanding events. */
int drm_vblank_init(struct drm_device *dev, unsigned int num_crtcs)
{
    struct drm_vblank_crtc *vblank;
    unsigned int            i;

    if (dev == NULL || num_crtcs == 0) { return -EINVAL; }

    vblank = malloc(sizeof(*vblank) * num_crtcs);
    if (vblank == NULL) { return -ENOMEM; }
    memset(vblank, 0, sizeof(*vblank) * num_crtcs);

    for (i = 0; i < num_crtcs; i++) {
        vblank[i].dev              = dev;
        vblank[i].lock.v           = 0;
        vblank[i].pipe             = i;
        vblank[i].count            = 0;
        vblank[i].last             = 0;
        vblank[i].enabled          = false;
        vblank[i].inmodeset        = false;
        vblank[i].max_vblank_count = 0;
        vblank[i].event_queue      = NULL;
        vblank[i].refcount         = 0;
        vblank[i].period_ns        = 16666667ULL; /* 60 Hz */
        vblank[i].next_vblank_ns   = 0;
        vblank[i].timestamp_ns     = 0;
        vblank[i].crtc             = NULL;
        wait_queue_init(&vblank[i].wait);
    }

    dev->vblank_unused_array = vblank;
    dev->num_crtc            = (int)num_crtcs;

    return 0;
}

/* The per-CRTC slot, or NULL when @crtc has no vblank behind it. */
static struct drm_vblank_crtc *drm_vblank_of(const struct drm_crtc *crtc)
{
    struct drm_device *dev;

    if (crtc == NULL || crtc->dev == NULL) { return NULL; }

    dev = crtc->dev;

    if (crtc->index < 0 || crtc->index >= dev->num_crtc) { return NULL; }
    if (dev->vblank_unused_array == NULL) { return NULL; }

    return &dev->vblank_unused_array[crtc->index];
}

uint32_t drm_crtc_vblank_count(struct drm_crtc *crtc)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL) { return 0; }

    return vblank->count;
}

/* Register interest: as long as somebody holds a reference, the counter for
 * this CRTC keeps running. */
int drm_crtc_vblank_get(struct drm_crtc *crtc)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL) { return -EINVAL; }

    spin_lock(&vblank->lock);
    vblank->crtc    = crtc;
    vblank->refcount++;
    vblank->enabled = true;
    spin_unlock(&vblank->lock);

    return 0;
}

void drm_crtc_vblank_put(struct drm_crtc *crtc)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL) { return; }

    spin_lock(&vblank->lock);
    if (vblank->refcount != 0) { vblank->refcount--; }
    /* Stay on while either a reference or a queued event needs the count. */
    if (vblank->refcount == 0 && vblank->event_queue == NULL) { vblank->enabled = false; }
    spin_unlock(&vblank->lock);
}

/*
 * Put @e on this CRTC's event queue.  The queue is ordered by the count the
 * events are waiting for, so drm_handle_vblank can take a prefix off the
 * front instead of scanning.
 */
void drm_crtc_arm_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL || e == NULL) { return; }

    spin_lock(&vblank->lock);

    e->pipe      = crtc->index;
    e->crtc      = crtc;
    e->next      = NULL;
    vblank->crtc = crtc;

    /* Take the file's reference before queueing: it is what keeps the file
     * alive until the event is delivered, and it must not be taken once the
     * client has started closing. */
    if (e->file_priv != NULL && !e->file_ref) {
        spin_lock(&e->file_priv->event_lock);
        if (e->file_priv->event_closing) {
            spin_unlock(&e->file_priv->event_lock);
            if (e->vblank_ref && vblank->refcount != 0) {
                vblank->refcount--;
                e->vblank_ref = false;
            }
            spin_unlock(&vblank->lock);
            free(e);
            return;
        }
        e->file_priv->event_refs++;
        e->file_ref = true;
        spin_unlock(&e->file_priv->event_lock);
    }

    if (vblank->event_queue == NULL || e->sequence < vblank->event_queue->sequence) {
        e->next             = vblank->event_queue;
        vblank->event_queue = e;
    } else {
        struct drm_pending_vblank_event *cur = vblank->event_queue;

        while (cur->next != NULL && cur->next->sequence <= e->sequence) { cur = cur->next; }
        e->next   = cur->next;
        cur->next = e;
    }

    spin_unlock(&vblank->lock);
}

/* Stamp @e with the time and count it fired at and hand it to its owner. */
void drm_crtc_send_vblank_event(struct drm_crtc *crtc, struct drm_pending_vblank_event *e)
{
    struct drm_vblank_crtc *vblank;
    uint64_t                timestamp;

    if (e == NULL || e->dev == NULL) { return; }
    if (crtc == NULL) { crtc = e->crtc; }

    vblank = (crtc != NULL) ? drm_vblank_of(crtc) : NULL;
    if (vblank == NULL) {
        /* Nowhere to attribute it: release everything it was holding and
         * drop it, rather than delivering an event with a bogus timestamp. */
        if (e->vblank_ref && e->crtc != NULL) {
            drm_crtc_vblank_put(e->crtc);
            e->vblank_ref = false;
        }
        if (e->file_ref && e->file_priv != NULL) {
            spin_lock(&e->file_priv->event_lock);
            if (e->file_priv->event_refs != 0) { e->file_priv->event_refs--; }
            e->file_ref = false;
            spin_unlock(&e->file_priv->event_lock);
            wait_queue_wake_all(&e->file_priv->event_wait);
        }
        free(e);
        return;
    }

    timestamp = (vblank->timestamp_ns != 0) ? vblank->timestamp_ns : nano_time();

    e->event.sequence = (uint32_t)e->sequence;
    e->event.crtc_id  = crtc->base.id;
    e->event.tv_sec   = (uint32_t)(timestamp / 1000000000ULL);
    e->event.tv_usec  = (uint32_t)((timestamp / 1000ULL) % 1000000ULL);

    if (drm_send_event(e->dev, e) != 0) { free(e); }
}

void drm_crtc_vblank_off(struct drm_crtc *crtc)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL) { return; }

    spin_lock(&vblank->lock);
    vblank->enabled        = false;
    vblank->next_vblank_ns = 0;
    spin_unlock(&vblank->lock);
}

void drm_crtc_vblank_on(struct drm_crtc *crtc)
{
    struct drm_vblank_crtc *vblank = drm_vblank_of(crtc);

    if (vblank == NULL) { return; }

    spin_lock(&vblank->lock);
    vblank->enabled = true;
    if (vblank->next_vblank_ns == 0) { vblank->next_vblank_ns = nano_time() + vblank->period_ns; }
    spin_unlock(&vblank->lock);
}

/*
 * A frame ended on @pipe.  Bump the count, take off every event that is due,
 * let the driver know, finish any page flip whose target frame has arrived,
 * and only then deliver the events -- delivering can sleep, so it happens
 * after the count has already moved on.
 */
void drm_handle_vblank(struct drm_device *dev, unsigned int pipe)
{
    struct drm_vblank_crtc           *vblank;
    struct drm_pending_vblank_event  *ready = NULL;
    struct drm_pending_vblank_event **tail  = &ready;
    struct drm_crtc_helper_funcs     *helpers;

    if (dev == NULL || (int)pipe >= dev->num_crtc || dev->vblank_unused_array == NULL) { return; }

    vblank = &dev->vblank_unused_array[pipe];

    spin_lock(&vblank->lock);

    vblank->count++;
    vblank->last         = vblank->count;
    vblank->timestamp_ns = nano_time();

    while (vblank->event_queue != NULL && vblank->event_queue->sequence <= vblank->count) {
        struct drm_pending_vblank_event *e = vblank->event_queue;

        vblank->event_queue = e->next;
        e->next             = NULL;
        *tail               = e;
        tail                = &e->next;
    }

    spin_unlock(&vblank->lock);
    wait_queue_wake_all(&vblank->wait);

    helpers = (vblank->crtc != NULL) ? (struct drm_crtc_helper_funcs *)vblank->crtc->helper_private : NULL;
    if (helpers != NULL && helpers->vblank != NULL) { helpers->vblank(vblank->crtc); }

    if (vblank->crtc != NULL) {
        bool completed_flip = false;

        spin_lock(&vblank->crtc->commit_lock);
        if (vblank->crtc->page_flip_pending && vblank->crtc->page_flip_target <= vblank->count) {
            vblank->crtc->page_flip_pending = false;
            vblank->crtc->page_flip_target  = 0;
            completed_flip                  = true;
        }
        spin_unlock(&vblank->crtc->commit_lock);

        if (completed_flip) { drm_crtc_vblank_put(vblank->crtc); }
    }

    while (ready != NULL) {
        struct drm_pending_vblank_event *e = ready;

        ready = e->next;

        if (e->vblank_ref && e->crtc != NULL) {
            e->vblank_ref = false;
            drm_crtc_vblank_put(e->crtc);
        }
        drm_crtc_send_vblank_event(e->crtc, e);
    }
}

/*
 * Called from the system tick: pretend a vblank arrived on every enabled
 * CRTC whose next frame is due.  Skipping whole periods in one go (rather
 * than ticking once) is what keeps the clock from sliding behind when the
 * tick is coarser than the frame rate.
 */
void drm_vblank_tick(void)
{
    extern struct drm_device *drm_get_singleton(void);
    struct drm_device        *dev = drm_get_singleton();
    uint64_t                  now = nano_time();
    int                       i;

    if (dev == NULL || dev->vblank_unused_array == NULL) { return; }

    for (i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc *vblank = &dev->vblank_unused_array[i];
        bool                    due;

        spin_lock(&vblank->lock);
        if (!vblank->enabled) {
            spin_unlock(&vblank->lock);
            continue;
        }
        if (vblank->next_vblank_ns == 0) { vblank->next_vblank_ns = now + vblank->period_ns; }
        due = now >= vblank->next_vblank_ns;
        if (due) {
            do {
                vblank->next_vblank_ns += vblank->period_ns;
            } while (now >= vblank->next_vblank_ns);
        }
        spin_unlock(&vblank->lock);

        if (due) { drm_handle_vblank(dev, (unsigned int)i); }
    }
}

/*
 * DRM_IOCTL_WAIT_VBLANK.  Two shapes: with _DRM_VBLANK_EVENT, queue an
 * event for the target frame and return immediately; without it, sleep
 * until the count reaches the target.  The target is either an absolute
 * count or an offset from now.
 */
int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    union drm_wait_vblank  *vblwait = (union drm_wait_vblank *)data;
    unsigned int            pipe;
    unsigned int            flags;
    struct drm_vblank_crtc *vblank;
    uint32_t                target;
    uint32_t                current;
    uint32_t                allowed;

    if (dev == NULL || vblwait == NULL) { return -EINVAL; }

    flags   = vblwait->request.type;
    allowed = _DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK | _DRM_VBLANK_HIGH_CRTC_MASK;
    if ((flags & ~allowed) != 0) { return -EINVAL; }
    if ((flags & (_DRM_VBLANK_SIGNAL | _DRM_VBLANK_FLIP)) != 0) { return -EINVAL; }

    pipe = (flags & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
    if ((flags & _DRM_VBLANK_SECONDARY) != 0 && pipe == 0) { pipe = 1; }

    if (pipe >= (unsigned int)dev->num_crtc || dev->vblank_unused_array == NULL) { return -EINVAL; }

    vblank = &dev->vblank_unused_array[pipe];
    if (vblank->crtc == NULL) { return -EINVAL; }

    spin_lock(&vblank->lock);
    current = vblank->count;
    target  = (flags & _DRM_VBLANK_RELATIVE) ? current + vblwait->request.sequence : vblwait->request.sequence;
    /* Missed it already?  NEXTONMISS says take the next one instead of
     * returning an event that is instantly in the past. */
    if ((flags & _DRM_VBLANK_NEXTONMISS) != 0 && (int32_t)(current - target) >= 0) { target = current + 1; }
    spin_unlock(&vblank->lock);

    if ((flags & _DRM_VBLANK_EVENT) != 0) {
        struct drm_pending_vblank_event *e = malloc(sizeof(*e));

        if (e == NULL) { return -ENOMEM; }
        memset(e, 0, sizeof(*e));

        e->dev               = dev;
        e->file_priv         = file_priv;
        e->pipe              = pipe;
        e->crtc              = vblank->crtc;
        e->event.base.type   = DRM_EVENT_VBLANK;
        e->event.base.length = sizeof(e->event);
        e->event.user_data   = vblwait->request.signal;
        e->event.crtc_id     = e->crtc->base.id;
        e->sequence          = target;

        if (drm_crtc_vblank_get(e->crtc) != 0) {
            free(e);
            return -EINVAL;
        }
        e->vblank_ref = true;

        if ((int32_t)(current - target) >= 0) {
            /* Already past it: deliver now instead of queueing for a frame
             * that will never be "next". */
            e->sequence  = current;
            e->vblank_ref = false;
            drm_crtc_vblank_put(e->crtc);
            drm_crtc_send_vblank_event(e->crtc, e);
        } else {
            drm_crtc_arm_vblank_event(e->crtc, e);
        }

        vblwait->reply.sequence  = vblank->count;
        vblwait->reply.tval_sec  = (int)(vblank->timestamp_ns / 1000000000ULL);
        vblwait->reply.tval_usec = (int)((vblank->timestamp_ns / 1000ULL) % 1000000ULL);

        return 0;
    }

    if (drm_crtc_vblank_get(vblank->crtc) != 0) { return -EINVAL; }

    for (;;) {
        spin_lock(&vblank->lock);
        current = vblank->count;
        if ((int32_t)(current - target) >= 0) {
            spin_unlock(&vblank->lock);
            break;
        }
        wait_queue_prepare(&vblank->wait);
        spin_unlock(&vblank->lock);
        wait_queue_sleep();
    }
    drm_crtc_vblank_put(vblank->crtc);

    vblwait->reply.sequence  = current;
    vblwait->reply.tval_sec  = (int)(vblank->timestamp_ns / 1000000000ULL);
    vblwait->reply.tval_usec = (int)((vblank->timestamp_ns / 1000ULL) % 1000000ULL);

    return 0;
}

/* A client is going away: drop its queued events and release what they held. */
void drm_vblank_cancel_pending(struct drm_device *dev, struct drm_file *file_priv)
{
    int i;

    if (dev == NULL || file_priv == NULL || dev->vblank_unused_array == NULL) { return; }

    for (i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc           *vblank = &dev->vblank_unused_array[i];
        struct drm_pending_vblank_event **link;

        spin_lock(&vblank->lock);

        link = &vblank->event_queue;
        while (*link != NULL) {
            struct drm_pending_vblank_event *event = *link;

            if (event->file_priv != file_priv) {
                link = &event->next;
                continue;
            }

            *link = event->next;

            if (event->vblank_ref && vblank->refcount != 0) { vblank->refcount--; }

            if (event->file_ref) {
                spin_lock(&file_priv->event_lock);
                if (file_priv->event_refs != 0) { file_priv->event_refs--; }
                event->file_ref = false;
                spin_unlock(&file_priv->event_lock);
                wait_queue_wake_all(&file_priv->event_wait);
            }
            free(event);
        }

        if (vblank->refcount == 0 && vblank->event_queue == NULL) { vblank->enabled = false; }

        spin_unlock(&vblank->lock);
    }
}

void drm_vblank_cleanup(struct drm_device *dev)
{
    int i;

    if (dev == NULL || dev->vblank_unused_array == NULL) { return; }

    for (i = 0; i < dev->num_crtc; i++) {
        struct drm_vblank_crtc          *vblank = &dev->vblank_unused_array[i];
        struct drm_pending_vblank_event *e      = vblank->event_queue;

        while (e != NULL) {
            struct drm_pending_vblank_event *next = e->next;

            free(e);
            e = next;
        }
        vblank->event_queue = NULL;

        /* Anyone asleep on this counter has to wake up and notice it is
         * gone, or they hang forever. */
        wait_queue_wake_all(&vblank->wait);
    }

    free(dev->vblank_unused_array);
    dev->vblank_unused_array = NULL;
    dev->num_crtc            = 0;
}
