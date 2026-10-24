/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tty_core.c — per-terminal state and the termios line discipline. (GPLv2)
 *
 * This is the part of the old tty.c that a read or a keystroke actually
 * touches: the terminal table, the input ring, the canonical line buffer,
 * the input/output flag handling, termios and job control.  Output to the
 * fbcon console lives in console.c, the VFS entry points in tty_io.c, the
 * VT_* ioctls in vt.c and the PS/2 decoder in tty_kbd.c.
 *
 * There are NR_VT terminals over one framebuffer and one keyboard.  Each owns
 * a line discipline, an input ring, a termios, a foreground process group and
 * an fbcon console; exactly one is *active*, meaning it is the one on screen
 * and the one the keyboard feeds.
 *
 * What the discipline actually *does* is not hard-coded: it is driven by one
 * struct termios per terminal, the same one user space reads and writes
 * through tcgetattr()/tcsetattr() (ioctl TCGETS/TCSETS*).
 */
#include <stdint.h>

#include "tty.h"
#include "tty_internal.h"
#include "vfs.h"
#include "proc.h"
#include "timer.h"
#include "kstring.h"

vt_t g_vt[NR_VT];
int  g_active; /* the terminal on screen */

/* ---- which terminal is this call about? -------------------------------- */
/*
 * /dev/tty is the odd one out.  Every /dev/ttyN node carries a pointer to its
 * own vt_t in node->priv, so a descriptor names a terminal directly; /dev/tty
 * carries NULL and means "my controlling terminal", which has to be looked up
 * in the caller's PCB every time.  Falling back to the active terminal covers
 * the two cases with no caller to ask: the kernel itself, and a process that
 * has never claimed a controlling terminal.
 */
vt_t *vt_current(void)
{
    proc_t *p = proc_current();
    if (p && p->ctty >= 0 && p->ctty < NR_VT)
        return &g_vt[p->ctty];
    return &g_vt[g_active];
}

vt_t *vt_of_node(const vfs_node_t *n)
{
    if (n && n->priv)
        return (vt_t *)n->priv;
    return vt_current();
}

int vt_index(const vt_t *v)
{
    return (int)(v - g_vt);
}

/* ---- the input ring ---------------------------------------------------- */
static int ring_empty(const vt_t *v)
{
    return v->head == v->tail;
}

static uint32_t ring_avail(const vt_t *v)
{
    return (v->head + RING_SIZE - v->tail) % RING_SIZE;
}

uint32_t tty_input_avail(void)
{
    return ring_avail(vt_current());
}

uint32_t tty_node_input_avail(const vfs_node_t *n)
{
    return ring_avail(vt_of_node(n));
}

static void ring_put(vt_t *v, uint16_t c)
{
    uint32_t next = (v->head + 1) % RING_SIZE;
    if (next == v->tail)
        return; /* full: drop, we have nowhere to block */
    v->ring[v->head] = c;
    v->head          = next;
}

static void ring_flush(vt_t *v)
{
    v->head = v->tail = 0;
}

/* ---- canonical line buffer --------------------------------------------- */
static void line_flush(vt_t *v, int with_newline)
{
    for (uint32_t i = 0; i < v->line_len; i++)
        ring_put(v, (uint8_t)v->line[i]);
    if (with_newline)
        ring_put(v, '\n');
    v->line_len = 0;
    sched_wake_poll_channels();
}

