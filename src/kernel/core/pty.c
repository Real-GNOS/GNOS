/*
 * pty.c — UNIX98 pseudo-terminals: /dev/ptmx + /dev/pts/N. (GPLv2)
 *
 * xterm and every other terminal emulator is a two-endpoint affair: the
 * emulator holds the master (ptmx), the shell holds the slave
 * (/dev/pts/N), and bytes cross through two rings.  Opening /dev/ptmx is
 * a cloning operation -- every open allocates a fresh pair, registers the
 * slave as /dev/pts/N and hands the caller a master descriptor; the
 * open_resolved() hook in syscall.c drives that via pty_reattach_master().
 *
 * What is deliberately missing: a real line discipline.  The slave stores
 * termios for tcgetattr/tcsetattr round-trips (isatty() is a TCGETS
 * probe) and translates LF to CRLF on the slave->master path (OPOST/
 * ONLCR, without which every line stair-steps), but ISIG/^C handling and
 * canonical mode are not implemented.
 */
#include <stdint.h>

#include "pty.h"
#include "proc.h"
#include "kstring.h"
#include "heap.h"
#include "vfs.h"
#include "sysnum.h"

#define PTY_MAX    16
#define PTY_RING   4096

typedef struct {
    int       used;                 /* pair allocated */
    int       slave_registered;     /* /dev/pts/N is in the dev table */
    int       slave_open;           /* an fd holds the slave end */
    uint8_t  *to_slave;             /* master writes, slave reads  */
    uint32_t  sh, st, scount;
    uint8_t  *to_master;            /* slave writes, master reads  */
    uint32_t  mh, mt, mcount;
    int       index;
    termios_t tio;
    winsize_t ws;
} pty_t;

static pty_t g_pty[PTY_MAX];

static void ring_put(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *count,
                     const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        ring[*head] = data[i];
        *head = (*head + 1) % cap;
        (*count)++;
    }
}

static void ring_get(uint8_t *ring, uint32_t cap, uint32_t *tail, uint32_t *count,
                     uint8_t *out, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        out[i] = ring[*tail];
        *tail = (*tail + 1) % cap;
        (*count)--;
    }
}

static void pty_wake(void)
{
    /* Blocking readers sit on WAIT_PIPE; poll/epoll waiters need the poll
     * channels poked as well (same deal as unix.c's ring_wake). */
    sched_wake_reason(WAIT_PIPE);
    sched_wake_poll_channels();
}

/* ---- master end ---------------------------------------------------------- */

static int32_t ptmx_no_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_INVAL;
}

static int32_t pty_master_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    asm volatile("cli");
    while (p->mcount == 0) {
        proc_t *me = proc_current();
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        /* EOF: the slave end was closed and nothing is buffered. */
        if (!p->slave_open) {
            asm volatile("sti");
            return 0;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t nout = (len < p->mcount) ? len : p->mcount;
    ring_get(p->to_master, PTY_RING, &p->mt, &p->mcount, (uint8_t *)buf, nout);
    asm volatile("sti");
    return (int32_t)nout;
}

static int32_t pty_master_write(vfs_node_t *n, uint64_t off, const void *buf,
                                uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    asm volatile("cli");
    while (p->scount == PTY_RING) {
        proc_t *me = proc_current();
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        if (!p->slave_open) {
            asm volatile("sti");
            return -E_PIPE;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t room = PTY_RING - p->scount;
    uint32_t nout = (len < room) ? len : room;
    ring_put(p->to_slave, PTY_RING, &p->sh, &p->scount,
             (const uint8_t *)buf, nout);
    asm volatile("sti");
    pty_wake();
    return (int32_t)nout;
}

static int32_t pty_master_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    switch (cmd) {
    case TIOCGPTN:                     /* int: the pts index */
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = p->index;
        return 0;
    case TIOCSPTLCK:                   /* unlock: accepted, locks unsupported */
        return 0;
    case FIONREAD:                     /* bytes ready for the next read */
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = (int32_t)p->mcount;
        return 0;
    case TIOCGWINSZ:                   /* xterm sizes the pty via the master */
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)arg, &p->ws, sizeof(p->ws));
        return 0;
    case TIOCSWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy(&p->ws, (const void *)(uintptr_t)arg, sizeof(p->ws));
        return 0;
    default:
        return -E_NOTTY;
    }
}

