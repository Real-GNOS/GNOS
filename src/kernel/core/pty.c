/*
 * pty.c — UNIX98 pseudo-terminals: /dev/ptmx + /dev/pts/N. (GPLv2)
 *
 * A pty is a terminal whose "line" is a pair of rings instead of a UART:
 * the emulator (xterm, tmux, script) holds the master, the shell holds the
 * slave, and everything a hardware terminal would do to the bytes in
 * between happens here.
 *
 *   master write  ->  input processing  ->  slave read
 *   slave  write  ->  output processing ->  master read
 *
 * Input processing is a real line discipline, mirroring tty.c: ISTRIP/
 * IGNCR/ICRNL/INLCR, ISIG (^C/^\/^Z to the foreground group), IXON (^S/^Q
 * stop and start the output side), canonical assembly with ERASE/KILL/EOF,
 * and echo back down the master with ECHOCTL/ECHOE/ECHOK/ECHOKE.  Output
 * processing is OPOST with ONLCR/OCRNL.  The rest of the terminal contract
 * -- winsize and SIGWINCH, controlling terminal, foreground process group,
 * packet mode, exclusive mode, flush, hangup -- lives in the ioctl
 * handlers below.
 *
 * Opening /dev/ptmx is a cloning operation: every open allocates a fresh
 * pair, registers the slave as /dev/pts/N and hands the caller a master
 * descriptor.  Both ends are re-attached per open from open_resolved() in
 * syscall.c (pty_reattach_master / pty_reattach_slave), because the VFS
 * hands out a copy of the device node and has no per-open hook of its own.
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
#define PTY_LINE   4096              /* canonical line buffer */

typedef struct {
    int       used;
    int       index;
    int       slave_registered;      /* /dev/pts/N is in the dev table */
    int       slave_open;            /* an fd holds the slave end */
    int       master_open;

    /* master -> slave: the input stream (what the shell reads). */
    uint8_t  *to_slave;
    uint32_t  sh, st, scount;
    /* slave -> master: the output stream (what the emulator paints). */
    uint8_t  *to_master;
    uint32_t  mh, mt, mcount;

    char      line[PTY_LINE];        /* canonical assembly buffer */
    uint32_t  line_len;
    int       eof_pending;           /* ^D: next slave read returns 0 once */
    int       stopped;               /* IXON: ^S seen, output held */
    int       pktmode;               /* TIOCPKT on this master */
    int       excl;                  /* TIOCEXCL: no second master open */
    int       fg_pgid;               /* foreground process group */
    int       session;               /* session id of the controlling side */
    termios_t tio;
    winsize_t ws;
} pty_t;

static pty_t g_pty[PTY_MAX];

/* ---- rings -------------------------------------------------------------- */

static void ring_put(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *count,
                     uint8_t b)
{
    if (*count == cap)
        return;
    ring[*head] = b;
    *head = (*head + 1) % cap;
    (*count)++;
}

static uint8_t ring_get(uint8_t *ring, uint32_t cap, uint32_t *tail, uint32_t *count)
{
    uint8_t b = ring[*tail];
    *tail = (*tail + 1) % cap;
    (*count)--;
    return b;
}

static void pty_wake(void)
{
    /* Blocking readers sit on WAIT_PIPE; poll/epoll waiters need the poll
     * channels poked as well (same deal as unix.c's ring_wake). */
    sched_wake_reason(WAIT_PIPE);
    sched_wake_poll_channels();
}

/* ---- output helpers (towards the emulator, i.e. the master) -------------- */

static void pty_out(pty_t *p, char c)
{
    if (p->stopped)
        return;
    ring_put(p->to_master, PTY_RING, &p->mh, &p->mcount, (uint8_t)c);
}

static void pty_out_str(pty_t *p, const char *s)
{
    for (; *s; s++)
        pty_out(p, *s);
}

/*
 * Echo one input byte the way c_lflag says to.  ECHOCTL turns control
 * characters into ^X -- that is why a Ctrl-A does not blank half the
 * terminal -- and DEL into ^?.
 */
static void echo_char(pty_t *p, uint8_t c)
{
    if (!(p->tio.c_lflag & ECHO))
        return;

    if ((p->tio.c_lflag & ECHOCTL) && c < 0x20 &&
        c != '\n' && c != '\r' && c != '\t') {
        pty_out(p, '^');
        pty_out(p, (char)(c + '@'));
        return;
    }
    if ((p->tio.c_lflag & ECHOCTL) && c == 0x7F) {
        pty_out_str(p, "^?");
        return;
    }
    pty_out(p, (char)c);
}