/* ---- the line discipline ----------------------------------------------- */
static void vt_input_char(vt_t *v, uint8_t c)
{
    /* ---- c_iflag: input mapping ---------------------------------------- */
    if (v->tio.c_iflag & ISTRIP)
        c &= 0x7F;

    if (c == '\r') {
        if (v->tio.c_iflag & IGNCR)
            return;
        if (v->tio.c_iflag & ICRNL)
            c = '\n';
    } else if (c == '\n') {
        if (v->tio.c_iflag & INLCR)
            c = '\r';
    }

    /* ---- c_lflag & ISIG: the keys that generate signals ----------------- */
    if (v->tio.c_lflag & ISIG) {
        int sig = 0;
        if (c == v->tio.c_cc[VINTR])
            sig = SIGINT;
        else if (c == v->tio.c_cc[VQUIT])
            sig = SIGQUIT;
        else if (c == v->tio.c_cc[VSUSP])
            sig = SIGTSTP;

        if (sig) {
            vt_echo_char(v, c);
            if (v->tio.c_lflag & ECHO)
                vt_out_char(v, '\n');
            v->line_len = 0; /* the half-typed line is gone */

            if (v->fg_pgid)
                proc_signal_group(v->fg_pgid, sig);

            /* A reader blocked in the foreground group has to come back out
             * of the kernel for the signal to take effect. */
            sched_wake_poll_channels();
            return;
        }
    }

    /* ---- c_iflag & IXON: software flow control -------------------------- */
    if (v->tio.c_iflag & IXON) {
        /* Output never backs up on a framebuffer, so ^S/^Q have nothing to
         * stop or start.  They are still swallowed rather than delivered,
         * which is what a program that leaves IXON on expects. */
        if (c == v->tio.c_cc[VSTOP] || c == v->tio.c_cc[VSTART])
            return;
    }

    /* ---- non-canonical: every byte goes straight through ---------------- */
    if (!(v->tio.c_lflag & ICANON)) {
        vt_echo_char(v, c);
        ring_put(v, c);
        sched_wake_poll_channels();
        return;
    }

    /* ---- canonical: assemble a line ------------------------------------- */
    if (c == v->tio.c_cc[VEOF]) {
        if (v->line_len)
            line_flush(v, 0); /* deliver the partial line */
        else {
            ring_put(v, RING_EOF);
            sched_wake_poll_channels();
        }
        return;
    }

    if (c == v->tio.c_cc[VERASE]) {
        if (v->line_len) {
            v->line_len--;
            vt_echo_erase(v);
        }
        return;
    }

    if (c == v->tio.c_cc[VKILL]) {
        if (v->tio.c_lflag & ECHOKE) {
            while (v->line_len) {
                v->line_len--;
                vt_echo_erase(v);
            }
        } else {
            v->line_len = 0;
            if ((v->tio.c_lflag & (ECHO | ECHOK)) == (ECHO | ECHOK))
                vt_out_char(v, '\n');
        }
        return;
    }

    if (c == '\n' || (v->tio.c_cc[VEOL] && c == v->tio.c_cc[VEOL])) {
        if (v->tio.c_lflag & (ECHO | ECHONL))
            vt_out_char(v, '\n');
        line_flush(v, 1);
        return;
    }

    if (v->line_len < LINE_MAX - 1) {
        v->line[v->line_len++] = (char)c;
        vt_echo_char(v, c);
    }
}

/*
 * The single entry point for a real key press: used only by the keyboard IRQ
 * after scancode decode.  Keystrokes always land on the terminal that is on
 * screen -- that is what "active" means.
 */
void tty_input_char(uint8_t c)
{
    vt_input_char(&g_vt[g_active], c);
}

/* Feed a whole buffer through the line discipline, byte by byte.  This is
 * the kernel side of the ttyinject(404) syscall: a test program hands us a
 * string of "keystrokes" and we treat each one exactly as the IRQ would have.
 *
 * Unlike a real key, injected input targets the *calling* terminal (its
 * controlling terminal), not whatever happens to be on screen.  A reader
 * always pulls from its own ctty, so injecting anywhere else would let a
 * stray VT switch during the test strand the reader's VEOF on a different
 * terminal and hang it forever -- which is exactly the boot-time "readline
 * scrolls on its own and the network test never runs" symptom. */
void tty_inject(const char *buf, uint32_t len)
{
    vt_t *v = vt_current();
    for (uint32_t i = 0; i < len; i++)
        vt_input_char(v, (uint8_t)buf[i]);
}

