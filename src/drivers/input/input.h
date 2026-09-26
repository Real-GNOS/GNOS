/*
 * input.h — the evdev UAPI driver interface. (GPLv2)
 *
 * Two hooks escape this driver into the rest of the kernel: the scancode
 * tap tty.c feeds for every PS/2 keyboard byte, and the O_NONBLOCK mirror
 * syscall.c applies through fcntl(F_SETFL).  Everything else -- the evdev
 * queues, the EVIOC* ioctls, the PS/2 mouse -- stays behind input.c.
 */
#ifndef GNUCOS_INPUT_H
#define GNUCOS_INPUT_H

#include <stdint.h>
#include "vfs.h"

/* Publish /dev/input/event0 (keyboard) and event1 (mouse), enable the
 * auxiliary PS/2 port and its IRQ.  Call after tty_init(), which enables
 * the keyboard port this driver shares. */
void input_init(void);

/* Feed one raw PS/2 keyboard byte into the evdev decoder.  Called from
 * tty.c's kbd_feed, which decodes the same byte for the terminal; the two
 * decoders are independent, each with its own 0xE0 state. */
void input_kbd_scancode(uint8_t sc);

/* Mirror fcntl(F_SETFL, O_NONBLOCK) onto an evdev node.  No-op for nodes
 * this driver does not own. */
void input_set_nonblock(vfs_node_t *n, int nb);

/* Feed decoded USB HID keyboard/mouse events into the evdev queues.  The
 * USB HID driver (usb_hid.c) turns boot-protocol reports into these
 * *results* -- a key code + pressed flag, or mouse deltas + button mask --
 * exactly the shape the PS/2 decoder produces, so libinput sees one
 * coherent device no matter which wire the event came over. */
/* Non-blocking pop of one decoded event (kernel-internal consumers, e.g.
 * the NEP shim's get_event).  mouse selects the device: 0 keyboard,
 * 1 mouse.  Returns 1 and fills the out params, or 0 when the queue is
 * empty. */
int input_pull(int mouse, uint16_t *type, uint16_t *code, int32_t *value);

void input_usb_kbd(uint16_t key, int pressed);
void input_usb_mouse(uint16_t btn_mask, int dx, int dy);

#endif
