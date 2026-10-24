/* SPDX-License-Identifier: GPL-2.0 */
/*
 * console.c — output side of the virtual terminals. (GPLv2)
 *
 * Everything a terminal puts *on screen* lives here: the fbcon tee to the
 * debug port, the echo of input bytes (including ECHOCTL's ^X and ECHOE's
 * rub-out), output post-processing (OPOST/ONLCR/OCRNL) and the window size
 * reported by TIOCGWINSZ.
 *
 * The line discipline itself -- what to echo and when -- is in tty_core.c;
 * this file only knows how to render one byte once that decision is made.
 */
#include <stdint.h>

#include "tty.h"
#include "tty_internal.h"
#include "fbcon.h"
#include "debugcon.h"
#include "vfs.h"
#include "proc.h"

/* ---- raw output -------------------------------------------------------- */
/*
 * Output goes to this terminal's console, and is teed to the debug port as
 * well.  The tee is not decoration: `make test` runs headless and the debug
 * log is the only place the boot can be read afterwards, so a getty prompt on
 * tty4 has to show up there even though nothing ever paints it.
 */
void vt_out_char(vt_t *v, char c)
{
    fbcon_putc_on(v->con, c);
    dbg_putc(c);
}

static void out_str(vt_t *v, const char *s)
{
    for (; *s; s++)
        vt_out_char(v, *s);
}

/* ---- echo -------------------------------------------------------------- */
/*
 * Echo one input byte the way c_lflag says to.  With ECHOCTL a control
 * character shows up as ^X rather than doing whatever the console would make
 * of it -- that is why a Ctrl-A does not blank half the screen.
 */
void vt_echo_char(vt_t *v, uint8_t c)
{
    if (!(v->tio.c_lflag & ECHO))
        return;

    if ((v->tio.c_lflag & ECHOCTL) && c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
        vt_out_char(v, '^');
        vt_out_char(v, (char)(c + '@'));
        return;
    }
    if ((v->tio.c_lflag & ECHOCTL) && c == 0x7F) {
        out_str(v, "^?");
        return;
    }
    vt_out_char(v, (char)c);
}

/* Rub out the last echoed character (ECHOE).  fbcon's '\b' already draws a
 * blank over the glyph and leaves the cursor on it, so one is enough. */
void vt_echo_erase(vt_t *v)
{
    if ((v->tio.c_lflag & (ECHO | ECHOE)) == (ECHO | ECHOE))
        vt_out_char(v, '\b');
}

/* ---- output ------------------------------------------------------------ */
int32_t vt_write(vt_t *v, const char *buf, uint32_t len)
{
    proc_t *p = proc_current();

    /*
     * A background write only stops the writer when TOSTOP is set, and never
     * when the writer is ignoring or blocking SIGTTOU -- POSIX carves that
     * exemption out so a shell can reclaim the terminal without stopping
     * itself.  Skipping the check would hang the machine on the first prompt.
     */
    if (p && v->fg_pgid && p->pgid != v->fg_pgid && (v->tio.c_lflag & TOSTOP) &&
        !proc_signal_blocked(p, SIGTTOU)) {
        proc_signal(p, SIGTTOU);
        return -E_INTR;
    }

    for (uint32_t i = 0; i < len; i++) {
        char c = buf[i];

        if (v->tio.c_oflag & OPOST) {
            if (c == '\n' && (v->tio.c_oflag & ONLCR)) {
                vt_out_char(v, '\r');
                vt_out_char(v, '\n');
                continue;
            }
            if (c == '\r' && (v->tio.c_oflag & OCRNL))
                c = '\n';
        } else if (c == '\n') {
            /* No post-processing: a bare LF moves down a row and leaves the
             * column alone, the way a real terminal behaves. */
            fbcon_lf_on(v->con);
            dbg_putc('\n');
            continue;
        }

        vt_out_char(v, c);
    }
    return (int32_t)len;
}

/* Kernel-side write, on the caller's controlling terminal.  The public
 * declaration lives in tty.h. */
int32_t tty_write(const char *buf, uint32_t len)
{
    return vt_write(vt_current(), buf, len);
}

/* ---- window size ------------------------------------------------------- */
void tty_get_winsize(winsize_t *ws)
{
    uint32_t cols = 0, rows = 0;
    fbcon_size(&cols, &rows);
    ws->ws_row    = (uint16_t)rows;
    ws->ws_col    = (uint16_t)cols;
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
}
