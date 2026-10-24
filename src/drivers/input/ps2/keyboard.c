/* SPDX-License-Identifier: GPL-2.0 */
/*
 * keyboard.c — scancode set 1 to Linux KEY_* decoding. (GPLv2)
 *
 * The identity mapping is the gift that keeps on giving: for non-extended
 * make codes 0x01..0x53 the set-1 scan code IS the evdev code (KEY_ESC=1,
 * KEY_1=2, ..., KEY_SPACE=57, KEY_CAPSLOCK=58, KEY_F1..F12=59..69+18,
 * KEY_NUMLOCK=69, KEY_KP7..KPDOT=71..83).  Only the few stragglers and
 * every 0xE0-prefixed key need a table.
 *
 * This decoder is deliberately parallel to tty.c's: the terminal needs a
 * CJK-aware map with hotkeys and a line discipline, evdev clients need
 * raw key codes.  The two keep independent 0xE0 state.
 */
#include "ps2_keyboard.h"
#include "input_event.h"

static uint16_t scancode_to_key(uint8_t sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x1C:
            return 96; /* keypad enter */
        case 0x1D:
            return 97; /* right ctrl */
        case 0x35:
            return 98; /* keypad / */
        case 0x38:
            return 100; /* right alt */
        case 0x47:
            return 102; /* home */
        case 0x48:
            return 103; /* up */
        case 0x49:
            return 104; /* page up */
        case 0x4B:
            return 105; /* left */
        case 0x4D:
            return 106; /* right */
        case 0x4F:
            return 107; /* end */
        case 0x50:
            return 108; /* down */
        case 0x51:
            return 109; /* page down */
        case 0x52:
            return 110; /* insert */
        case 0x53:
            return 111; /* delete */
        case 0x5B:
            return 125; /* left meta */
        case 0x5C:
            return 126; /* right meta */
        case 0x5D:
            return 127; /* menu */
        }
        return KEY_RESERVED;
    }
    switch (sc) {
    case 0x54:
        return 99; /* print screen / sysrq (KEY_SYSRQ) */
    case 0x55:
        return KEY_RESERVED; /* unused in set 1 */
    case 0x56:
        return 86; /* the 102nd key on ISO keyboards */
    case 0x57:
        return 87; /* f11 */
    case 0x58:
        return 88; /* f12 */
    }
    return sc;
}

void ps2_keyboard_fill_caps(evdev_dev_t *d)
{
    d->kind_bits |= (1u << EV_SYN) | (1u << EV_KEY) | (1u << EV_LED);
    d->led_bits = (uint16_t)((1u << LED_NUML) | (1u << LED_CAPSL) |
                             (1u << LED_SCROLLL));

    for (uint16_t sc = 1; sc <= 0x58; sc++) {
        uint16_t key = scancode_to_key((uint8_t)sc, 0);
        if (key && key < 1024)
            d->key_bits[key >> 4] |= (uint16_t)(1u << (key & 15));
    }

    static const uint16_t extkeys[] = {
        96, 97, 98, 100, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 125, 126, 127,
    };
    for (uint32_t i = 0; i < sizeof(extkeys) / sizeof(extkeys[0]); i++)
        d->key_bits[extkeys[i] >> 4] |= (uint16_t)(1u << (extkeys[i] & 15));
}

void ps2_keyboard_handle_byte(uint8_t sc)
{
    static int ext;
    static int pause_state; /* 0 none, 1 after E1, 2 after E1 1D, 4 after E1 9D */

    /* Pause is the one key sent as a multi-byte E1 sequence:
     * E1 1D 45 press, E1 9D C5 release.  Swallowing only the byte after
     * E1 (the old code) let 0x45 through as a phantom NumLock. */
    if (pause_state) {
        if (pause_state == 1) {
            if (sc == 0x1D) {
                pause_state = 2;
            } else if (sc == 0x9D) {
                pause_state = 4;
            } else {
                pause_state = 0;
            }
            return;
        }

        int press   = (pause_state == 2 && sc == 0x45);
        int release = (pause_state == 4 && sc == 0xC5);
        pause_state = 0;
        if (press || release) {
            evdev_dev_t *d = evdev_keyboard();
            evdev_push_event(d, EV_KEY, KEY_PAUSE, press ? 1 : 0);
            evdev_push_syn(d);
            evdev_signal();
        }
        return;
    }
    if (sc == 0xE1) {
        pause_state = 1;
        return;
    }
    if (sc == 0xE0) {
        ext = 1;
        return;
    }

    uint16_t key = scancode_to_key(sc & 0x7F, ext);
    ext          = 0;
    if (!key)
        return;

    evdev_dev_t *d = evdev_keyboard();
    evdev_push_event(d, EV_KEY, key, (sc & 0x80) ? 0 : 1);
    evdev_push_syn(d);
    evdev_signal();
}