/* Rub out the last echoed character (ECHOE).  xterm's '\b' erases the cell
 * it backs onto, so one is enough. */
static void echo_erase(pty_t *p)
{
    if ((p->tio.c_lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        pty_out(p, '\b');
}

/* ---- output processing (slave -> master) -------------------------------- */

static void pty_output(pty_t *p, const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];
        if (!(p->tio.c_oflag & OPOST)) {
            pty_out(p, c);
            continue;
        }
        if (c == '\n' && (p->tio.c_oflag & ONLCR)) {
            pty_out(p, '\r');
            pty_out(p, '\n');
            continue;
        }
        if (c == '\r' && (p->tio.c_oflag & OCRNL)) {
            pty_out(p, '\n');
            continue;
        }
        pty_out(p, c);
    }
}

/* ---- input processing (master -> slave) --------------------------------- */

static void line_deliver(pty_t *p)
{
    for (uint32_t i = 0; i < p->line_len; i++)
        ring_put(p->to_slave, PTY_RING, &p->sh, &p->scount,
                 (uint8_t)p->line[i]);
    p->line_len = 0;
}

static void pty_input(pty_t *p, uint8_t c)
{
    if (p->tio.c_iflag & ISTRIP)
        c &= 0x7F;

    if (c == '\r') {
        if (p->tio.c_iflag & IGNCR)
            return;
        if (p->tio.c_iflag & ICRNL)
            c = '\n';
    } else if (c == '\n') {
        if (p->tio.c_iflag & INLCR)
            c = '\r';
    }

    /* ISIG: the keys that generate signals go to the foreground group. */
    if (p->tio.c_lflag & ISIG) {
        int sig = 0;
        if (c == p->tio.c_cc[VINTR])      sig = SIGINT;
        else if (c == p->tio.c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == p->tio.c_cc[VSUSP]) sig = SIGTSTP;

        if (sig) {
            echo_char(p, c);
            if (p->tio.c_lflag & ECHO)
                pty_out(p, '\n');
            p->line_len = 0;            /* the half-typed line is gone */
            if (p->fg_pgid)
                proc_signal_group(p->fg_pgid, sig);
            pty_wake();
            return;
        }
    }

    /* IXON: software flow control.  The rings back up, so ^S/^Q really do
     * stop and start the output side. */
    if (p->tio.c_iflag & IXON) {
        if (c == p->tio.c_cc[VSTOP] && !p->stopped) {
            p->stopped = 1;
            if (p->pktmode)
                pty_out(p, (char)TIOCPKT_STOP);
            return;
        }
        if (c == p->tio.c_cc[VSTART] && p->stopped) {
            p->stopped = 0;
            if (p->pktmode)
                pty_out(p, (char)TIOCPKT_START);
            return;
        }
    }

    /* Non-canonical: every byte goes straight through. */
    if (!(p->tio.c_lflag & ICANON)) {
        echo_char(p, c);
        ring_put(p->to_slave, PTY_RING, &p->sh, &p->scount, c);
        pty_wake();
        return;
    }

    /* ---- canonical: assemble a line ------------------------------------- */
    if (c == p->tio.c_cc[VEOF]) {
        if (p->line_len)
            line_deliver(p);           /* deliver the partial line first */
        else
            p->eof_pending = 1;        /* a bare ^D reads as EOF */
        pty_wake();
        return;
    }

    if (c == p->tio.c_cc[VERASE]) {
        if (p->line_len) {
            p->line_len--;
            echo_erase(p);
        }
        return;
    }

    if (c == p->tio.c_cc[VKILL]) {
        if (p->tio.c_lflag & ECHOKE) {
            while (p->line_len) {
                p->line_len--;
                echo_erase(p);
            }
        } else {
            p->line_len = 0;
            if ((p->tio.c_lflag & (ECHO | ECHOK)) == (ECHO | ECHOK))
                pty_out(p, '\n');
        }
        return;
    }

    /* A newline (or whatever ends the line) delivers the buffer. */
    if (c == '\n' || c == p->tio.c_cc[VEOL]) {
        echo_char(p, c);
        if (p->line_len < PTY_LINE)
            p->line[p->line_len++] = (char)c;
        line_deliver(p);
        pty_wake();
        return;
    }

    echo_char(p, c);
    if (p->line_len < PTY_LINE)
        p->line[p->line_len++] = (char)c;
}

/* ---- master end ---------------------------------------------------------- */

