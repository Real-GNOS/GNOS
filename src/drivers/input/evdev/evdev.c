/* SPDX-License-Identifier: GPL-2.0 */
/*
 * evdev.c — the Linux evdev UAPI on top of a per-device event ring. (GPLv2)
 *
 * This is the VFS half of the input stack: read(), poll(), the EVIOC*
 * ioctls libinput issues at open time, and the capability maps the
 * transports fill in.  The old driver kept all of this, the PS/2 decoder
 * and the i8042 controller in one 640-line file; the split follows
 * Uinxed's evdev/ transport-neutral layout.
 */
#include "evdev.h"
#include "input.h"
#include "input_event.h"
#include "proc.h"
#include "timer.h"
#include "kstring.h"
#include "chrdev.h"

static const vfs_ops_t g_evdev_ops;

/* ---- the two built-in devices ----------------------------------------- */
static evdev_dev_t g_kbd_dev = {
    .name    = "GNOS i8042 keyboard",
    .vendor  = 0x0001,
    .product = 0x0001,
};
static evdev_dev_t g_mouse_dev = {
    .name    = "GNOS i8042 mouse",
    .vendor  = 0x0002,
    .product = 0x0002,
};

evdev_dev_t *evdev_keyboard(void)
{
    return &g_kbd_dev;
}

evdev_dev_t *evdev_mouse(void)
{
    return &g_mouse_dev;
}

int evdev_init(void)
{
    evdev_queue_init(&g_kbd_dev.q);
    evdev_queue_init(&g_mouse_dev.q);

    if (cdev_add_vfs("input", "event0", 13, 64, 1, 0666, 0, 0, &g_evdev_ops, &g_kbd_dev) != 0 ||
        cdev_add_vfs("input", "event1", 13, 65, 1, 0666, 0, 0, &g_evdev_ops, &g_mouse_dev) != 0)
        return -1;
    return 0;
}

void evdev_push_event(evdev_dev_t *d, uint16_t type, uint16_t code, int32_t value)
{
    input_event_t ev;

    ev.tv_sec  = (int64_t)(timer_ticks() / SCHED_HZ + timer_boot_epoch());
    ev.tv_usec = (int64_t)((timer_ticks() % SCHED_HZ) * (1000000ULL / SCHED_HZ));
    ev.type    = type;
    ev.code    = code;
    ev.value   = value;

    /* Keep the "currently held" bitmap in sync: EVIOCGKEY reports it to
     * clients that attach while a key is already down (X server startup),
     * and a stale all-zero answer leaves that key stuck in the wrong
     * state until it is released. */
    if (type == EV_KEY && code < 1024) {
        if (value)
            d->key_state[code >> 4] |= (uint16_t)(1u << (code & 15));
        else
            d->key_state[code >> 4] &= (uint16_t) ~(1u << (code & 15));
    }

    /* A full ring drops the event, exactly like Linux's evdev overflow. */
    (void)evdev_queue_push(&d->q, &ev);
}

void evdev_push_syn(evdev_dev_t *d)
{
    evdev_push_event(d, EV_SYN, SYN_REPORT, 0);
}

/* A queued event is exactly what a poll() sleeper was waiting for.  The
 * poll core parks evdev waiters in the WAIT_PIPE channel (see do_ppoll),
 * so waking that channel here is what makes blocking poll()/read() return. */
void evdev_signal(void)
{
    /* Xorg waits in epoll on event0/event1; a new packet must wake both
     * the blocking readers (WAIT_PIPE) *and* the poll/epoll channels, or
     * the pointer keeps its old position until some other fd happens to
     * wake the server.  Same deal as unix.c's ring_wake. */
    sched_wake_reason(WAIT_PIPE);
    sched_wake_poll_channels();
}

int evdev_pull(evdev_dev_t *d, uint16_t *type, uint16_t *code, int32_t *value)
{
    input_event_t ev;

    asm volatile("cli");
    if (evdev_queue_count(&d->q) == 0) {
        asm volatile("sti");
        return 0;
    }
    evdev_queue_pop(&d->q, &ev, 1);
    asm volatile("sti");

    *type  = ev.type;
    *code  = ev.code;
    *value = ev.value;
    return 1;
}