/* ---- read -------------------------------------------------------------- */
int32_t vt_read(vt_t *v, char *buf, uint32_t len)
{
    if (len == 0)
        return 0;

    proc_t *p = proc_current();

    /*
     * Background jobs are not allowed to read the terminal.  SIGTTIN's
     * default action stops the offending process, so it simply freezes until
     * the shell brings it to the foreground with fg.  If the reader cannot be
     * stopped -- it is ignoring or blocking SIGTTIN -- POSIX says fail with
     * EIO rather than let it steal the user's keystrokes.
     */
    if (p && v->fg_pgid && p->pgid != v->fg_pgid) {
        if (proc_signal_blocked(p, SIGTTIN))
            return -E_IO;
        proc_signal(p, SIGTTIN);
        return -E_INTR;
    }

    int canon = (v->tio.c_lflag & ICANON) != 0;

    /* In non-canonical mode VMIN/VTIME decide when a read is "done": how few
     * bytes will do, and how long to wait for them. */
    uint32_t vmin  = canon ? 1 : v->tio.c_cc[VMIN];
    uint32_t vtime = canon ? 0 : v->tio.c_cc[VTIME];
    uint32_t want  = vmin > len ? len : vmin;

    asm volatile("cli");

    uint64_t deadline   = 0;
    uint32_t last_avail = 0;

    for (;;) {
        if (proc_pending_signals(p)) {
            asm volatile("sti");
            return -E_INTR;
        }

        uint32_t avail = ring_avail(v);
        if (canon) {
            if (avail)
                break;
        } else {
            if (avail && avail >= want)
                break;
            if (want == 0 && vtime == 0)
                break; /* a pure poll: 0 bytes is fine */
        }

        if (!p) {
            /* No scheduler yet (very early boot): just idle. */
            asm volatile("sti; hlt; cli");
            continue;
        }

        /* With VTIME the wait is bounded.  MIN == 0 starts the timer with the
         * read itself; MIN > 0 makes it an inter-byte timer, armed only once
         * the first byte has landed and restarted by every byte after it. */
        if (!canon && vtime && (vmin == 0 || avail > 0)) {
            uint64_t now = timer_ticks();
            if (!deadline || avail != last_avail)
                deadline = now + (uint64_t)vtime * TICKS_PER_DECISEC;
            last_avail = avail;
            if (now >= deadline)
                break;
            sched_block_timeout(WAIT_TTY, deadline - now);
            continue;
        }

        sched_block_irqoff(WAIT_TTY);
    }

    uint32_t n   = 0;
    int      eof = 0;
    while (n < len && !ring_empty(v)) {
        uint16_t c = v->ring[v->tail];
        v->tail    = (v->tail + 1) % RING_SIZE;
        if (c == RING_EOF) {
            eof = 1;
            break;
        }
        buf[n++] = (char)c;
        if (canon && c == '\n')
            break;
    }

    asm volatile("sti");

    if (eof && n == 0)
        return 0;
    return (int32_t)n;
}

int32_t tty_read(char *buf, uint32_t len)
{
    return vt_read(vt_current(), buf, len);
}

/* ---- foreground process group and job control -------------------------- */
void tty_set_pgrp(int pgid)
{
    vt_current()->fg_pgid = pgid;
}

int tty_get_pgrp(void)
{
    return vt_current()->fg_pgid;
}

int vt_check_ttou(vt_t *v)
{
    proc_t *p = proc_current();

    if (!p || !v->fg_pgid || p->pgid == v->fg_pgid)
        return 0;

    /* The exemption again: a process that is ignoring or blocking SIGTTOU is
     * telling us it knows what it is doing with the terminal.  Every shell
     * does, which is the only reason handing a job the terminal from the
     * (background) child of a fork works at all. */
    if (proc_signal_blocked(p, SIGTTOU))
        return 0;

    proc_signal(p, SIGTTOU);
    return 1;
}

int tty_check_ttou(void)
{
    return vt_check_ttou(vt_current());
}

/* ---- termios ----------------------------------------------------------- */
void vt_get_termios(vt_t *v, termios_t *t)
{
    *t = v->tio;
}

void vt_set_termios(vt_t *v, const termios_t *t, int flush)
{
    asm volatile("cli");

    /* Leaving canonical mode with a half-typed line would strand those bytes
     * in a buffer nobody reads any more, so hand them over first. */
    if ((v->tio.c_lflag & ICANON) && !(t->c_lflag & ICANON) && v->line_len)
        line_flush(v, 0);

    v->tio        = *t;
    v->tio.c_line = 0;

    if (flush) {
        v->line_len = 0;
        ring_flush(v);
    }

    asm volatile("sti");

    /* A reader parked under the old rules may already be satisfied by the
     * new ones (VMIN dropping to 0, say), so let it look again. */
    sched_wake_poll_channels();
}

void tty_get_termios(termios_t *t)
{
    vt_get_termios(vt_current(), t);
}

void tty_set_termios(const termios_t *t, int flush)
{
    vt_set_termios(vt_current(), t, flush);
}

void tty_termios_defaults(termios_t *t)
{
    memset(t, 0, sizeof(*t));

    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD | CLOCAL | HUPCL;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;

    t->c_cc[VINTR]    = 0x03; /* ^C */
    t->c_cc[VQUIT]    = 0x1C; /* ^\ */
    t->c_cc[VERASE]   = 0x7F; /* DEL */
    t->c_cc[VKILL]    = 0x15; /* ^U */
    t->c_cc[VEOF]     = 0x04; /* ^D */
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSTART]   = 0x11; /* ^Q */
    t->c_cc[VSTOP]    = 0x13; /* ^S */
    t->c_cc[VSUSP]    = 0x1A; /* ^Z */
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 0x12; /* ^R */
    t->c_cc[VDISCARD] = 0x0F; /* ^O */
    t->c_cc[VWERASE]  = 0x17; /* ^W */
    t->c_cc[VLNEXT]   = 0x16; /* ^V */
    t->c_cc[VEOL2]    = 0;
}
