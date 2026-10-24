/* SPDX-License-Identifier: GPL-2.0 */
/*
 * evdev.h — kernel-side evdev device state and event injection. (GPLv2)
 *
 * One evdev_dev_t is one /dev/input/eventN node.  Transports (the i8042
 * PS/2 decoder, USB HID boot protocol) fill the capability maps and push
 * decoded events; the VFS side in evdev.c owns read/poll/ioctl.
 */
#ifndef GNUCOS_EVDEV_H
#define GNUCOS_EVDEV_H

#include <stdint.h>
#include "input_event.h"
#include "evdev_queue.h"

typedef struct evdev_dev {
    evdev_queue_t q;
    const char   *name;
    uint16_t      vendor;
    uint16_t      product;
    /* Bit T of kind_bits says "this device emits event type T".  This is
     * a bitmap, not an array indexed by type: EV_LED is 0x11, and the old
     * uint16_t kind_bits[8] indexed it out of bounds. */
    uint32_t kind_bits;
    uint16_t key_bits[64];   /* KEY_CNT = 1024 bits: codes the device emits */
    uint16_t key_state[64];  /* currently held keys, for EVIOCGKEY */
    uint16_t rel_bits[2];    /* REL_CNT = 32 bits */
    uint16_t led_bits;       /* LED_CNT = 16 bits, for EVIOCGBIT(EV_LED) */
    int      grabbed;
    int      nonblock;
} evdev_dev_t;

/* The two built-in devices, created before evdev_init() registers them. */
evdev_dev_t *evdev_keyboard(void);
evdev_dev_t *evdev_mouse(void);

/* Initialise the queues and publish /dev/input/event0 and event1.
 * Returns 0 on success, -1 if the VFS refused a node. */
int evdev_init(void);

/* Queue one event with the current realtime timestamp.  A full queue
 * drops the event, matching Linux's overflow behaviour. */
void evdev_push_event(evdev_dev_t *d, uint16_t type, uint16_t code, int32_t value);

/* Frame the events pushed since the last SYN_REPORT. */
void evdev_push_syn(evdev_dev_t *d);

/* Wake blocking readers and poll/epoll waiters. */
void evdev_signal(void);

/* Non-blocking pop of one event.  Returns 1 and fills the out parameters,
 * or 0 when the queue is empty. */
int evdev_pull(evdev_dev_t *d, uint16_t *type, uint16_t *code, int32_t *value);

#endif
