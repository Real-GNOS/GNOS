/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb_hid.c — USB HID boot-protocol keyboard and mouse. (GPLv2)
 *
 * A class driver on the USB core: usb_add_device() dispatches to
 * usb_hid_probe() for every HID-class interface after the device model has
 * registered it.  The probe requires the boot-protocol subclass, switches
 * the device to boot protocol, and arms its interrupt IN endpoint through
 * usb_interrupt_start(); reports are reaped by usb_hid_poll(), which is
 * called from the timer tick like net_tick() and drives the hcd completion
 * callbacks.  The decoded results go into the evdev queues through
 * input_usb_kbd()/input_usb_mouse().
 *
 * The keyboard report is the standard boot format: modifier byte, reserved,
 * then six current key usages (0 = empty).  Releases are detected by
 * diffing against the previous report, and the modifier byte is diffed
 * separately so Ctrl/Shift/Alt transitions are their own EV_KEY events.
 * The mouse boot report is three bytes (buttons, dx, dy), with an optional
 * fourth wheel byte some devices send.
 */
#include <stdint.h>

#include "usb.h"
#include "xhci.h" /* xhci_usb_reap() */
#include "input.h"
#include "kstring.h"
#include "debugcon.h"

#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

#define HID_CLASS             0x03
#define HID_PROTOCOL_KEYBOARD 1
#define HID_PROTOCOL_MOUSE    2

#define USB_RT_HID (0x21) /* class, interface, device->host */

#define HID_MAX_DEVICES 4

/* ---- HID usage -> Linux evdev keycode --------------------------------------
 * The standard boot keyboard usage table (Linux hid-input's hid_keyboard[]):
 * usages 0x04..0x66 map to KEY_A..KEY_COMPOSE with a couple of holes.  The
 * table is indexed by usage - 4; 0 means "no key". */
static const uint16_t hid_keytab[0x66 - 0x04 + 1] = {
    /* 04 a */ 30,          /* 05 b */ 48,     /* 06 c */ 46,       /* 07 d */ 32,
    /* 08 e */ 18,          /* 09 f */ 33,     /* 0a g */ 34,       /* 0b h */ 35,
    /* 0c i */ 23,          /* 0d j */ 36,     /* 0e k */ 37,       /* 0f l */ 38,
    /* 10 m */ 50,          /* 11 n */ 49,     /* 12 o */ 24,       /* 13 p */ 25,
    /* 14 q */ 16,          /* 15 r */ 19,     /* 16 s */ 31,       /* 17 t */ 20,
    /* 18 u */ 22,          /* 19 v */ 47,     /* 1a w */ 17,       /* 1b x */ 45,
    /* 1c y */ 21,          /* 1d z */ 44,     /* 1e 1 */ 2,        /* 1f 2 */ 3,
    /* 20 3 */ 4,           /* 21 4 */ 5,      /* 22 5 */ 6,        /* 23 6 */ 7,
    /* 24 7 */ 8,           /* 25 8 */ 9,      /* 26 9 */ 10,       /* 27 0 */ 11,
    /* 28 ent */ 28,        /* 29 esc */ 1,    /* 2a bs */ 14,      /* 2b tab */ 15,
    /* 2c spc */ 57,        /* 2d - */ 12,     /* 2e = */ 13,       /* 2f [ */ 26,
    /* 30 ] */ 27,          /* 31 \ */ 43,     /* 32 (non-US) */ 0, /* 33 ; */ 39,
    /* 34 ' */ 40,          /* 35 ` */ 41,     /* 36 , */ 51,       /* 37 . */ 52,
    /* 38 / */ 53,          /* 39 caps */ 58,  /* 3a F1 */ 59,      /* 3b F2 */ 60,
    /* 3c F3 */ 61,         /* 3d F4 */ 62,    /* 3e F5 */ 63,      /* 3f F6 */ 64,
    /* 40 F7 */ 65,         /* 41 F8 */ 66,    /* 42 F9 */ 67,      /* 43 F10 */ 68,
    /* 44 F11 */ 87,        /* 45 F12 */ 88,   /* 46 prtsc */ 99,   /* 47 sclk */ 70,
    /* 48 pause */ 119,     /* 49 ins */ 110,  /* 4a home */ 102,   /* 4b pgup */ 104,
    /* 4c del */ 111,       /* 4d end */ 107,  /* 4e pgdn */ 109,   /* 4f right */ 106,
    /* 50 left */ 105,      /* 51 down */ 108, /* 52 up */ 103,     /* 53 numlk */ 69,
    /* 54 KP/ */ 98,        /* 55 KP* */ 55,   /* 56 KP- */ 74,     /* 57 KP+ */ 78,
    /* 58 KPent */ 96,      /* 59 KP1 */ 79,   /* 5a KP2 */ 80,     /* 5b KP3 */ 81,
    /* 5c KP4 */ 75,        /* 5d KP5 */ 76,   /* 5e KP6 */ 77,     /* 5f KP7 */ 71,
    /* 60 KP8 */ 72,        /* 61 KP9 */ 73,   /* 62 KP0 */ 82,     /* 63 KP. */ 83,
    /* 64 (non-US \|) */ 0, /* 65 app */ 127,  /* 66 power */ 116,
};

