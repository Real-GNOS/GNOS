/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scsi.h — the SCSI bus layer: command construction, device scan and the
 * block read/write surface every storage transport shares. (GPLv2)
 *
 * A transport (USB Mass Storage's CBW/CSW exchange, ATAPI's PACKET
 * protocol, a real HBA later) only knows how to move a CDB and its data.
 * Everything above — INQUIRY parsing, TEST UNIT READY, READ CAPACITY,
 * the READ(10)/WRITE(10) block loops — lives here once, so the block
 * devices those transports expose behave identically.
 */
#ifndef GNUCOS_SCSI_H
#define GNUCOS_SCSI_H

#include <stdint.h>

typedef struct scsi_device scsi_device_t;

/* The transport half: execute one CDB, moving data either direction.
 * Returns the number of bytes transferred, or -1. */
typedef struct {
    int (*execute)(void *host, const uint8_t *cdb, uint8_t cdb_len, void *data, uint32_t data_len,
                   int dir_in);
    void *host;
} scsi_transport_t;

struct scsi_device {
    scsi_transport_t xport;
    uint8_t          lun;
    uint32_t         sector_size; /* usually 512 or 2048 */
    uint64_t         nblocks;     /* capacity, in sectors */
    char             vendor[9];   /* INQUIRY, NUL-terminated */
    char             product[17];
    uint8_t          ready; /* TEST UNIT READY passed */
    uint8_t          live;
};

/* Attach a device on a transport and scan it (INQUIRY + TEST UNIT READY +
 * READ CAPACITY).  Returns 0 or a negative errno. */
int scsi_device_add(scsi_device_t *d);

/* Re-run TEST UNIT READY on one device (media arrival/removal). */
int scsi_test_unit_ready(scsi_device_t *d);

/* Block surface in device sectors. */
int scsi_read_blocks(scsi_device_t *d, uint64_t lba, void *buf, uint32_t nblocks);
int scsi_write_blocks(scsi_device_t *d, uint64_t lba, const void *buf, uint32_t nblocks);

/* Registry (for /dev publishing and devtmpfs enumeration). */
int            scsi_count(void);
scsi_device_t *scsi_device_by_index(int i);

#endif
