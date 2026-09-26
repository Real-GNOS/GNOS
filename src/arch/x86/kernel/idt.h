/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idt.h — 64-bit interrupt descriptor table. (GPLv2)
 */
#ifndef GNUCOS_IDT_H
#define GNUCOS_IDT_H

#include <stdint.h>
#include "panic.h"

#define IRQ_BASE      0x20    /* PIC vectors are remapped to 0x20..0x2F */
#define MSI_VECTOR_BASE 0xE0  /* dedicated MSI/MSI-X pool, 16 vectors    */
#define SYSCALL_VECTOR 0x80

typedef void (*irq_handler_t)(regs_t *r);

/* ---- /proc/interrupts snapshot -----------------------------------------
 * Per-CPU hit counts for every dispatchable interrupt source, taken while
 * interrupts are off in the reader's context.  PIC vectors are the remapped
 * 0x20..0x2F range; MSI/MSI-X share the dedicated 0xE0..0xEF pool. */
#define IRQSTAT_CPUS 4

typedef struct {
    uint32_t      pic[16][IRQSTAT_CPUS];
    uint32_t      msi[16][IRQSTAT_CPUS];
    uint32_t      lapic_timer[IRQSTAT_CPUS];
    const char   *pic_name[16];       /* registered handler names or NULL */
    const char   *msi_name[16];
} irqstat_t;

void irqstat_snapshot(irqstat_t *out);

/* Fill in all 256 gates and load the IDT.  Also remaps and masks the PIC. */
void idt_init(void);

/* Reload the (already-built) IDT on the current CPU.  idt_init() calls this
 * once for the BSP; APs call it too so their local IDTR points at the shared
 * table.  Interrupts stay disabled on APs, so this is just a safety net. */
void idt_load(void);

/* Register a handler for a hardware IRQ (0..15) and unmask it. */
void irq_install(unsigned irq, irq_handler_t fn, const char *name);
void msi_install(unsigned vec, irq_handler_t fn, const char *name);

/* Register the handler for int 0x80 (the POSIX syscall gate). */
void syscall_install(irq_handler_t fn);

#endif