/* The modifier usages appear in the report's modifier byte, one bit each. */
static const uint16_t hid_modifier_keys[8] = {
    29,  /* 0x01 LCtrl  -> KEY_LEFTCTRL  */
    42,  /* 0x02 LShift -> KEY_LEFTSHIFT */
    56,  /* 0x04 LAlt   -> KEY_LEFTALT   */
    125, /* 0x08 LGui   -> KEY_LEFTMETA  */
    97,  /* 0x10 RCtrl  -> KEY_RIGHTCTRL */
    54,  /* 0x20 RShift -> KEY_RIGHTSHIFT*/
    100, /* 0x40 RAlt   -> KEY_RIGHTALT  */
    126, /* 0x80 RGui   -> KEY_RIGHTMETA */
};

typedef struct {
    usb_device_t   *udev;     /* owning device (core-registered) */
    usb_endpoint_t *ep;       /* interrupt IN endpoint */
    int             type;     /* HID_PROTOCOL_KEYBOARD / HID_PROTOCOL_MOUSE */
    uint8_t         ep_packet; /* endpoint max packet size */
    uint8_t         report[8];
    uint8_t         last_report[8];
    uint8_t         last_mod;
    uint8_t         active;
} hid_dev_t;

static hid_dev_t g_hid[HID_MAX_DEVICES];
static int       g_hid_count;

static void hid_emit_kbd(hid_dev_t *dev)
{
    uint8_t mod = dev->report[0];

    if (mod != dev->last_mod) {
        for (int i = 0; i < 8; i++) {
            int now = (mod >> i) & 1;
            int was = (dev->last_mod >> i) & 1;
            if (now != was)
                input_usb_kbd(hid_modifier_keys[i], now);
        }
        dev->last_mod = mod;
    }

    for (int n = 2; n < 8; n++) {
        uint8_t usage = dev->report[n];
        if (usage == 0)
            continue;
        int seen = 0;
        for (int m = 2; m < 8; m++)
            if (dev->last_report[m] == usage)
                seen = 1;
        if (!seen && (uint32_t)(usage - 4) < sizeof(hid_keytab) / sizeof(hid_keytab[0])) {
            uint16_t key = hid_keytab[usage - 4];
            if (key)
                input_usb_kbd(key, 1);
        }
    }
    for (int n = 2; n < 8; n++) {
        uint8_t usage = dev->last_report[n];
        if (usage == 0)
            continue;
        int still = 0;
        for (int m = 2; m < 8; m++)
            if (dev->report[m] == usage)
                still = 1;
        if (!still && (uint32_t)(usage - 4) < sizeof(hid_keytab) / sizeof(hid_keytab[0])) {
            uint16_t key = hid_keytab[usage - 4];
            if (key)
                input_usb_kbd(key, 0);
        }
    }
}

static void hid_emit_mouse(hid_dev_t *dev)
{
    uint8_t buttons = dev->report[0];
    int8_t  dx      = (int8_t)dev->report[1];
    int8_t  dy      = (int8_t)dev->report[2];
    /* Drop spurious 255-delta reports (some mice report a single -1 on
     * first contact), keep everything else. */
    if (dx == -1 && dy == -1 && dev->last_report[0] == 0 && dev->last_report[1] == 0 &&
        dev->last_report[2] == 0)
        dx = dy = 0;
    input_usb_mouse(buttons, dx, dy);
}

