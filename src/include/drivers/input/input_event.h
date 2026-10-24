/* SPDX-License-Identifier: GPL-2.0 */
/*
 * input_event.h — the Linux input-event record and the constants the
 * kernel-side evdev implementation shares. (GPLv2)
 *
 * The record layout is Linux's own struct input_event on x86-64: a
 * 16-byte timeval followed by the 2+2+4 type/code/value triple.  Keeping
 * it byte-for-byte identical is what lets libinput, libevdev and Xorg
 * read /dev/input/event* without a translation layer.
 */
#ifndef GNUCOS_INPUT_EVENT_H
#define GNUCOS_INPUT_EVENT_H

#include <stdint.h>

/* ---- event types ------------------------------------------------------ */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_LED 0x11

#define SYN_REPORT 0

/* ---- relative axes ---------------------------------------------------- */
#define REL_X 0x00
#define REL_Y 0x01

/* ---- mouse buttons ---------------------------------------------------- */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define BTN_TASK   0x117

#define KEY_RESERVED 0
#define KEY_PAUSE    119

/* ---- keyboard LEDs ---------------------------------------------------- */
#define LED_NUML    0
#define LED_CAPSL   1
#define LED_SCROLLL 2

/* ---- evdev identity --------------------------------------------------- */
/* Linux's EV_VERSION is 0x010001; libevdev compares EVIOCGVERSION against
 * exactly this value, so the three fields have to add up to it. */
#define EV_VERSION_MAJOR 0x01
#define EV_VERSION_MINOR 0x00
#define EV_VERSION_PATCH 0x01

#define BUS_I8042 0x11

/* Width of the type bitmap EVIOCGBIT(0, len) reports (EV_MAX + 1). */
#define EV_CNT 0x20

/* One evdev ring buffer per device.  64 events is what the old combined
 * driver used; keeping it a compile-time constant lets the queue live
 * inside the device instead of in a separately owned allocation. */
#define EVDEV_QUEUE_SIZE 64

typedef struct {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

#endif
