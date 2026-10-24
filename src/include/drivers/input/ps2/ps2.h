/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ps2.h — the i8042 controller and its two ports. (GPLv2)
 *
 * The keyboard port is drained by tty.c's IRQ 1 handler; this file owns
 * the auxiliary port (IRQ 12) and the controller configuration that makes
 * both ports deliver bytes.
 */
#ifndef GNUCOS_PS2_H
#define GNUCOS_PS2_H

#include <stdint.h>

/* ---- i8042 ports ------------------------------------------------------ */
#define PS2_DATA    0x60
#define PS2_CMD     0x64
#define PS2_STATUS  0x64

#define PS2_ST_OUT_BUF  0x01
#define PS2_ST_INPT_BUF 0x02
#define PS2_ST_AUX_DATA 0x20

/* ---- controller commands --------------------------------------------- */
#define PS2_CMD_ENABLE_PORT2 0xa8
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_WRITE_PORT2  0xd4

/* ---- device commands -------------------------------------------------- */
#define PS2_DEV_SET_DEFAULTS 0xf6
#define PS2_DEV_ENABLE       0xf4

/* Enable the auxiliary port, enable IRQ 1/IRQ 12 in the controller
 * configuration byte, and put the mouse into streaming mode.  Call after
 * tty_init(), which enables the keyboard port this controller shares. */
void ps2_init(void);

#endif
