/* SPDX-License-Identifier: GPL-2.0 */
/*
 * serial8250.h - 16550/8250 UART boot-console backend. (GPLv2)
 */
#ifndef GNUCOS_SERIAL8250_H
#define GNUCOS_SERIAL8250_H

#include <stdint.h>

/* Probe COM1 (0x3F8) and initialise the UART as 8N1/115200, polled.  Safe to
 * call before the allocator: it only touches IO ports.  No-op on machines
 * without a UART. */
void serial8250_early_init(void);

/* Write one character / a NUL-terminated string to the UART.  When no UART
 * was probed these return immediately. */
void serial8250_putc(char c);
void serial8250_puts(const char *s);

#endif