static int32_t pty_master_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;
    if (len == 0)
        return 0;

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

    uint32_t nout = 0;
    uint8_t *dst = (uint8_t *)buf;
    /* Packet mode: one status byte in front of every read. */
    if (p->pktmode && len > 1) {
        dst[nout++] = TIOCPKT_DATA;
        len--;
    }
    while (nout < len && p->mcount)
        dst[nout++] = ring_get(p->to_master, PTY_RING, &p->mt, &p->mcount);
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
    if (!p->slave_open)
        return -E_PIPE;

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++)
        pty_input(p, src[i]);
    return (int32_t)len;
}

/* ioctls that mean the same thing on either end. */
static int pty_common_ioctl(pty_t *p, uint64_t cmd, uint64_t arg)
{
    switch (cmd) {
    case TIOCGWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)arg, &p->ws, sizeof(p->ws));
        return 0;
    case TIOCSWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        memcpy(&p->ws, (const void *)(uintptr_t)arg, sizeof(p->ws));
        /* A resize is a signal, not just a number: shells redraw. */
        if (p->fg_pgid)
            proc_signal_group(p->fg_pgid, SIGWINCH);
        pty_wake();
        return 0;
    case TIOCGPGRP:
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = p->fg_pgid;
        return 0;
    case TIOCSPGRP: {
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        p->fg_pgid = *(int32_t *)(uintptr_t)arg;
        return 0;
    }
    case TIOCPKT: {
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        p->pktmode = *(int32_t *)(uintptr_t)arg ? 1 : 0;
        return 0;
    }
    case TIOCEXCL:
        p->excl = 1;
        return 0;
    case TIOCNXCL:
        p->excl = 0;
        return 0;
    case TIOCOUTQ:
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = (int32_t)p->mcount;
        return 0;
    case TIOCSIG: {
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        if (p->fg_pgid)
            proc_signal_group(p->fg_pgid, *(int32_t *)(uintptr_t)arg);
        pty_wake();
        return 0;
    }
    case TCXONC: {
        int how = (int)arg;
        if (how == TCOOFF || how == TCIOFF)
            p->stopped = 1;
        else if (how == TCOON || how == TCION)
            p->stopped = 0;
        return 0;
    }
    case TCFLSH: {
        int what = (int)arg;
        if (what == TCIFLUSH || what == TCIOFLUSH) {
            p->scount = 0;
            p->sh = p->st = 0;
            p->line_len = 0;
        }
        if (what == TCOFLUSH || what == TCIOFLUSH) {
            p->mcount = 0;
            p->mh = p->mt = 0;
        }
        return 0;
    }
    default:
        return -E_NOTTY;
    }
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
        if (cmd == TCSETSF) {
            p->line_len = 0;
            p->scount = 0;
            p->sh = p->st = 0;
        }
        return 0;
    default:
        return pty_common_ioctl(p, cmd, arg);
    }
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
    int16_t r = 0;
    if ((events & POLLIN) && p->mcount)
        r |= POLLIN;
    if ((events & POLLOUT))
        r |= POLLOUT;
    *revents = r;
    return 0;
}

/* ---- slave end ----------------------------------------------------------- */

static int32_t pty_slave_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;
    if (len == 0)
        return 0;

    asm volatile("cli");
    while (p->scount == 0 && !p->eof_pending) {
        proc_t *me = proc_current();
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        /* Master closed with nothing left: the terminal is hung up. */
        if (!p->master_open) {
            asm volatile("sti");
            return 0;
        }
        sched_block_irqoff(WAIT_PIPE);
    }

    /* ^D: a zero-length read, once. */
    if (p->eof_pending && p->scount == 0) {
        p->eof_pending = 0;
        asm volatile("sti");
        return 0;
    }

    uint32_t nout = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (nout < len && p->scount)
        dst[nout++] = ring_get(p->to_slave, PTY_RING, &p->st, &p->scount);
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
    if (!p->master_open)
        return -E_PIPE;

    /* Block while the master is not reading, the way a real terminal
     * blocks when the line cannot take any more. */
    asm volatile("cli");
    while (p->mcount >= PTY_RING - 64) {
        proc_t *me = proc_current();
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }
    pty_output(p, (const char *)buf, len);
    asm volatile("sti");
    pty_wake();
    return (int32_t)len;
}