static int evdev_like_ready(uint32_t count, int16_t events, int16_t *revents)
{
    int16_t r = 0;
    if ((events & POLLIN) && count)
        r |= POLLIN;
    if ((events & POLLOUT))
        r |= POLLOUT;
    *revents = r;
    return 0;
}

static int pty_master_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used) {
        *revents = 0;
        return 0;
    }
    /* Hangup looks like readable-with-EOF to a select loop. */
    if (p->mcount == 0 && !p->slave_open) {
        *revents = (int16_t)(events & POLLIN);
        return 0;
    }
    return evdev_like_ready(p->mcount, events, revents);
}

/* ---- slave end ----------------------------------------------------------- */

static int32_t pty_slave_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    asm volatile("cli");
    while (p->scount == 0) {
        proc_t *me = proc_current();
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    uint32_t nout = (len < p->scount) ? len : p->scount;
    ring_get(p->to_slave, PTY_RING, &p->st, &p->scount, (uint8_t *)buf, nout);
    asm volatile("sti");
    return (int32_t)nout;
}

static int32_t pty_slave_write(vfs_node_t *n, uint64_t off, const void *buf,
                               uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    asm volatile("cli");
    for (uint32_t i = 0; i < len; i++) {
        const char c = ((const char *)buf)[i];
        while (p->mcount == PTY_RING) {
            proc_t *me = proc_current();
            if (!me || proc_pending_signals(me)) {
                asm volatile("sti");
                return (int32_t)i;
            }
            sched_block_irqoff(WAIT_PIPE);
        }
        /* OPOST/ONLCR: a bare LF on the slave path renders as CRLF on the
         * emulator, or every line stair-steps down the terminal. */
        if (c == '\n') {
            uint8_t cr = '\r';
            ring_put(p->to_master, PTY_RING, &p->mh, &p->mcount, &cr, 1);
        }
        ring_put(p->to_master, PTY_RING, &p->mh, &p->mcount,
                 (const uint8_t *)&c, 1);
    }
    asm volatile("sti");
    pty_wake();
    return (int32_t)len;
}

static int32_t pty_slave_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;

    switch (cmd) {
    case TCGETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)arg, &p->tio, sizeof(p->tio));
        return 0;
    case TCSETS:
    case TCSETSF:
    case TCSETSW:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        memcpy(&p->tio, (const void *)(uintptr_t)arg, sizeof(p->tio));
        return 0;
    case TIOCGWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)arg, &p->ws, sizeof(p->ws));
        return 0;
    case TIOCSWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy(&p->ws, (const void *)(uintptr_t)arg, sizeof(p->ws));
        return 0;
    case TIOCSCTTY:                    /* become controlling terminal */
        return 0;
    case TIOCGPGRP:
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = proc_current() ? (int32_t)proc_current()->pid : 0;
        return 0;
    default:
        return -E_NOTTY;
    }
}

static int pty_slave_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used) {
        *revents = 0;
        return 0;
    }
    return evdev_like_ready(p->scount, events, revents);
}

/* ---- lifecycle ----------------------------------------------------------- */

/* There is no snprintf in the kernel; "pts/N" is short enough to build
 * by hand. */
static void pts_name(char *dst, size_t cap, int index)
{
    const char *pfx = "pts/";
    size_t      o   = 0;
    while (*pfx && o + 1 < cap)
        dst[o++] = *pfx++;
    char tmp[8];
    int  n = 0;
    int  v = index;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n && o + 1 < cap)
        dst[o++] = tmp[--n];
    dst[o] = '\0';
}

static void pty_pair_release(pty_t *p)
{
    if (!p->used)
        return;
    if (p->slave_registered) {
        char name[16];
        pts_name(name, sizeof(name), p->index);
        vfs_unregister_dev(name);
        p->slave_registered = 0;
    }
    kfree(p->to_slave);
    kfree(p->to_master);
    p->to_slave = p->to_master = NULL;
    p->used = 0;
    pty_wake();
}

