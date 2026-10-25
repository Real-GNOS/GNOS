/* SPDX-License-Identifier: GPL-2.0 */
/*
 * satapi.h — SCSI commands over an AHCI port (SATAPI / ATAPI). (GPLv2)
 *
 * A CD-ROM on a SATA cable is a SCSI device whose command block happens to
 * travel in the ATAPI CDB field of an AHCI command table instead of over a
 * real SCSI bus.  satapi.c builds the CDBs (INQUIRY, READ CAPACITY,
 * READ(12), ...) and issues them through the port's command engine, the
 * way a legacy ATAPI drive would be spoken to through the eight PATA
 * registers except with the port doing the data movement itself.
 *
 * Only the read path is implemented: a CD-ROM is a read-only medium, and
 * the /dev/sr* node this drives is published read-only too.  A write CDB
 * would additionally need the host-to-device direction bit set and the
 * bounce buffer as source, which nothing in the tree currently asks for.
 */
#ifndef GNOS_ATA_SATA_SATAPI_H
#define GNOS_ATA_SATA_SATAPI_H

#include <stddef.h>
#include <stdint.h>

#include "ahci.h"

/* ATAPI data-transfer types for the packet command. */
#define SATAPI_PROT_NODATA 0
#define SATAPI_PROT_PIO    1
#define SATAPI_PROT_DMA    2

/* One ATAPI device the AHCI probe found.  Indexed in parallel with
 * ahci_devices[] (device_idx is that index). */
typedef struct {
    uint8_t  reserved;
    uint8_t  port_idx;   /* index into ahci_ports[] */
    uint8_t  device_idx; /* index into ahci_devices[] */
    uint32_t lba_size;   /* capacity, in blk_size-byte blocks */
    uint32_t blk_size;   /* bytes per block: 2048 for a CD */
} ahci_satapi_device_t;

extern ahci_satapi_device_t ahci_satapi_devices[AHCI_MAX_DEVICES];
extern int                  ahci_satapi_device_count;

/* Ask every probed ATAPI device for its capacity (READ CAPACITY) and
 * identification (INQUIRY), filling the table and the model and size
 * fields of the matching ahci_devices[] entries.  Runs from ahci_init(),
 * after the ports are up. */
void ahci_satapi_init(void);

/* Read numsects blk_size-byte blocks from the device.  Returns 0 or a
 * negative errno. */
int ahci_satapi_read_sectors(uint8_t drive, uint8_t numsects, uint64_t lba,
                             void *buffer);

/* Execute a 12-byte SCSI CDB.  When direction is not SATAPI_PROT_NODATA the
 * first byte_limit bytes of device output are copied into buf and
 * *xfer_len is set to the byte count.  Returns 0 on success, 0xFF on error
 * or 0xFE when no command slot is free. */
uint8_t ahci_satapi_send_packet(uint8_t drive, const uint8_t *cdb,
                                uint16_t byte_limit, uint8_t direction,
                                void *buf, size_t *xfer_len);

/* READ CAPACITY: fill the last block number plus one and the block size. */
uint8_t ahci_satapi_read_capacity(uint8_t drive, uint32_t *lba_size,
                                  uint32_t *blk_size);

/* INQUIRY: vendor (8 bytes) and product (16 bytes) identification, space
 * trimmed and joined into model. */
uint8_t ahci_satapi_inquiry(uint8_t drive, char *model, size_t model_size);

#endif /* GNOS_ATA_SATA_SATAPI_H */
