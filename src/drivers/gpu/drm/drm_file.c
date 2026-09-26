/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drm_file.c - what each open file descriptor owns. (GPLv2)
 *
 * Every open of /dev/dri/card0 gets a drm_file: its GEM handles, its magic
 * table, and its queue of pending events.  Two properties matter more than
 * the rest.  Handles are per file, so one client cannot name another's
 * buffers.  And the event queue has to survive the client going away -- a
 * file cannot be freed while an event still points at it, or the kernel
 * would write into freed memory the next time a frame completed.
 *
 * Events are the only thing a client reads from the fd: a small header,
 * then whatever payload (a vblank event, a page-flip completion).  read()
 * returns exactly one event and never a partial one.
 */

#ifndef container_of
#    define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm_device.h"
#include "drm_hashtab.h"
#include "drm_port.h" /* copy_to_user wrapper */
#include "drm_print.h"
#include "heap.h"
#include "intrusive_list.h"
#include "kstring.h"
#include "smp.h"
#include "vfs.h"

/* What poll(2) asks for and what we answer with. */
#define DRM_POLLIN     0x0001
#define DRM_POLLOUT    0x0004
#define DRM_POLLRDNORM 0x0040

struct drm_file *drm_file_alloc(struct drm_device *dev)
{
    struct drm_file *file;

    (void)dev;

    file = malloc(sizeof(*file));
    if (file == NULL) { return NULL; }
    memset(file, 0, sizeof(*file));

    drm_idr_init(&file->object_idr);
    ilist_init(&file->fbs_head);
    ilist_init(&file->object_list);

    if (drm_ht_create(&file->magiclist, 4) != 0) {
        drm_idr_destroy(&file->object_idr);
        free(file);
        return NULL;
    }

    file->authenticated        = false;
    file->universal_planes     = false;
    file->atomic               = false;
    file->aspect_ratio_allowed = false;
    file->event_list_head      = NULL;
    file->event_list_tail      = NULL;
    file->event_space          = 0;
    file->event_closing        = false;
    wait_queue_init(&file->event_wait);

    return file;
}

void drm_file_free(struct drm_file *file)
{
    struct drm_event_node *node;

    if (file == NULL) { return; }

    /* Refuse new events, then wait for the ones already armed to be
     * delivered: they hold a pointer to this file. */
    spin_lock(&file->event_lock);
    file->event_closing = true;
    while (file->event_refs != 0) {
        wait_queue_prepare(&file->event_wait);
        spin_unlock(&file->event_lock);
        wait_queue_sleep();
        spin_lock(&file->event_lock);
    }
    spin_unlock(&file->event_lock);

    node = file->event_list_head;
    while (node != NULL) {
        struct drm_event_node *next = node->next;

        free(node->event);
        free(node);
        node = next;
    }
    file->event_list_head = NULL;
    file->event_list_tail = NULL;

    drm_ht_destroy(&file->magiclist);
    drm_idr_destroy(&file->object_idr);
    free(file);
}

/* Give back the reference an event took on the file it was armed for. */
static void drm_event_release_file_ref(struct drm_pending_vblank_event *e)
{
    struct drm_file *file_priv;

    if (e == NULL || !e->file_ref || e->file_priv == NULL) { return; }

    file_priv = e->file_priv;

    spin_lock(&file_priv->event_lock);
    if (file_priv->event_refs != 0) { file_priv->event_refs--; }
    e->file_ref = false;
    spin_unlock(&file_priv->event_lock);

    wait_queue_wake_all(&file_priv->event_wait);
}

/*
 * Queue @e for delivery to its file and wake anybody reading.  The event is
 * copied into the queue, so @e itself is finished with by the time we
 * return -- destroyed through its own callback if it has one.
 */