static void pty_master_release(vfs_node_t *n)
{
    pty_t *p = (pty_t *)n->priv;
    if (p)
        pty_pair_release(p);
}

static void pty_slave_release(vfs_node_t *n)
{
    pty_t *p = (pty_t *)n->priv;
    if (p)
        p->slave_open = 0;
    pty_wake();
}

static const vfs_ops_t pty_ptmx_ops = {
    .ioctl = pty_master_ioctl,
};

static const vfs_ops_t pty_master_ops = {
    .read    = pty_master_read,
    .write   = pty_master_write,
    .ioctl   = pty_master_ioctl,
    .poll    = pty_master_poll,
    .release = pty_master_release,
};

static const vfs_ops_t pty_slave_ops = {
    .read    = pty_slave_read,
    .write   = pty_slave_write,
    .ioctl   = pty_slave_ioctl,
    .poll    = pty_slave_poll,
    .release = pty_slave_release,
};

/* ---- the cloning open ---------------------------------------------------- */

int pty_is_ptmx(const vfs_node_t *n)
{
    return n && n->ops == &pty_ptmx_ops;
}

int pty_reattach_master(vfs_node_t *n)
{
    pty_t *p = NULL;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_pty[i].used) {
            p = &g_pty[i];
            break;
        }
    }
    if (!p)
        return -E_NOSPC;

    memset(p, 0, sizeof(*p));
    p->to_slave  = kmalloc(PTY_RING);
    p->to_master = kmalloc(PTY_RING);
    if (!p->to_slave || !p->to_master) {
        kfree(p->to_slave);
        kfree(p->to_master);
        return -E_NOMEM;
    }
    p->used  = 1;
    p->index = (int)(p - g_pty);
    p->slave_open = 0;

    /* A default termios that reads as "sane terminal": ISIG|ICANON|ECHO on
     * the local side, ICRNL|OPOST on the I/O sides, ^C/^\/^D/^U control
     * characters, VMIN=1/VTIME=0. */
    memset(&p->tio, 0, sizeof(p->tio));
    p->tio.c_iflag = 0x0001 | 0x0002;              /* BRKINT | ICRNL   */
    p->tio.c_oflag = 0x0001;                       /* OPOST            */
    p->tio.c_cflag = 0x004B;                       /* CS8 | CREAD      */
    p->tio.c_lflag = 0x0001 | 0x0002 | 0x0008 | 0x0010;  /* ISIG|ICANON|ECHO|ECHOE */
    p->tio.c_line  = 0;
    p->tio.c_cc[0] = 0x03;                         /* VINTR = ^C */
    p->tio.c_cc[1] = 0x1C;                         /* VQUIT  = ^\ */
    p->tio.c_cc[2] = 0x7F;                         /* VERASE = DEL */
    p->tio.c_cc[3] = 0x15;                         /* VKILL  = ^U */
    p->tio.c_cc[4] = 0x04;                         /* VEOF   = ^D */
    p->tio.c_cc[5] = 0;                            /* VTIME */
    p->tio.c_cc[6] = 1;                            /* VMIN */

    p->ws.ws_row = 25;
    p->ws.ws_col = 80;

    /* The slave appears in the dev table under pts/N; the master keeps the
     * pair pointer in its own (per-open) node. */
    char name[16];
    pts_name(name, sizeof(name), p->index);
    if (vfs_register_devnum(name, &pty_slave_ops, p, 136,
                            (uint32_t)p->index) != 0) {
        kfree(p->to_slave);
        kfree(p->to_master);
        p->used = 0;
        return -E_NOSPC;
    }
    p->slave_registered = 1;

    n->ops  = &pty_master_ops;
    n->priv = p;
    return 0;
}

void pty_init(void)
{
    vfs_register_dev("ptmx", &pty_ptmx_ops, NULL);
    dbg_puts("PTY: /dev/ptmx ready (");
    dbg_puts_dec((uint32_t)PTY_MAX);
    dbg_puts(" pairs)\r\n");
}
