/* SPDX-License-Identifier: GPL-2.0 */
/*
 * console.c - kernel console router. (GPLv2)
 *
 * Decouples "print a kernel message" from the physical sink.  The boot
 * command line chooses one:
 *   console=tty0   (default) - the framebuffer text console (fbcon)
 *   console=ttyS0  (or serial) - the real 16550 UART at COM1
 * console_puts()/console_putc() send to whichever was selected; tty0 keeps
 * the framebuffer path so the screen still comes up, the serial path just
 * stops echoing to it.
 */
#include <stdint.h>
#include "console.h"
#include "fbcon.h"
#include "serial8250.h"

enum kernel_console g_console = CONSOLE_TTY0;

/* Fixed-name match helper (no strcmp in the freestanding kernel). */
static int name_is(const char *opt, const char *name)
{
    int n = 0;
    while (name[n]) {
        if (opt[n] != name[n])
            return 0;
        n++;
    }
    return opt[n] == 0; /* exact token, no trailing junk */
}

void console_choose(const char *opt)
{
    if (!opt)
        return;
    if (name_is(opt, "ttyS0") || name_is(opt, "ttyS") || name_is(opt, "serial") ||
        name_is(opt, "ttyS0,115200") || name_is(opt, "serial8250"))
        g_console = CONSOLE_TTYS0;
    else
        g_console = CONSOLE_TTY0; /* tty0, fb, or anything unknown */
}

void console_putc(char c)
{
    if (g_console == CONSOLE_TTYS0)
        serial8250_putc(c);
    else
        fbcon_putc(c);
}

void console_puts(const char *s)
{
    if (g_console == CONSOLE_TTYS0)
        serial8250_puts(s);
    else
        fbcon_puts(s);
}