/* ---- read / poll ------------------------------------------------------ */
static int32_t evdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    evdev_dev_t *d    = (evdev_dev_t *)n->priv;
    uint32_t     want = len / sizeof(input_event_t);

    if (want == 0)
        return 0; /* whole events only */

    /* Test-and-sleep atomic against the IRQ, the same cli/schedule dance
     * pipe_node_read uses. */
    asm volatile("cli");
    while (evdev_queue_count(&d->q) == 0) {
        proc_t *me = proc_current();
        if (d->nonblock) {
            asm volatile("sti");
            return -E_AGAIN;
        }
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t count = evdev_queue_count(&d->q);
    uint32_t nout  = want < count ? want : count;
    evdev_queue_pop_to(&d->q, buf, nout);
    asm volatile("sti");
    return (int32_t)(nout * sizeof(input_event_t));
}

static int evdev_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    evdev_dev_t *d = (evdev_dev_t *)n->priv;
    int16_t      r = 0;

    if ((events & POLLIN) && evdev_queue_count(&d->q))
        r |= POLLIN;
    *revents = r;
    return 0;
}

/* fcntl(F_SETFL, O_NONBLOCK) mirror: libevdev flips the flag through fcntl
 * as often as at open.  Open-time O_NONBLOCK is not plumbed to the node by
 * the VFS, which is fine -- every real client (libinput, evtest) uses
 * poll() to gate reads anyway, and the mirror keeps those that fcntl. */
void input_set_nonblock(vfs_node_t *n, int nb)
{
    if (n->priv == &g_kbd_dev || n->priv == &g_mouse_dev)
        ((evdev_dev_t *)n->priv)->nonblock = nb;
}

/* ---- EVIOC* ioctls (the Linux numbers) -------------------------------- */
#define EVIOCGVERSION      0x80044501
#define EVIOCGID           0x80084502
#define EVIOCGNAME(len)    (0x80000000 | ((len) << 16) | 0x4506)
#define EVIOCGBIT(ev, len) (0x80000000 | ((len) << 16) | (0x45 << 8) | 0x20)
#define EVIOCGRAB          0x40044590
#define EVIOCSCLOCKID      0x400445A0

