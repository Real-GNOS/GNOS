/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ata.h — legacy ATA (IDE) disks over programmed I/O. (GPLv2)
 *
 * This is the first real block device in GNOS.  Everything before it -- the
 * ext2 root, the FAT volume -- lived in the initrd the bootloader dropped in
 * RAM, which meant "writable until you switch the machine off".  A disk is
 * what makes an installer possible at all.
 *
 * Two backends sit behind this header.  The legacy PATA side deliberately
 * uses the oldest interface the PC has: the fixed legacy port pairs at
 * 0x1F0/0x3F6 and 0x170/0x376, programmed I/O, no DMA, no interrupts,
 * polling for completion.  That costs throughput -- every sector crosses the
 * bus 16 bits at a time through the CPU -- and buys three things worth more
 * here:
 *
 *   - no PCI enumeration, no bus-mastering setup, no scatter/gather lists;
 *   - no interrupt handler, so nothing to get wrong about IRQ 14/15 routing
 *     through the legacy PIC or the IOAPIC;
 *   - it works on the plainest QEMU invocation, with no -device at all.
 *
 * When the chipset carries an AHCI controller, the SATA disks and CD-ROMs
 * behind it are probed by drivers/block/ata/sata/ and land in the same
 * namespace: /dev/sda, /dev/sdb, ... and their MBR/GPT partitions as
 * /dev/sda1 .. /dev/sdaN, the letters continuing across backends.  A
 * partition node is a *window*: the same disk, with an offset added and the
 * length clamped, which is all a partition has ever been.  ATAPI CD-ROMs are
 * published as /dev/sr0, /dev/sr1, ... and are read-only.
 *
 * The implementation follows Uinxed's drivers/block/ata/ layout: pata/ide.c
 * owns the legacy ATA register protocol, pata/atapi.c the SCSI packets
 * tunnelled over it, sata/ahci.c the AHCI controller and sata/satapi.c the
 * SCSI packets over it, and ata_block.c the GNOS-specific /dev nodes and
 * partition scan.
 */
#ifndef GNOS_ATA_H
#define GNOS_ATA_H

#include <stdint.h>

/* Sector size.  Every ATA disk the legacy interface can address uses
 * 512-byte logical sectors; ATAPI CDs use 2048 but that is the packet
 * device's own d->secsize. */
#define ATA_SECTOR 512

/* Probe both channels, publish /dev/sd* and /dev/sr* and their partitions,
 * and register with the subsystem table.  Returns the number of disks
 * found.  Safe to call more than once: only the first pass probes. */
int ata_init(void);

/* Whole-disk boot read: copy the first used partition of disk 0 into dst
 * (cap bytes).  Returns the byte count, or 0 on no-disk / no-partition /
 * too-small buffer / I/O error. */
int ata_read_boot_partition(uint8_t *dst, uint32_t cap);

#endif /* GNOS_ATA_H */