static int32_t pty_slave_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;
    proc_t *me = proc_current();

    switch (cmd) {
    case TCGETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        memcpy((void *)(uintptr_t)arg, &p->tio, sizeof(p->tio));
        return 0;
    case TCSETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        memcpy(&p->tio, (const void *)(uintptr_t)arg, sizeof(p->tio));
        return 0;
    case TCSETSF:
    case TCSETSW:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        memcpy(&p->tio, (const void *)(uintptr_t)arg, sizeof(p->tio));
        if (cmd == TCSETSF) {
            p->line_len = 0;
            p->scount = 0;
            p->sh = p->st = 0;
        }
        return 0;
    case TIOCSCTTY:                    /* become my controlling terminal */
        if (me) {
            p->session = me->sid;
            p->fg_pgid = me->pgid;
            /* Encode a pty in ctty without colliding with the vt indices
             * tty.c indexes with: negative means "not a console". */
            me->ctty = -(p->index + 1);
        }
        return 0;
    case TIOCNOTTY:
        if (me)
            me->ctty = -1;
        return 0;
    case FIONREAD:
        if (!user_ptr_ok(arg, 4))
            return -E_FAULT;
        *(int32_t *)(uintptr_t)arg = (int32_t)p->scount;
        return 0;
    case TIOCSTI:                      /* push a byte back as input */
        pty_input(p, (uint8_t)arg);
        pty_wake();
        return 0;
    default:
        return pty_common_ioctl(p, cmd, arg);
    }
}

static int pty_slave_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used) {
        *revents = 0;
        return 0;
    }
    int16_t r = 0;
    if ((events & POLLIN) && (p->scount || p->eof_pending || !p->master_open))
        r |= POLLIN;
    if ((events & POLLOUT))
        r |= POLLOUT;
    *revents = r;
    return 0;
}

/* ---- lifecycle ----------------------------------------------------------- */

/* The name of the slave node: "pts/N".  There is no snprintf in the
 * kernel, so it is built by hand. */
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
    p->index = -1;
    pty_wake();
}

static void pty_master_release(vfs_node_t *n)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p)
        return;
    p->master_open = 0;
    /* Hangup: the emulator went away, so the session's foreground group
     * gets SIGHUP and anybody reading sees EOF. */
    if (p->fg_pgid)
        proc_signal_group(p->fg_pgid, SIGHUP);
    pty_pair_release(p);
}

static void pty_slave_release(vfs_node_t *n)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p)
        return;
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

/* ---- the cloning opens --------------------------------------------------- */

int pty_is_ptmx(const vfs_node_t *n)
{
    return n && n->ops == &pty_ptmx_ops;
}

int pty_is_pts(const vfs_node_t *n)
{
    return n && n->ops == &pty_slave_ops;
}

/*
 * Opening the slave again is not a new pty -- it is another handle on the
 * same pair -- but it does have to be counted, because "the slave is open"
 * is what tells the master it is not at EOF yet.
 */
int pty_reattach_slave(vfs_node_t *n)
{
    pty_t *p = (pty_t *)n->priv;
    if (!p || !p->used)
        return -E_BADF;
    p->slave_open = 1;
    return 0;
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
    p->used        = 1;
    p->index       = (int)(p - g_pty);
    p->master_open = 1;

    /* A default termios that reads as "sane terminal": ISIG|ICANON|ECHO on
     * the local side, ICRNL|IXON|OPOST on the I/O sides, ^C/^\/^D/^U/^Z
     * control characters, VMIN=1/VTIME=0. */
    memset(&p->tio, 0, sizeof(p->tio));
    p->tio.c_iflag = ICRNL | IXON;
    p->tio.c_oflag = OPOST | ONLCR;
    p->tio.c_cflag = 0x004B;                       /* CS8 | CREAD */
    p->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    p->tio.c_line  = 0;
    p->tio.c_cc[VINTR]  = 0x03;                    /* ^C */
    p->tio.c_cc[VQUIT]  = 0x1C;                    /* ^\ */
    p->tio.c_cc[VERASE] = 0x7F;                    /* DEL */
    p->tio.c_cc[VKILL]  = 0x15;                    /* ^U */
    p->tio.c_cc[VEOF]   = 0x04;                    /* ^D */
    p->tio.c_cc[VTIME]  = 0;
    p->tio.c_cc[VMIN]   = 1;
    p->tio.c_cc[VSTART] = 0x11;                    /* ^Q */
    p->tio.c_cc[VSTOP]  = 0x13;                    /* ^S */
    p->tio.c_cc[VSUSP]  = 0x1A;                    /* ^Z */

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
    for (int i = 0; i < PTY_MAX; i++)
        g_pty[i].index = -1;
    vfs_register_dev("ptmx", &pty_ptmx_ops, NULL);
    dbg_puts("PTY: /dev/ptmx ready (");
    dbg_puts_dec((uint32_t)PTY_MAX);
    dbg_puts(" pairs)\r\n");
}
