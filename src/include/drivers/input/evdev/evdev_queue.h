/* SPDX-License-Identifier: GPL-2.0 */
/*
 * evdev_queue.h — the per-device evdev event ring. (GPLv2)
 *
 * This is deliberately a plain ring of already-timestamped records: the
 * queue owns no locks and no sleepers.  evdev.c serialises access with
 * cli/sti around the blocking read, exactly like the pipe code does, and
 * the IRQ side only ever calls evdev_queue_push().
 */
#ifndef GNUCOS_EVDEV_QUEUE_H
#define GNUCOS_EVDEV_QUEUE_H

#include <stdint.h>
#include "input_event.h"

typedef struct {
    uint32_t      head;
    uint32_t      tail;
    uint32_t      count;
    input_event_t ev[EVDEV_QUEUE_SIZE];
} evdev_queue_t;

/* Reset a queue to empty. */
void evdev_queue_init(evdev_queue_t *q);

/* Append one event.  Returns 0 on success, -1 when the ring is full. */
int evdev_queue_push(evdev_queue_t *q, const input_event_t *ev);

/* Remove up to `max` events, copying them contiguously to `out`.  Returns
 * the number removed. */
uint32_t evdev_queue_pop(evdev_queue_t *q, input_event_t *out, uint32_t max);

/* Remove up to `max` events, copying each record to `dst` (which may be a
 * user buffer).  Returns the number removed. */
uint32_t evdev_queue_pop_to(evdev_queue_t *q, void *dst, uint32_t max);

/* Number of queued events. */
static inline uint32_t evdev_queue_count(const evdev_queue_t *q)
{
    return q->count;
}

#endif
