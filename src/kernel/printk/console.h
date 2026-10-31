/* SPDX-License-Identifier: GPL-2.0 */
/*
 * console.h - kernel console router. (GPLv2)
 */
#ifndef GNUCOS_CONSOLE_H
#define GNUCOS_CONSOLE_H

#include <stdint.h>

enum kernel_console {
    CONSOLE_TTY0 = 0, /* framebuffer text console (default) */
    CONSOLE_TTYS0     /* real 16550 UART at COM1 */
};

/* Selected console; set once from the boot command line. */
extern enum kernel_console g_console;

/* Parse a console= value ("tty0", "ttyS0", "serial", ...). */
void console_choose(const char *opt);

/* Kernel console output; routes to the selected sink. */
void console_puts(const char *s);
void console_putc(char c);

#endif
