/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vt.c — virtual-terminal switching, the VT_* ioctls and tty_init. (GPLv2)
 *
 * There is one framebuffer and one keyboard, so "which terminal is live"
 * is a single global: g_active.  Switching it is almost free because every
 * terminal's fbcon console keeps its own cell buffer -- the incoming one is
 * repainted from cells, nothing is replayed.  The discipline state does not
 * move: a half-typed line on tty1 is still sitting there when you come back,
 * exactly like a real Linux console.
 *
 * The VT_* ioctls are here rather than in tty_io.c because they are about
 * the terminal *set*, not about one terminal's contract: activate, wait for
 * activation, report the bitmap of allocated terminals.  Terminal numbers are
 * 1-based on the user-space side (they are the N in /dev/ttyN) and 0-based
 * inside the tty code; every conversion happens in this file.
 */
#include <stdint.h>

#include "tty.h"
#include "tty_internal.h"
#include "fbcon.h"
#include "vfs.h"
#include "proc.h"
#include "vmm.h"
#include "subsys.h"
#include "debugcon.h"
#include "kstring.h"
#include "vcs.h"

int tty_vt_active(void)
{
    return g_active;
}

/* ---- switching terminals ----------------------------------------------- */
/*
 * Moving the focus is almost free: the incoming terminal's console already
 * holds every character ever written to it, so fbcon repaints from cells and
 * nothing has to be replayed.  What does *not* move is any of the discipline
 * state -- a half-typed line on tty1 is still sitting there when you come
 * back, which is exactly the behaviour a real Linux console has.
 *
 * The sleeping-reader wakeup is the subtle part.  A process blocked in
 * tty_read() on the terminal being switched *away from* is still correctly
 * blocked, but VT_WAITACTIVE sleepers are waiting on precisely this event, so
 * everyone parked on WAIT_TTY gets a look.  They re-check their own condition
 * and go back to sleep if it has not been met.
 */
void tty_vt_switch(int n)
{
    if (n < 0 || n >= NR_VT || n == g_active)
        return;

    /* The Ctrl-Alt that triggered the switch is, by definition, still held:
     * its release scancodes belong to the *new* terminal's key stream and may
     * never arrive (the user let go while another VT was up, or the release
     * was dropped).  Clearing the modifier state here means a switch can never
     * leave Ctrl/Alt latched on -- which would otherwise turn the next F-key
     * into another silent switch, or the next letter into a control char. */
    tty_kbd_reset_modifiers();

    g_active = n;
    fbcon_activate(g_vt[n].con);
    sched_wake_poll_channels();
}

void tty_vt_release_session(int sid)
{
    if (!sid)
        return;
    for (int i = 0; i < NR_VT; i++)
        if (g_vt[i].sid == sid) {
            g_vt[i].sid     = 0;
            g_vt[i].fg_pgid = 0;
        }
}

