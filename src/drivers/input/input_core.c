/* SPDX-License-Identifier: GPL-2.0 */
/*
 * input_core.c — binding for the evdev and PS/2 halves. (GPLv2)
 *
 * The transports fill the capability maps; evdev owns the nodes; this
 * file wires them together and exposes the small kernel-internal API
 * (pull, USB HID injection, inputinject) that other subsystems use.
 */
#include "input.h"
#include "evdev.h"
#include "ps2.h"
#include "ps2_keyboard.h"
#include "ps2_mouse.h"
#include "input_event.h"
#include "debugcon.h"

void input_init(void)
{
    evdev_dev_t *kbd   = evdev_keyboard();
    evdev_dev_t *mouse = evdev_mouse();

    ps2_keyboard_fill_caps(kbd);
    ps2_mouse_fill_caps(mouse);

    if (evdev_init() != 0) {
        dbg_puts("INPUT: failed to publish /dev/input/eventN\n");
        return;
    }

    ps2_init();
    dbg_puts("INPUT: /dev/input/event0 keyboard, event1 mouse\n");
}

/* ---- the keyboard hook tty.c calls for every PS/2 byte ------------------ */
void input_kbd_scancode(uint8_t sc)
{
    ps2_keyboard_handle_byte(sc);
}

int input_pull(int mouse, uint16_t *type, uint16_t *code, int32_t *value)
{
    return evdev_pull(mouse ? evdev_mouse() : evdev_keyboard(), type, code, value);
}

/* ---- USB HID injection --------------------------------------------------
 * usb_hid.c decodes boot-protocol reports and hands the decoded results
 * here; these push into the same queues the PS/2 side feeds, so evdev
 * clients get one coherent keyboard and one coherent mouse regardless of
 * which transport produced the event.
 */
void input_usb_kbd(uint16_t key, int pressed)
{
    evdev_dev_t *d = evdev_keyboard();
    evdev_push_event(d, EV_KEY, key, pressed);
    evdev_push_syn(d);
    evdev_signal();
}

void input_usb_mouse(uint16_t btn_mask, int dx, int dy)
{
    evdev_dev_t *d = evdev_mouse();
    evdev_push_event(d, EV_KEY, BTN_LEFT, btn_mask & 0x01);
    evdev_push_event(d, EV_KEY, BTN_RIGHT, (btn_mask >> 1) & 0x01);
    evdev_push_event(d, EV_KEY, BTN_MIDDLE, (btn_mask >> 2) & 0x01);
    evdev_push_event(d, EV_REL, REL_X, dx);
    evdev_push_event(d, EV_REL, REL_Y, dy);
    evdev_push_syn(d);
    evdev_signal();
}

/* inputinject(405): push a synthetic event into event0/event1 for
 * headless tests.  Relative motion and button codes belong to the mouse
 * device; the X server's pointer sprite only materialises once the
 * pointer has actually moved. */
int64_t sys_inputinject(uint64_t type, uint64_t code, uint64_t value)
{
    if (type > 0xFF || code > 0xFFFF)
        return -E_INVAL;

    evdev_dev_t *d;
    if (type == EV_REL || (code >= BTN_LEFT && code <= BTN_TASK))
        d = evdev_mouse();
    else
        d = evdev_keyboard();

    evdev_push_event(d, (uint16_t)type, (uint16_t)code, (int32_t)value);
    evdev_push_syn(d);
    evdev_signal();
    return 0;
}
