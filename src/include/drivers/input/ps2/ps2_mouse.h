/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps2_mouse.h — the three-byte PS/2 mouse packet decoder. (GPLv2)
 */
#ifndef GNUCOS_PS2_MOUSE_H
#define GNUCOS_PS2_MOUSE_H

#include <stdint.h>
#include "evdev.h"

/* Fill the mouse device's EV/KEY/REL capability maps. */
void ps2_mouse_fill_caps(evdev_dev_t *d);

/* Feed one auxiliary-port byte.  Incomplete packets are held in static
 * state until the third byte arrives. */
void ps2_mouse_handle_byte(uint8_t b);

#endif
