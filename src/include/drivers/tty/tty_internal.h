/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tty_internal.h — state shared by the tty implementation files. (GPLv2)
 *
 * tty.c used to hold the whole virtual-console stack.  It is now split the
 * way Uinxed splits its tty tree:
 *
 *   tty_core.c   the per-terminal state, line discipline, read side, termios
 *   tty_io.c     the VFS character-device glue: read/write/ioctl entry points
 *   console.c    output to the framebuffer console, echo and winsize
 *   tty_kbd.c    PS/2 scancode decode and IRQ 1
 *   vt.c         VT switching, the VT_* ioctls and device registration
 *
 * Nothing here is part of the public tty interface; include/drivers/tty/tty.h
 * is.  The vt_t definition stays in one place so no file has to guess the
 * layout of the terminal table.
 */
#ifndef GNUCOS_TTY_INTERNAL_H
#define GNUCOS_TTY_INTERNAL_H

#include <stdint.h>

#include "tty.h"

#define LINE_MAX  256
#define RING_SIZE 1024

/*
 * End-of-file is kept *out of band*.  Storing it as the byte 0x04 would make
 * a raw-mode read of a literal Ctrl-D indistinguishable from end of input, so
 * the ring holds 16-bit cells and the marker uses a value no byte can have.
 */
#define RING_EOF 0x100u

/* The PIT runs at SCHED_HZ (100) Hz and c_cc[VTIME] counts tenths of a
 * second, so one VTIME unit is ten ticks. */
#define TICKS_PER_DECISEC 10

/* One virtual terminal.  Everything that used to be a file-scope singleton
 * now lives in here, one copy per terminal. */
typedef struct vt {
    int con; /* the fbcon console it draws on */

    char     line[LINE_MAX]; /* the line being edited (canonical) */
    uint32_t line_len;

    uint16_t          ring[RING_SIZE]; /* finished input waiting to be read */
    volatile uint32_t head, tail;

    termios_t tio;     /* this terminal's line discipline */
    int       fg_pgid; /* foreground group, 0 = nobody */
    int       sid;     /* session that claimed it, 0 = free */
} vt_t;

extern vt_t g_vt[NR_VT];
extern int  g_active; /* the terminal on screen */

struct vfs_node;

/* Resolve "the terminal this call is about": node->priv for /dev/ttyN,
 * proc_t::ctty for /dev/tty, the active terminal for the kernel itself. */
vt_t *vt_current(void);
vt_t *vt_of_node(const struct vfs_node *n);
int   vt_index(const vt_t *v);

/* ---- tty_core.c --------------------------------------------------------- */
int32_t vt_read(vt_t *v, char *buf, uint32_t len);
void    vt_get_termios(vt_t *v, termios_t *t);
void    vt_set_termios(vt_t *v, const termios_t *t, int flush);
int     vt_check_ttou(vt_t *v);
void    tty_termios_defaults(termios_t *t);

/* ---- console.c ---------------------------------------------------------- */
void    vt_out_char(vt_t *v, char c);
int32_t vt_write(vt_t *v, const char *buf, uint32_t len);
void    vt_echo_char(vt_t *v, uint8_t c);
void    vt_echo_erase(vt_t *v);

/* ---- vt.c --------------------------------------------------------------- */
int32_t vt_ioctl(vt_t *v, uint64_t cmd, uint64_t arg);

/* ---- tty_kbd.c ---------------------------------------------------------- */
void tty_keyboard_init(void);
void tty_kbd_reset_modifiers(void);

#endif
