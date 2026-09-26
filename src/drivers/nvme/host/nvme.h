/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nvme.h — NVMe 1.x block transport over PCI MMIO. (GPLv2)
 *
 * The driver is deliberately synchronous: one admin queue pair and one
 * I/O queue pair per controller, both completion queues polled.  No IRQ
 * routing, no async ownership -- the same trade the ATA driver made, at
 * PCIe speeds instead of 1986 PIO speeds.  Namespaces appear as
 * /dev/nvme0n1, /dev/nvme0n2, ... and their MBR/GPT partitions as
 * /dev/nvme0n1p1 .. p16, reusing the same window logic as ata.c.
 */
#ifndef GNOS_NVME_H
#define GNOS_NVME_H

#include <stdint.h>

/* Probe the PCI table for class 01/08 controllers, publish /dev/nvme* nodes
 * and register with the subsystem table.  Returns controllers initialized.
 * Safe to call more than once: only the first pass probes. */
int nvme_init(void);

/* Logical sector size.  Only 512-byte LBA formats are driven; namespaces
 * formatted otherwise are skipped at identify time. */
#define NVME_SECTOR 512

#endif /* GNOS_NVME_H */
