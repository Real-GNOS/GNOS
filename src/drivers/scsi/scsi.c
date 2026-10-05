/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scsi.c — the SCSI bus layer. (GPLv2)
 *
 * Command construction is where storage transports overlap the most, and
 * that is exactly what this file centralises: build the CDB, hand it to
 * the device's transport, interpret the reply.  A new transport (USB
 * MSC today, ATAPI or a real HBA later) implements one function and gets
 * INQUIRY/scan/READ/WRITE for free.
 *
 * Chunks: a READ(10)/WRITE(10) transfer length field is 16 bits, so long
 * I/O walks the LBA in bounded chunks.  The chunk size stays below the
 * transports' comfort zone (USB bulk bursts in particular).
 */
#include <stdint.h>
#include <stddef.h>

#include "scsi.h"
#include "kstring.h"
#include "debugcon.h"
#include "vfs.h"

#define SCSI_MAX_DEV 8
#define SCSI_CHUNK   32u /* blocks per READ(10)/WRITE(10) */

static scsi_device_t g_scsi_devs[SCSI_MAX_DEV];
static int           g_scsi_count;

int scsi_count(void)
{
    return g_scsi_count;
}

scsi_device_t *scsi_device_by_index(int i)
{
    if (i < 0 || i >= g_scsi_count)
        return NULL;
    return &g_scsi_devs[i];
}

static int scsi_exec(scsi_device_t *d, uint8_t *cdb, uint8_t cdb_len, void *data, uint32_t data_len,
                     int dir_in)
{
    if (!d || !d->xport.execute)
        return -1;
    return d->xport.execute(d->xport.host, cdb, cdb_len, data, data_len, dir_in);
}

/* ---- TEST UNIT READY (00): no data, completes silently ------------------ */

int scsi_test_unit_ready(scsi_device_t *d)
{
    if (!d || !d->live)
        return -E_NODEV;
    uint8_t cdb[12] = {0};
    return scsi_exec(d, cdb, 12, NULL, 0, 0) < 0 ? -E_IO : 0;
}

/* ---- INQUIRY (12): vendor/product identification ------------------------ */

static int scsi_inquiry(scsi_device_t *d)
{
    uint8_t cdb[12] = {0x12, 0};
    cdb[4]          = 36; /* standard INQUIRY data */
    uint8_t r[36];
    memset(r, 0, sizeof r);
    if (scsi_exec(d, cdb, 12, r, sizeof r, 1) < 0)
        return -1;

    memcpy(d->vendor, r + 8, 8);
    d->vendor[8] = 0;
    memcpy(d->product, r + 16, 16);
    d->product[16] = 0;
    return 0;
}

/* ---- READ CAPACITY (25): geometry --------------------------------------- */

static int scsi_read_capacity(scsi_device_t *d)
{
    uint8_t cdb[12] = {0x25, 0};
    uint8_t r[8];
    memset(r, 0, sizeof r);
    if (scsi_exec(d, cdb, 12, r, sizeof r, 1) < 0)
        return -1;

    uint64_t last =
        ((uint64_t)r[0] << 24) | ((uint64_t)r[1] << 16) | ((uint64_t)r[2] << 8) | (uint64_t)r[3];
    uint32_t bsize =
        ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | (uint32_t)r[7];
    if (!bsize)
        return -1;
    d->nblocks     = last + 1;
    d->sector_size = bsize;
    return 0;
}

/* ---- attach + scan ------------------------------------------------------ */

int scsi_device_add(scsi_device_t *d)
{
    if (!d || !d->xport.execute)
        return -E_INVAL;
    if (g_scsi_count >= SCSI_MAX_DEV)
        return -E_NOMEM;

    scsi_device_t *slot = &g_scsi_devs[g_scsi_count];
    *slot               = *d;
    slot->live          = 1;

    if (scsi_inquiry(slot) < 0 || scsi_read_capacity(slot) < 0) {
        memset(slot, 0, sizeof *slot);
        return -E_IO;
    }
    slot->ready = (scsi_test_unit_ready(slot) == 0);

    g_scsi_count++;
    dbg_puts("SCSI: sd");
    dbg_puts_dec((uint32_t)(g_scsi_count - 1));
    dbg_puts(" ");
    dbg_puts(slot->vendor);
    dbg_puts(" ");
    dbg_puts(slot->product);
    dbg_puts(" ");
    dbg_puts_dec((uint32_t)(slot->nblocks >> 1));
    dbg_puts(" MiB\r\n");
    return 0;
}

/* ---- READ(10) / WRITE(10) ----------------------------------------------- */

int scsi_read_blocks(scsi_device_t *d, uint64_t lba, void *buf, uint32_t nblocks)
{
    if (!d || !d->live || !nblocks)
        return -E_INVAL;
    uint8_t *out = buf;
    while (nblocks) {
        uint32_t chunk   = nblocks > SCSI_CHUNK ? SCSI_CHUNK : nblocks;
        uint8_t  cdb[12] = {0x28, 0}; /* READ(10) */
        cdb[2]           = (uint8_t)(lba >> 24);
        cdb[3]           = (uint8_t)(lba >> 16);
        cdb[4]           = (uint8_t)(lba >> 8);
        cdb[5]           = (uint8_t)lba;
        cdb[7]           = (uint8_t)(chunk >> 8);
        cdb[8]           = (uint8_t)chunk;
        if (scsi_exec(d, cdb, 12, out, chunk * d->sector_size, 1) < 0)
            return -E_IO;
        out += chunk * d->sector_size;
        lba += chunk;
        nblocks -= chunk;
    }
    return 0;
}

int scsi_write_blocks(scsi_device_t *d, uint64_t lba, const void *buf, uint32_t nblocks)
{
    if (!d || !d->live || !nblocks)
        return -E_INVAL;
    const uint8_t *in = buf;
    while (nblocks) {
        uint32_t chunk   = nblocks > SCSI_CHUNK ? SCSI_CHUNK : nblocks;
        uint8_t  cdb[12] = {0x2A, 0}; /* WRITE(10) */
        cdb[2]           = (uint8_t)(lba >> 24);
        cdb[3]           = (uint8_t)(lba >> 16);
        cdb[4]           = (uint8_t)(lba >> 8);
        cdb[5]           = (uint8_t)lba;
        cdb[7]           = (uint8_t)(chunk >> 8);
        cdb[8]           = (uint8_t)chunk;
        if (scsi_exec(d, cdb, 12, (void *)in, chunk * d->sector_size, 0) < 0)
            return -E_IO;
        in += chunk * d->sector_size;
        lba += chunk;
        nblocks -= chunk;
    }
    return 0;
}