static int32_t evdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    evdev_dev_t *d = (evdev_dev_t *)n->priv;
    uint64_t     u = arg;

    switch (cmd) {
    case EVIOCGVERSION: {
        if (!user_ptr_ok(u, 4))
            return -E_FAULT;
        uint32_t v = (EV_VERSION_MAJOR << 16) | (EV_VERSION_MINOR << 8) | EV_VERSION_PATCH;
        memcpy((void *)(uintptr_t)u, &v, 4);
        return 0;
    }
    case EVIOCGID: {
        if (!user_ptr_ok(u, 8))
            return -E_FAULT;
        /* struct input_id { bus, vendor, product, version }: 4 shorts. */
        uint16_t id[4] = {BUS_I8042, d->vendor, d->product, 0x0100};
        memcpy((void *)(uintptr_t)u, id, sizeof(id));
        return 0;
    }
    case EVIOCGRAB:
        /* Linux passes the grab flag *by value*, not through a pointer:
         * libevdev calls ioctl(fd, EVIOCGRAB, 1).  Dereferencing the
         * value as an address made every grab fail with EFAULT. */
        d->grabbed = (int)(u != 0);
        return 0;
    case EVIOCSCLOCKID:
        return 0; /* we always report realtime */
    }
    /* Repeat-rate get/set and keycode set: Xorg's evdev driver programs
     * these on every keyboard open; refusing them is logged as a device
     * error.  EVIOCSREP is _IOW('E', 0x03) and EVIOCSKEYCODE is
     * _IOW('E', 0x04) -- the old test only matched 0x04 and silently
     * rejected the repeat-rate write. */
    if ((cmd >> 30) == 1 /* _IOC_WRITE */ && ((cmd & 0xFF) == 0x03 || (cmd & 0xFF) == 0x04))
        return 0;
    if ((cmd >> 30) == 2 /* _IOC_READ */ && (cmd & 0xFF) == 0x03) {
        uint16_t rep[2] = {250, 33}; /* delay ms, period ms */
        if (!user_ptr_ok(u, sizeof(rep)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)u, rep, sizeof(rep));
        return 0;
    }
    /* EVIOCGNAME(len) and EVIOCGBIT(ev,len) carry their length in the
     * command word; decode it like Linux's _IOC_SIZE.  EVIOCGBIT puts the
     * event type in the *nr* field: 0x20 + ev. */
    unsigned dir  = (unsigned)(cmd >> 30);
    unsigned size = (unsigned)((cmd >> 16) & 0x3FFF);
    unsigned nr   = (unsigned)(cmd & 0xFF);

    if (dir == 2 /* _IOC_READ */) {
        if (nr == 0x06) { /* EVIOCGNAME */
            const char *name = d->name;
            uint32_t    len  = (uint32_t)size;
            if (!user_ptr_ok(u, len))
                return -E_FAULT;
            /* Linux returns the string including its NUL; libevdev reads
             * it as a C string, so a non-terminated copy is a bug even
             * when the name fits exactly. */
            uint32_t n = (uint32_t)strlen(name);
            if (n + 1 > len) {
                n = len ? len - 1 : 0;
            }
            memcpy((void *)(uintptr_t)u, name, n);
            if (len)
                ((char *)(uintptr_t)u)[n] = 0;
            return (int32_t)(n + 1);
        }
        /* GBIT covers every event type 0..EV_MAX, so nr runs 0x20..0x3F:
         * libevdev probes each type in turn (including EV_FF at 0x35) and
         * a rejected query aborts the whole device init. */
        if (nr >= 0x20 && nr < 0x40) { /* EVIOCGBIT(ev, len) */
            unsigned ev  = nr - 0x20;
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len))
                return -E_FAULT;
            memset((void *)(uintptr_t)u, 0, len);
            uint16_t *bits  = NULL;
            uint32_t  nbits = 0;
            if (ev == EV_SYN) {
                /* The SYN query asks "which event types does this device
                 * emit?" as a plain Linux bitmap: bit T set = type T.  The
                 * kernel-side map is already exactly that bitmap. */
                uint32_t bytes = (EV_CNT + 7) / 8;
                if (bytes > len)
                    bytes = len;
                for (unsigned t = 0; t < EV_CNT && (t >> 3) < bytes; t++)
                    if (d->kind_bits & (1u << t))
                        ((uint8_t *)(uintptr_t)u)[t >> 3] |= (uint8_t)(1u << (t & 7));
                return 0;
            } else if (ev == EV_KEY) {
                bits  = d->key_bits;
                nbits = 1024;
            } else if (ev == EV_REL) {
                bits  = d->rel_bits;
                nbits = 32;
            } else if (ev == EV_LED) {
                bits  = &d->led_bits;
                nbits = 16;
            }
            if (bits) {
                uint32_t bytes = (nbits + 7) / 8;
                if (bytes > len)
                    bytes = len;
                memcpy((void *)(uintptr_t)u, bits, bytes);
            }
            return 0;
        }
        if (nr == 0x07 || nr == 0x08) { /* EVIOCGPHYS / EVIOCGUNIQ: none */
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len))
                return -E_FAULT;
            if (len)
                ((char *)(uintptr_t)u)[0] = 0;
            return 0;
        }
        if (nr == 0x18) { /* EVIOCGKEY: currently held keys */
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len))
                return -E_FAULT;
            memset((void *)(uintptr_t)u, 0, len);
            uint32_t bytes = sizeof(d->key_state);
            if (bytes > len)
                bytes = len;
            memcpy((void *)(uintptr_t)u, d->key_state, bytes);
            return 0;
        }
        if (nr == 0x09 || nr == 0x19 || nr == 0x1a || nr == 0x1b) {
            /* EVIOCGPROP / EVIOCGLED / EVIOCGSND / EVIOCGSW: state
             * queries libevdev makes while initialising the device.  We
             * track no such state (the keyboard has LEDs but nothing
             * drives them yet), so every answer is all-zero -- but the
             * answer must exist, or the query fails and libevdev refuses
             * the whole fd ("Unable to query fd") and Xorg's evdev driver
             * drops the device. */
            uint32_t len = (uint32_t)size;
            if (!user_ptr_ok(u, len))
                return -E_FAULT;
            memset((void *)(uintptr_t)u, 0, len);
            return 0;
        }
    }
    return -E_INVAL;
}

static const vfs_ops_t g_evdev_ops = {
    .read  = evdev_read,
    .ioctl = evdev_ioctl,
    .poll  = evdev_poll,
};
