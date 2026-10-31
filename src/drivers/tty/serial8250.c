/* SPDX-License-Identifier: GPL-2.0 */
/*
 * serial8250.c - real 16550/8250 UART driver, COM1 (0x3F8). (GPLv2)
 *
 * This is the backend for the `console=ttyS0` kernel command-line option.
 * It is a minimal polled (no-IRQ) driver: enough to be a boot console and a
 * real serial port once userspace opens /dev/ttyS0.  The probe pokes the
 * scratch register (offset 7), which a conformant 16550 implements and
 * QEMU's uart model implements, so on hardware without a UART the puts
 * calls silently no-op instead of hanging on a THRE bit that never clears.
 */
#include <stdint.h>
#include "serial8250.h"

#define COM1 0x3F8

/* The UART lives behind x86 IO ports.  These match the local helpers used by
 * the PIT/CMOS and debugcon code in the tree. */
static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static int g_present;

void serial8250_early_init(void)
{
    /* Probe: the scratch register must round-trip a value. */
    outb(COM1 + 7, 0x55);
    if (inb(COM1 + 7) != 0x55)
        return;
    outb(COM1 + 7, 0xAA);
    if (inb(COM1 + 7) != 0xAA)
        return;
    g_present = 1;

    outb(COM1 + 1, 0x00); /* IER: disable all interrupts (polled) */
    outb(COM1 + 3, 0x80); /* LCR: DLAB on to program the divisor */
    outb(COM1 + 0, 0x01); /* DLL: divisor low  -> 115200 baud (1.8432 MHz) */
    outb(COM1 + 1, 0x00); /* DLM: divisor high */
    outb(COM1 + 3, 0x03); /* LCR: 8 data bits, no parity, 1 stop (8N1) */
    outb(COM1 + 2, 0xC7); /* FCR: enable FIFO, clear, 14-byte trigger */
    outb(COM1 + 4, 0x03); /* MCR: assert DTR and RTS (loopback off) */
}

void serial8250_putc(char c)
{
    if (!g_present)
        return;
    if (c == '\n')
        serial8250_putc('\r');
    while (!(inb(COM1 + 5) & 0x20)) /* LSR: wait for THR empty */
        ;
    outb(COM1 + 0, (uint8_t)c);
}

void serial8250_puts(const char *s)
{
    if (!g_present)
        return;
    for (; *s; s++)
        serial8250_putc(*s);
}
