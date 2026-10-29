/* SPDX-License-Identifier: GPL-2.0 */
/*
 * evdev_queue.c — the per-device evdev event ring. (GPLv2)
 *
 * No locking lives here: IRQ producers and the blocking reader are
 * serialised by the evdev core (cli/sti around the consumer side).  The
 * queue is small enough to sit inside evdev_dev_t, so there is no
 * allocation and no failure path at device creation.
 */
#include "evdev_queue.h"
#include "kstring.h"

void evdev_queue_init(evdev_queue_t *q)
{
    q->head  = 0;
    q->tail  = 0;
    q->count = 0;
}

int evdev_queue_push(evdev_queue_t *q, const input_event_t *ev)
{
    if (q->count == EVDEV_QUEUE_SIZE)
        return -1; /* full: drop the newest event, like Linux's overflow */

    q->ev[q->tail] = *ev;
    q->tail        = (q->tail + 1) % EVDEV_QUEUE_SIZE;
    q->count++;
    return 0;
}

uint32_t evdev_queue_pop(evdev_queue_t *q, input_event_t *out, uint32_t max)
{
    uint32_t n = q->count < max ? q->count : max;

    for (uint32_t i = 0; i < n; i++) {
        out[i]  = q->ev[q->head];
        q->head = (q->head + 1) % EVDEV_QUEUE_SIZE;
        q->count--;
    }
    return n;
}

uint32_t evdev_queue_pop_to(evdev_queue_t *q, void *dst, uint32_t max)
{
    uint32_t n = q->count < max ? q->count : max;

    for (uint32_t i = 0; i < n; i++) {
        memcpy((uint8_t *)dst + i * sizeof(input_event_t), &q->ev[q->head], sizeof(input_event_t));
        q->head = (q->head + 1) % EVDEV_QUEUE_SIZE;
        q->count--;
    }
    return n;
}
