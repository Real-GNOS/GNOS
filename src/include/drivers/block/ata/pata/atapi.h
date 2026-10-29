/* SPDX-License-Identifier: GPL-2.0 */
/*
 * atapi.h — the SCSI packet layer tunnelled over ATA. (GPLv2)
 *
 * An ATAPI device speaks the SCSI command set, but the command block travels
 * through the same eight ATA registers as a disk command.  atapi.c owns the
 * packet handshake (PACKET, CDB delivery, PIO data phase) and the READ(10)
 * loop; ide.c calls back into it during IDENTIFY to ask a CD-ROM for its
 * capacity.
 */
#ifndef GNOS_ATA_PATA_ATAPI_H
#define GNOS_ATA_PATA_ATAPI_H

#include <stdint.h>

#include "ide.h"

/* Soft-reset the device, preserving nIEN so the polled driver does not leave
 * an interrupt latched on the PIC. */
void atapi_soft_reset(const ata_disk_t *d);

/* Send a 12-byte CDB and read up to `nbytes` of PIO data back.  `nbytes` is
 * a uint32_t because a 32-sector transfer is exactly 65536, which does not
 * fit in 16 bits even though the drive's byte-count register encodes it as
 * zero.  Returns 0 or -1. */
int atapi_packet_read(const ata_disk_t *d, const uint8_t *pkt, uint32_t nbytes, uint8_t *buf);

/* READ(10) `nsect` 2048-byte sectors starting at `lba`, in chunks small
 * enough for the packet byte-count limit. */
int atapi_read(const ata_disk_t *d, uint64_t lba, uint32_t nsect, uint8_t *buf);

#endif /* GNOS_ATA_PATA_ATAPI_H */
