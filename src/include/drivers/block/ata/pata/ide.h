/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ide.h — the legacy PATA register protocol. (GPLv2)
 *
 * One ata_disk_t describes one drive on one channel: where its registers
 * live, whether it is master or slave, and what IDENTIFY said about it.
 * ide.c implements the ATA side (IDENTIFY, PIO read/write, flush); atapi.c
 * builds the SCSI packet layer on the same helpers, which is why the
 * register-level waits and the sector transfers are exported here.
 */
#ifndef GNOS_ATA_PATA_IDE_H
#define GNOS_ATA_PATA_IDE_H

#include <stdint.h>

#include "ata.h"
#include "ata_cmds.h"

typedef struct {
    uint16_t io;      /* command block base    */
    uint16_t ctrl;    /* control block base    */
    uint8_t  slave;   /* 0 = master, 1 = slave */
    uint8_t  lba48;
    uint8_t  atapi;   /* PACKET-device (CD-ROM): SCSI over ATA */
    uint32_t secsize; /* bytes per sector: 512 ATA, 2048 ATAPI */
    uint64_t sectors; /* capacity, in secsize-byte sectors     */
    char     model[41];
} ata_disk_t;

/* 400 ns settle after a drive select, spent on four alternate-status reads. */
void ata_delay400(const ata_disk_t *d);

/* Wait for BSY to clear.  Returns the final status, or -1 on timeout/floating
 * bus.  ERR is deliberately not treated as failure: the ATAPI probe path
 * needs the signature left behind by a failed IDENTIFY. */
int ata_wait_ready(const ata_disk_t *d);

/* Wait for BSY clear and DRQ set; ERR/DF are failures.  Returns 0 or -1. */
int ata_wait_drq(const ata_disk_t *d);

/* Select master/slave and let the selection settle. */
void ata_select(const ata_disk_t *d, uint8_t head_bits);

/* Probe one drive and fill its geometry.  Returns 1 on success, 0 when the
 * slot is empty or the device is unusable. */
int ata_identify(ata_disk_t *d);

/* One 512-byte programmed-I/O sector.  Both are alignment-safe: the caller's
 * buffer is treated as a byte array, not as a uint16_t array. */
int ata_read_sector(const ata_disk_t *d, uint64_t lba, uint8_t *buf);
int ata_write_sector(const ata_disk_t *d, uint64_t lba, const uint8_t *buf);

#endif /* GNOS_ATA_PATA_IDE_H */