/* Completion callback, run by xhci_usb_reap() from the timer tick: latch the
 * fresh report and decode it through the same path the old poll loop used. */
static void hid_complete(usb_endpoint_t *endpoint, const void *data, size_t length, int status, void *context)
{
    (void)endpoint;
    (void)status;
    hid_dev_t *dev = (hid_dev_t *)context;
    if (!dev || !dev->active || !data || length < 1)
        return;

    memcpy(dev->last_report, dev->report, sizeof(dev->report));
    memcpy(dev->report, data, length > 8 ? 8 : length);

    if (dev->type == HID_PROTOCOL_KEYBOARD)
        hid_emit_kbd(dev);
    else
        hid_emit_mouse(dev);
}

/* Probe a HID-class interface the USB core has registered.  Only the boot
 * protocol subclass (1) with protocol keyboard (1) or mouse (2) is driven. */
int usb_hid_probe(usb_interface_t *interface)
{
    if (!interface || !interface->device)
        return -E_INVAL;
    if (interface->descriptor.interface_subclass != 1)
        return -E_OPNOTSUPP;
    int proto = interface->descriptor.interface_protocol;
    if (proto != HID_PROTOCOL_KEYBOARD && proto != HID_PROTOCOL_MOUSE)
        return -E_OPNOTSUPP;

    usb_endpoint_t *ep = usb_find_endpoint(interface, USB_ENDPOINT_XFER_INT, true);
    if (!ep)
        return -E_OPNOTSUPP;
    if (g_hid_count >= HID_MAX_DEVICES)
        return -E_NOSPC;

    hid_dev_t *dev = &g_hid[g_hid_count];
    memset(dev, 0, sizeof(*dev));
    dev->udev      = interface->device;
    dev->ep        = ep;
    dev->type      = proto;
    dev->ep_packet = ep->descriptor.max_packet_size ? ep->descriptor.max_packet_size : 8;

    /* Switch to boot protocol and stop the idle rate so reports only
     * arrive on change -- the two things real firmware does. */
    if (usb_control_msg(dev->udev, USB_RT_HID, HID_REQ_SET_PROTOCOL, (uint16_t)proto, interface->descriptor.interface_number, NULL, 0, USB_CTRL_TIMEOUT_MS) != 0)
        return -E_IO;
    (void)usb_control_msg(dev->udev, USB_RT_HID, HID_REQ_SET_IDLE, 0, interface->descriptor.interface_number, NULL, 0, USB_CTRL_TIMEOUT_MS);

    if (usb_interrupt_start(dev->ep, dev->ep_packet, hid_complete, dev) != 0)
        return -E_IO;

    memset(dev->last_report, 0xFF, sizeof(dev->last_report));
    dev->last_mod = 0xFF;
    dev->active   = 1;
    g_hid_count++;

    dbg_puts(proto == HID_PROTOCOL_KEYBOARD ? "USBHID: keyboard on " : "USBHID: mouse on ");
    dbg_puts(dev->udev->path);
    dbg_puts("\r\n");
    return 0;
}

/* Disconnect the class driver from every HID interface of this device. */
void usb_hid_disconnect(usb_interface_t *interface)
{
    if (!interface || !interface->device)
        return;
    for (int i = 0; i < g_hid_count; i++) {
        hid_dev_t *dev = &g_hid[i];
        if (!dev->active || dev->udev != interface->device)
            continue;
        usb_interrupt_stop(dev->ep);
        dev->active = 0;
    }
    /* Compact the table so the slot can be reused by the next device. */
    int kept = 0;
    for (int i = 0; i < g_hid_count; i++) {
        if (g_hid[i].active)
            g_hid[kept++] = g_hid[i];
    }
    g_hid_count = kept;
}

/* ---- periodic poll -----------------------------------------------------------
 * Called from the timer tick.  The hcd shim re-arms interrupt IN endpoints
 * after every transfer event; here we pump the completions, which latch the
 * fresh reports and decode them.
 */
void usb_hid_poll(void)
{
    if (!g_hid_count)
        return;
    xhci_usb_reap();
}