/* ---- the VT_* ioctls --------------------------------------------------- */
int32_t vt_ioctl(vt_t *v, uint64_t cmd, uint64_t arg)
{
    switch (cmd) {
    case VT_GETSTATE: {
        if (!user_ptr_ok(arg, sizeof(vt_stat_t)))
            return -E_FAULT;
        vt_stat_t *st = (vt_stat_t *)(uintptr_t)arg;
        st->v_active  = (uint16_t)(g_active + 1);
        st->v_signal  = 0;
        /* Bit N means terminal N exists.  All of them always do here -- they
         * are statically allocated -- and bit 0 is set the way Linux sets it,
         * for a terminal 0 that has never existed. */
        st->v_state = (uint16_t)((1u << (NR_VT + 1)) - 1);
        return 0;
    }

    case VT_OPENQRY: {
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        int free_vt = -1;
        for (int i = 0; i < NR_VT; i++)
            if (!g_vt[i].sid) {
                free_vt = i + 1;
                break;
            }
        *(int *)(uintptr_t)arg = free_vt;
        return 0;
    }

    case VT_ACTIVATE:
        if (arg < 1 || arg > NR_VT)
            return -E_INVAL;
        tty_vt_switch((int)arg - 1);
        return 0;

    case VT_WAITACTIVE:
        if (arg < 1 || arg > NR_VT)
            return -E_INVAL;
        /* Already there is the common case and must not block. */
        while (g_active != (int)arg - 1) {
            proc_t *p = proc_current();
            if (!p)
                return -E_INVAL;
            if (proc_pending_signals(p))
                return -E_INTR;
            sched_block(WAIT_TTY);
        }
        return 0;

    case VT_GETMODE: {
        if (!user_ptr_ok(arg, sizeof(vt_mode_t)))
            return -E_FAULT;
        vt_mode_t *m = (vt_mode_t *)(uintptr_t)arg;
        m->mode      = VT_AUTO;
        m->waitv     = 0;
        m->relsig = m->acqsig = m->frsig = 0;
        return 0;
    }

    case VT_SETMODE:
        /* The kernel always switches on its own authority; there is no
         * process to negotiate with, so VT_AUTO is the only honest answer
         * and asking for VT_PROCESS is refused rather than quietly ignored. */
        if (!user_ptr_ok(arg, sizeof(vt_mode_t)))
            return -E_FAULT;
        if (((const vt_mode_t *)(uintptr_t)arg)->mode != VT_AUTO)
            return -E_INVAL;
        return 0;

    case VT_DISALLOCATE:
        /* Terminals are static.  Clearing the session is the only part of
         * "deallocate" that means anything here, and it is what lets a fresh
         * getty claim the line. */
        if (arg > NR_VT)
            return -E_INVAL;
        if (arg == 0)
            v->sid = 0;
        else
            g_vt[arg - 1].sid = 0;
        return 0;

    default:
        return -E_NOTTY;
    }
}

/* ---- setup ------------------------------------------------------------- */
void tty_init(void)
{
    memset(g_vt, 0, sizeof(g_vt));
    g_active = 0;

    for (int i = 0; i < NR_VT; i++) {
        /* Terminal 1 draws on console 0, the one fbcon_init() already made
         * and the kernel has been logging to since boot -- so the boot
         * messages stay on screen and the first shell continues below them. */
        g_vt[i].con = (i == 0) ? 0 : fbcon_alloc();
        tty_termios_defaults(&g_vt[i].tio);
    }

    /* Modifier state is global (one keyboard), so it starts clean; the 8042
     * bring-up itself lives in tty_kbd.c with the scancode decoder. */
    tty_keyboard_init();

    /*
     * /dev/tty1 .. /dev/tty6 name one terminal each; /dev/tty is "mine",
     * resolved per caller; /dev/console is where the kernel talks, which is
     * always terminal 1.  An init system opens one or the other with no way
     * to ask which exists, so all of them have to.
     */
    static const char *vt_names[NR_VT] = {
        "tty1", "tty2", "tty3", "tty4", "tty5", "tty6",
    };
    for (int i = 0; i < NR_VT; i++) {
        vfs_register_dev(vt_names[i], (const vfs_ops_t *)tty_ops(), &g_vt[i]);
        subsys_set_state(subsys_register(vt_names[i], vt_names[i], SUBSYS_CLASS_TTY, 4, i + 1),
                         SUBSYS_STATE_LIVE);
    }
    vfs_register_dev("tty", (const vfs_ops_t *)tty_ops(), NULL);
    vfs_register_dev("console", (const vfs_ops_t *)tty_ops(), &g_vt[0]);
    vcs_init(); /* /dev/vcs, /dev/vcsa: the active screen as a file */
    subsys_set_state(subsys_register("tty", "tty", SUBSYS_CLASS_TTY, 5, 0), SUBSYS_STATE_LIVE);
    subsys_set_state(subsys_register("console", "console", SUBSYS_CLASS_TTY, 5, 1),
                     SUBSYS_STATE_LIVE);

    dbg_puts("GNOS: tty ready (");
    dbg_puts_dec(NR_VT);
    dbg_puts(" virtual terminals, Ctrl-Alt-F1..F");
    dbg_puts_dec(NR_VT);
    dbg_puts(" switches)\r\n");
}
