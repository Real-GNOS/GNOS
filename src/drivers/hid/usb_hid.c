/*
 * usb_hid.c — USB HID boot-protocol keyboard and mouse. (GPLv2)
 *
 * Sits on the xHCI transfer primitives: after the controller has addressed
 * a device, this driver reads its configuration descriptor, finds a HID
 * interface (class 3) with a boot-protocol keyboard (protocol 1) or mouse
 * (protocol 2), switches it to boot protocol, and arms its interrupt IN
 * endpoint.  Reports are reaped by usb_hid_poll(), which is called from the
 * timer tick like net_tick(); the decoded results go into the evdev queues
 * through input_usb_kbd()/input_usb_mouse().
 *
 * The keyboard report is the standard boot format: modifier byte, reserved,
 * then six current key usages (0 = empty).  Releases are detected by
 * diffing against the previous report, and the modifier byte is diffed
 * separately so Ctrl/Shift/Alt transitions are their own EV_KEY events.
 * The mouse boot report is three bytes (buttons, dx, dy), with an optional
 * fourth wheel byte some devices send.
 */
#include <stdint.h>

#include "xhci.h"
#include "input.h"
#include "kstring.h"
#include "heap.h"
#include "debugcon.h"
#include "subsys.h"

#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

#define HID_CLASS            0x03
#define HID_PROTOCOL_KEYBOARD 1
#define HID_PROTOCOL_MOUSE    2

#define USB_RT_HID  (0x21)                    /* class, interface, device->host */

#define HID_MAX_DEVICES 4

/* ---- HID usage -> Linux evdev keycode --------------------------------------
 * The standard boot keyboard usage table (Linux hid-input's hid_keyboard[]):
 * usages 0x04..0x66 map to KEY_A..KEY_COMPOSE with a couple of holes.  The
 * table is indexed by usage - 4; 0 means "no key". */
static const uint16_t hid_keytab[0x66 - 0x04 + 1] = {
/* 04 a */ 30, /* 05 b */ 48, /* 06 c */ 46, /* 07 d */ 32,
/* 08 e */ 18, /* 09 f */ 33, /* 0a g */ 34, /* 0b h */ 35,
/* 0c i */ 23, /* 0d j */ 36, /* 0e k */ 37, /* 0f l */ 38,
/* 10 m */ 50, /* 11 n */ 49, /* 12 o */ 24, /* 13 p */ 25,
/* 14 q */ 16, /* 15 r */ 19, /* 16 s */ 31, /* 17 t */ 20,
/* 18 u */ 22, /* 19 v */ 47, /* 1a w */ 17, /* 1b x */ 45,
/* 1c y */ 21, /* 1d z */ 44, /* 1e 1 */ 2,  /* 1f 2 */ 3,
/* 20 3 */ 4,  /* 21 4 */ 5,  /* 22 5 */ 6,  /* 23 6 */ 7,
/* 24 7 */ 8,  /* 25 8 */ 9,  /* 26 9 */ 10, /* 27 0 */ 11,
/* 28 ent */ 28, /* 29 esc */ 1, /* 2a bs */ 14, /* 2b tab */ 15,
/* 2c spc */ 57, /* 2d - */ 12, /* 2e = */ 13, /* 2f [ */ 26,
/* 30 ] */ 27, /* 31 \ */ 43, /* 32 (non-US) */ 0, /* 33 ; */ 39,
/* 34 ' */ 40, /* 35 ` */ 41, /* 36 , */ 51, /* 37 . */ 52,
/* 38 / */ 53, /* 39 caps */ 58, /* 3a F1 */ 59, /* 3b F2 */ 60,
/* 3c F3 */ 61, /* 3d F4 */ 62, /* 3e F5 */ 63, /* 3f F6 */ 64,
/* 40 F7 */ 65, /* 41 F8 */ 66, /* 42 F9 */ 67, /* 43 F10 */ 68,
/* 44 F11 */ 87, /* 45 F12 */ 88, /* 46 prtsc */ 99, /* 47 sclk */ 70,
/* 48 pause */ 119, /* 49 ins */ 110, /* 4a home */ 102, /* 4b pgup */ 104,
/* 4c del */ 111, /* 4d end */ 107, /* 4e pgdn */ 109, /* 4f right */ 106,
/* 50 left */ 105, /* 51 down */ 108, /* 52 up */ 103, /* 53 numlk */ 69,
/* 54 KP/ */ 98, /* 55 KP* */ 55, /* 56 KP- */ 74, /* 57 KP+ */ 78,
/* 58 KPent */ 96, /* 59 KP1 */ 79, /* 5a KP2 */ 80, /* 5b KP3 */ 81,
/* 5c KP4 */ 75, /* 5d KP5 */ 76, /* 5e KP6 */ 77, /* 5f KP7 */ 71,
/* 60 KP8 */ 72, /* 61 KP9 */ 73, /* 62 KP0 */ 82, /* 63 KP. */ 83,
/* 64 (non-US \|) */ 0, /* 65 app */ 127, /* 66 power */ 116,
};

