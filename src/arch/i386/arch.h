/* SPDX-License-Identifier: GPL-2.0 */
/*
 * i386 architecture backend.
 *
 * Scaffolding: the kernel core is 64-bit-clean (fixed-width types
 * throughout) but the descriptor tables, context switch and AP trampoline
 * here are still 32-bit ports waiting to be written.  A build with
 * ARCH=i386 today fails at link time with these symbols missing -- which
 * is the honest answer until the port exists.
 */
#ifndef GNOS_ARCH_I386_H
#define GNOS_ARCH_I386_H

/* Reserved for the 32-bit port.  The x86_64 backend in ../x86_64 defines
 * the contract: gdt_init/idt_init/irq wiring plus the switch.asm context
 * switch the scheduler calls. */

#endif /* GNOS_ARCH_I386_H */
