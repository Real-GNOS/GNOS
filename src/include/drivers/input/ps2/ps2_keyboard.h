/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps2_keyboard.h — scancode-set-1 to Linux KEY_* decoding. (GPLv2)
 */
#ifndef GNUCOS_PS2_KEYBOARD_H
#define GNUCOS_PS2_KEYBOARD_H

#include <stdint.h>
#include "evdev.h"

/* Fill the keyboard device's EV/KEY capability maps from the scancode
 * table, so libinput's probe sees every key the decoder can produce. */
void ps2_keyboard_fill_caps(evdev_dev_t *d);

/* Feed one raw keyboard byte (the value tty.c read from port 0x60). */
void ps2_keyboard_handle_byte(uint8_t sc);

#endif
