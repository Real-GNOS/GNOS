/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mouse.c — the three-byte PS/2 mouse packet decoder. (GPLv2)
 *
 * The auxiliary port delivers packets MSB-first: flags, dx, dy.  The
 * first byte always has bit 3 set, which is also the resync point when a
 * stray command ACK (0xFA) lands in the stream after init.
 */
#include "ps2_mouse.h"
#include "input_event.h"

void ps2_mouse_fill_caps(evdev_dev_t *d)
{
    d->kind_bits |= (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_REL);

    d->key_bits[BTN_LEFT >> 4] |= (uint16_t)(1u << (BTN_LEFT & 15));
    d->key_bits[BTN_RIGHT >> 4] |= (uint16_t)(1u << (BTN_RIGHT & 15));
    d->key_bits[BTN_MIDDLE >> 4] |= (uint16_t)(1u << (BTN_MIDDLE & 15));

    d->rel_bits[REL_X >> 4] |= (uint16_t)(1u << (REL_X & 15));
    d->rel_bits[REL_Y >> 4] |= (uint16_t)(1u << (REL_Y & 15));
}

void ps2_mouse_handle_byte(uint8_t b)
{
    static uint8_t pkt[3];
    static int     n;

    /* Every first packet byte has bit 3 set; anything else (command ACKs
     * like 0xFA that arrive after the init drain, resends) would shift the
     * three-byte state machine out of phase forever, so resync here. */
    if (n == 0 && !(b & 0x08))
        return;
    pkt[n++] = b;
    if (n < 3)
        return;
    n = 0;

    uint8_t flags = pkt[0];
    int8_t  dx    = (int8_t)pkt[1];
    int8_t  dy    = (int8_t)pkt[2];

    evdev_dev_t *d = evdev_mouse();
    evdev_push_event(d, EV_KEY, BTN_LEFT, flags & 0x01);
    evdev_push_event(d, EV_KEY, BTN_RIGHT, (flags >> 1) & 0x01);
    evdev_push_event(d, EV_KEY, BTN_MIDDLE, (flags >> 2) & 0x01);
    evdev_push_event(d, EV_REL, REL_X, dx);
    /* The PS/2 device reports "up = positive" while evdev's REL_Y follows
     * the screen convention "down = positive", so the sign flips here. */
    evdev_push_event(d, EV_REL, REL_Y, -dy);
    evdev_push_syn(d);
    evdev_signal();
}