/* The modifier usages appear in the report's modifier byte, one bit each. */
static const uint16_t hid_modifier_keys[8] = {
    29,   /* 0x01 LCtrl  -> KEY_LEFTCTRL  */
    42,   /* 0x02 LShift -> KEY_LEFTSHIFT */
    56,   /* 0x04 LAlt   -> KEY_LEFTALT   */
    125,  /* 0x08 LGui   -> KEY_LEFTMETA  */
    97,   /* 0x10 RCtrl  -> KEY_RIGHTCTRL */
    54,   /* 0x20 RShift -> KEY_RIGHTSHIFT*/
    100,  /* 0x40 RAlt   -> KEY_RIGHTALT  */
    126,  /* 0x80 RGui   -> KEY_RIGHTMETA */
};

typedef struct {
    int slot;
    int type;                    /* HID_PROTOCOL_KEYBOARD / HID_PROTOCOL_MOUSE */
    int ep_addr;                 /* interrupt IN endpoint address (0x81) */
    uint8_t  ep_packet;          /* endpoint max packet size */
    uint8_t  report[8];
    uint8_t  last_report[8];
    uint8_t  last_mod;
    uint8_t  active;
} hid_dev_t;

static hid_dev_t g_hid[HID_MAX_DEVICES];
static int g_hid_count;
static int g_hid_ready;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int hid_set_protocol(int slot, int iface, uint8_t protocol)
{
    return xhci_control_transfer(slot, USB_RT_HID, HID_REQ_SET_PROTOCOL,
                                 protocol, (uint16_t)iface, NULL, 0);
}

static int hid_set_idle(int slot, int iface)
{
    return xhci_control_transfer(slot, USB_RT_HID, HID_REQ_SET_IDLE,
                                 0, (uint16_t)iface, NULL, 0);
}

/* Find the first HID interface in the configuration descriptor and arm its
 * interrupt IN endpoint.  Returns the endpoint address or 0. */
static int hid_probe_config(hid_dev_t *dev)
{
    int slot = dev->slot;

    uint8_t header[9];
    if (xhci_control_transfer(slot, 0x80, 6, 0x0200, 0,
                              header, sizeof(header)) < 0)
        return 0;
    uint16_t total = rd16(header + 2);
    if (total < sizeof(header) || total > 4096)
        return 0;

    uint8_t *cfg = kmalloc(total);
    if (!cfg)
        return 0;
    int ret = xhci_control_transfer(slot, 0x80, 6, 0x0200, 0, cfg, total);
    if (ret < (int)sizeof(header)) {
        kfree(cfg);
        return 0;
    }
    uint16_t got = ret < total ? (uint16_t)ret : total;

    int current_hid = 0;
    int interface_num = 0;
    int ep_addr = 0;
    uint8_t ep_packet = 0;

    for (uint16_t off = 0; off + 2 <= got; ) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > got)
            break;

        if (type == 4 && len >= 9) {          /* interface descriptor */
            current_hid = cfg[off + 5] == HID_CLASS &&
                          cfg[off + 7] == dev->type;
            if (current_hid)
                interface_num = cfg[off + 2];
        } else if (type == 5 && len >= 7 && current_hid &&
                   (cfg[off + 3] & 0x03) == 0x03) {   /* interrupt EP */
            uint8_t addr = cfg[off + 2];
            if (addr & 0x80) {                 /* IN endpoint */
                ep_addr = addr;
                ep_packet = (uint8_t)(rd16(cfg + off + 4) & 0x07FFu);
                break;
            }
        }
        off += len;
    }
    kfree(cfg);

    if (!ep_addr)
        return 0;

    /* Switch to boot protocol and stop the idle rate so reports only
     * arrive on change -- the two things real firmware does. */
    hid_set_protocol(slot, interface_num, (uint8_t)dev->type);
    hid_set_idle(slot, interface_num);

    uint64_t buf_phys = 0;
    uint8_t *buf = (uint8_t *)xhci_alloc_page(&buf_phys);
    if (!buf)
        return 0;

    dev->ep_addr = ep_addr;
    dev->ep_packet = ep_packet ? ep_packet : 8;

    /* The xHCI core owns the persistent report buffer. */
    if (xhci_setup_interrupt_in((uint32_t)slot, ep_addr,
                                dev->ep_packet, 10,
                                buf, buf_phys) < 0)
        return 0;

    memset(dev->last_report, 0xFF, sizeof(dev->last_report));
    dev->last_mod = 0xFF;
    dev->active = 1;
    return ep_addr;
}

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
        if (!seen && (uint32_t)(usage - 4) <
                     sizeof(hid_keytab) / sizeof(hid_keytab[0])) {
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
        if (!still && (uint32_t)(usage - 4) <
                      sizeof(hid_keytab) / sizeof(hid_keytab[0])) {
            uint16_t key = hid_keytab[usage - 4];
            if (key)
                input_usb_kbd(key, 0);
        }
    }
}