int drm_send_event(struct drm_device *dev, struct drm_pending_vblank_event *e)
{
    struct drm_event_node *node;
    struct drm_file       *file_priv;

    if (e == NULL) { return -EINVAL; }

    file_priv = e->file_priv;
    if (file_priv == NULL) {
        /* No owner was recorded: fall back to the first file on the device,
         * which is the master in practice. */
        spin_lock(&dev->filelist_lock);
        if (dev->filelist.next == NULL || dev->filelist.next == &dev->filelist) {
            spin_unlock(&dev->filelist_lock);
            return -ENOENT;
        }
        file_priv = container_of(dev->filelist.next, struct drm_file, head);
        spin_unlock(&dev->filelist_lock);
    }

    node = malloc(sizeof(*node));
    if (node == NULL) {
        drm_event_release_file_ref(e);
        return -ENOMEM;
    }

    node->event = malloc(e->event.base.length);
    if (node->event == NULL) {
        free(node);
        drm_event_release_file_ref(e);
        return -ENOMEM;
    }
    memcpy(node->event, &e->event, e->event.base.length);
    node->next = NULL;

    spin_lock(&file_priv->event_lock);

    if (file_priv->event_closing) {
        /* The client is gone: drop the event rather than queue it for a
         * reader that will never come. */
        if (e->file_ref && file_priv->event_refs != 0) { file_priv->event_refs--; }
        e->file_ref = false;
        spin_unlock(&file_priv->event_lock);
        wait_queue_wake_all(&file_priv->event_wait);

        free(node->event);
        free(node);
        if (e->destroy != NULL) {
            e->destroy(e);
        } else {
            free(e);
        }
        return 0;
    }

    if (file_priv->event_list_tail != NULL) {
        file_priv->event_list_tail->next = node;
    } else {
        file_priv->event_list_head = node;
    }
    file_priv->event_list_tail = node;
    file_priv->event_space += (int)e->event.base.length;

    if (e->file_ref && file_priv->event_refs != 0) { file_priv->event_refs--; }
    e->file_ref = false;

    spin_unlock(&file_priv->event_lock);
    wait_queue_wake_all(&file_priv->event_wait);

    if (e->destroy != NULL) {
        e->destroy(e);
    } else {
        free(e);
    }

    return 0;
}

/*
 * read(): one whole event, or nothing.  Sleeping here is the point -- a
 * compositor parks in this call and wakes when a frame completes.
 */
int drm_read(struct drm_file *file_priv, char *buf, size_t count, size_t *offset)
{
    struct drm_event_node *node;
    size_t                 copy_size;

    (void)offset;

    if (file_priv == NULL || buf == NULL || count == 0) { return -EINVAL; }

    for (;;) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_list_head != NULL) { break; }
        if (file_priv->event_closing) {
            spin_unlock(&file_priv->event_lock);
            return 0; /* EOF once the file is being torn down */
        }
        wait_queue_prepare(&file_priv->event_wait);
        spin_unlock(&file_priv->event_lock);
        wait_queue_sleep();
    }

    node = file_priv->event_list_head;
    if (count < node->event->length) {
        spin_unlock(&file_priv->event_lock);
        return -EINVAL; /* a partial event is worse than no event */
    }

    file_priv->event_list_head = node->next;
    if (file_priv->event_list_head == NULL) { file_priv->event_list_tail = NULL; }
    file_priv->event_space -= (int)node->event->length;

    copy_size = node->event->length;
    spin_unlock(&file_priv->event_lock);

    if (copy_to_user(buf, node->event, copy_size) != 0) {
        free(node->event);
        free(node);
        return -EFAULT;
    }

    free(node->event);
    free(node);
    return (int)copy_size;
}

/* poll(): readable when an event is waiting; always writable, since
 * everything else about this device is done with ioctls. */
unsigned int drm_poll(struct drm_file *file_priv, unsigned int events)
{
    unsigned int mask = 0;

    if (file_priv == NULL) { return 0; }

    spin_lock(&file_priv->event_lock);
    if (file_priv->event_list_head != NULL) {
        if ((events & DRM_POLLIN) != 0) { mask |= DRM_POLLIN; }
        if ((events & DRM_POLLRDNORM) != 0) { mask |= DRM_POLLRDNORM; }
    }
    if ((events & DRM_POLLOUT) != 0) { mask |= DRM_POLLOUT; }
    spin_unlock(&file_priv->event_lock);

    return mask;
}
