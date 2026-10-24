/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tty_io.c — the VFS character-device face of the virtual terminals. (GPLv2)
 *
 * /dev/tty1 .. /dev/tty6, /dev/tty and /dev/console all carry the same
 * vfs_ops_t defined at the bottom of this file.  read() and write() hand the
 * work to the line discipline (tty_core.c) and the console (console.c); the
 * ioctl handler is split between the terminal contract that belongs here --
 * termios, window size and job control -- and the VT_* commands implemented
 * in vt.c.
 *
 * Keeping this glue in its own file means the rest of the tty code never has
 * to include vfs.h just to compile, and pty.c can keep using the same public
 * tty.h interface it always did.
 */
#include <stdint.h>

#include "tty.h"
#include "tty_internal.h"
#include "vfs.h"
#include "proc.h"
#include "vmm.h"

/* ---- the VFS glue ------------------------------------------------------ */
static int32_t tty_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    return vt_read(vt_of_node(n), (char *)buf, len);
}

static int32_t tty_node_write(vfs_node_t *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)off;
    return vt_write(vt_of_node(n), (const char *)buf, len);
}

/*
 * Terminal ioctls, answered on the VFS char-device path rather than via the
 * syscall-layer tty_ops() fallback.  This is what makes musl's isatty()
 * (a TCGETS probe) and tcgetattr()/tcsetattr() succeed on /dev/tty and
 * /dev/console -- without it a shell like bash refuses to start in interactive
 * mode, because it decides "interactive" from isatty(0) && isatty(1).
 */
static int32_t tty_node_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    vt_t *v = vt_of_node(n);

    switch (cmd) {
    case TCGETS:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        vt_get_termios(v, (termios_t *)(uintptr_t)arg);
        return 0;

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_ptr_ok(arg, sizeof(termios_t)))
            return -E_FAULT;
        /* No job-control check here, and that is deliberate: Linux applies
         * none either (POSIX only says tcsetattr "may" send SIGTTOU, and the
         * kernel doesn't).  Measured on a real Linux pty: a background group
         * calling tcsetattr() survives untouched.  Checking here stalled the
         * whole boot -- busybox init's sysinit child sets up its own group
         * before exec'ing /sbin/openrc, found itself behind init's fg group,
         * earned a SIGTTOU on its very first tcsetattr() and stopped dead,
         * taking every runlevel (and every getty) with it.  TIOCSPGRP below
         * still carries the check: handing over the terminal is the one call
         * where the stop is the point. */
        vt_set_termios(v, (const termios_t *)(uintptr_t)arg, cmd == TCSETSF);
        return 0;

    case TIOCGWINSZ:
        if (!user_ptr_ok(arg, sizeof(winsize_t)))
            return -E_FAULT;
        tty_get_winsize((winsize_t *)(uintptr_t)arg);
        return 0;

    case TIOCSWINSZ:
        /* The console is exactly as large as the framebuffer makes it. */
        return 0;

    case TIOCGPGRP:
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        *(int *)(uintptr_t)arg = v->fg_pgid;
        return 0;

    case TIOCSPGRP:
        if (!user_ptr_ok(arg, sizeof(int)))
            return -E_FAULT;
        if (vt_check_ttou(v))
            return -E_INTR;
        v->fg_pgid = *(const int *)(uintptr_t)arg;
        return 0;

    /*
     * TIOCSCTTY is what a getty issues after setsid() to say "this terminal
     * is mine".  With several terminals it finally has work to do: it records
     * the index in the caller's PCB, so every later /dev/tty open by that
     * process (or anything it forks) resolves to this line and not to
     * whichever console the user is looking at.
     */
    case TIOCSCTTY: {
        proc_t *p = proc_current();
        if (!p)
            return -E_PERM;
        p->ctty = vt_index(v);
        if (!v->sid || v->sid == p->sid || arg /* force */) {
            v->sid = p->sid;
            /*
             * Claiming the terminal hands the caller's group the foreground
             * too, exactly as Linux's __proc_set_tty() does -- and the
             * "exactly" matters.  Terminal 1 still carries the process group
             * of /etc/rc, long since exited, when init starts a getty on it;
             * leaving that stale pgid in place would make the new session a
             * *background* one on its own terminal, and its very next
             * tcsetpgrp() would earn it a SIGTTOU and stop it dead.
             */
            v->fg_pgid = p->pgid;
        }
        return 0;
    }

    case TIOCNOTTY: {
        proc_t *p = proc_current();
        if (p) {
            p->ctty = -1;
            if (v->sid == p->sid)
                v->sid = 0;
        }
        return 0;
    }

    case VT_OPENQRY:
    case VT_GETMODE:
    case VT_SETMODE:
    case VT_GETSTATE:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case VT_DISALLOCATE:
        return vt_ioctl(v, cmd, arg);

    default:
        return -E_NOTTY;
    }
}

static const vfs_ops_t g_tty_ops = {
    .read  = tty_node_read,
    .write = tty_node_write,
    .ioctl = tty_node_ioctl,
};

const void *tty_ops(void)
{
    return &g_tty_ops;
}