static void hid_emit_mouse(hid_dev_t *dev)
{
    uint8_t buttons = dev->report[0];
    int8_t dx = (int8_t)dev->report[1];
    int8_t dy = (int8_t)dev->report[2];
    /* Drop spurious 255-delta reports (some mice report a single -1 on
     * first contact), keep everything else. */
    if (dx == -1 && dy == -1 && dev->last_report[0] == 0 &&
        dev->last_report[1] == 0 && dev->last_report[2] == 0)
        dx = dy = 0;
    input_usb_mouse(buttons, dx, dy);
}

/* ---- periodic poll -----------------------------------------------------------
 * Called from the timer tick.  The xHCI core re-arms interrupt IN
 * endpoints after every transfer event, so reports accumulate in the
 * persistent per-endpoint buffer; here we pull whatever is fresh and
 * decode it.
 */
void usb_hid_poll(void)
{
    if (!g_hid_ready)
        return;

    for (int i = 0; i < g_hid_count; i++) {
        hid_dev_t *dev = &g_hid[i];
        if (!dev->active)
            continue;

        uint8_t buf[8];
        int ret = xhci_read_interrupt_report(dev->slot, dev->ep_addr,
                                             buf, dev->ep_packet);
        if (ret < 0)
            continue;

        memcpy(dev->last_report, dev->report, sizeof(dev->report));
        memcpy(dev->report, buf, ret > 8 ? 8 : (uint32_t)ret);

        if (dev->type == HID_PROTOCOL_KEYBOARD)
            hid_emit_kbd(dev);
        else
            hid_emit_mouse(dev);
    }
}

/* Probe every addressed device for a HID keyboard or mouse. */
void usb_hid_init(void)
{
    int count = xhci_device_count();
    for (int i = 0; i < count && g_hid_count < HID_MAX_DEVICES; i++) {
        int slot = xhci_device_slot(i);
        if (slot < 0)
            continue;

        /* Device descriptor: class 0 means "per-interface", which is what
         * keyboards and mice use. */
        usb_device_desc_t dd;
        uint8_t raw[18];
        if (xhci_control_transfer(slot, 0x80, 6, 0x0100, 0, raw, 18) < 0)
            continue;
        memcpy(&dd, raw, 18);

        if (dd.bDeviceClass != 0)
            continue;

        /* Keyboard first (protocol 1), then mouse (protocol 2). */
        for (int proto = 1; proto <= 2; proto++) {
            hid_dev_t *dev = &g_hid[g_hid_count];
            memset(dev, 0, sizeof(*dev));
            dev->slot = slot;
            dev->type = proto;
            if (hid_probe_config(dev)) {
                dbg_puts(proto == HID_PROTOCOL_KEYBOARD
                         ? "USBHID: keyboard on slot "
                         : "USBHID: mouse on slot ");
                dbg_puts_dec(slot);
                dbg_puts("\r\n");
                g_hid_count++;
            }
        }
    }
    g_hid_ready = 1;
}
