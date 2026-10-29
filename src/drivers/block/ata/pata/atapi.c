/* SPDX-License-Identifier: GPL-2.0 */
/*
 * atapi.c — the SCSI command set tunnelled over an ATA channel. (GPLv2)
 *
 * An ATAPI drive is a SCSI device whose command block happens to be
 * delivered through the ATA task file.  The handshake is a short state
 * machine:
 *
 *   select, wait not-BSY, load the byte count, write PACKET,
 *   wait for the CDB-delivery DRQ, write the 12-byte CDB,
 *   wait for the data DRQ (a CD spin-up can take seconds on first access),
 *   read the words, wait for completion, check ERR.
 *
 * Two details are easy to get wrong -- both used to be wrong here:
 *
 *   - The byte-count register is 16 bits.  A 32-sector transfer is exactly
 *     65536 bytes, which the register encodes as zero; a uint16_t parameter
 *     therefore wrapped to "no data" and silently dropped the transfer.
 *   - A soft reset clears the drive's sense data, so REQUEST SENSE has to be
 *     issued *before* SRST, not after.
 */
#include <stdint.h>

#include "ide.h"
#include "atapi.h"
#include "io.h"
#include "kstring.h"
#include "debugcon.h"

/* Print the failure registers and ask the drive why.  Kept here rather than
 * in ata_block.c so a packet error is diagnosed where it happened. */
static void atapi_dump_error(const ata_disk_t *d, uint8_t opcode)
{
    dbg_puts("ATAPI: ABRT st=");
    dbg_puts_hexn(inb(d->io + ATA_REG_STATUS), 2);
    dbg_puts(" err=");
    dbg_puts_hexn(inb(d->io + ATA_REG_ERROR), 2);
    dbg_puts(" cmd=");
    dbg_puts_hexn(opcode, 2);
    dbg_puts("\r\n");
}

static void atapi_request_sense(const ata_disk_t *d)
{
    uint8_t spkt[12] = {GPCMD_REQUEST_SENSE, 0, 0, 0, 18, 0};
    uint8_t sense[18];

    memset(sense, 0, sizeof sense);
    if (atapi_packet_read(d, spkt, 18, sense) == 0) {
        dbg_puts("ATAPI: sense SK=");
        dbg_puts_hexn(sense[2] & 0x0F, 2);
        dbg_puts(" ASC=");
        dbg_puts_hexn(sense[12], 2);
        dbg_puts(" ASCQ=");
        dbg_puts_hexn(sense[13], 2);
        dbg_puts("\r\n");
    }
}

/*
 * Common failure path.  REQUEST SENSE must precede the reset (SRST clears
 * the sense data), and REQUEST SENSE itself must not ask for sense again:
 * that recursion would only stop at the stack guard.
 */
static int atapi_fail(const ata_disk_t *d, uint8_t opcode)
{
    atapi_dump_error(d, opcode);
    if (opcode != GPCMD_REQUEST_SENSE)
        atapi_request_sense(d);
    atapi_soft_reset(d);
    return -1;
}

void atapi_soft_reset(const ata_disk_t *d)
{
    /* Assert SRST without re-enabling interrupts: nIEN stays set for the
     * whole reset, so the polled driver cannot leave IRQ14/15 latched. */
    outb(d->ctrl, ATA_DCR_NIEN | ATA_DCR_SRST);
    ata_delay400(d);
    ata_delay400(d);
    outb(d->ctrl, ATA_DCR_NIEN);
    ata_delay400(d);
    ata_wait_ready(d);
}

int atapi_packet_read(const ata_disk_t *d, const uint8_t *pkt, uint32_t nbytes, uint8_t *buf)
{
    /* The drive's byte-count limit is 16 bits; 65536 is encoded as zero. */
    if (nbytes > 65536)
        return -1;
    uint16_t byte_count = (nbytes == 65536) ? 0 : (uint16_t)nbytes;

    outb(d->io + ATA_REG_DRIVE, (uint8_t)(0xA0 | (d->slave << 4)));
    ata_delay400(d);
    if (ata_wait_ready(d) < 0)
        return -1;

    outb(d->io + ATA_REG_FEATURES, 0); /* PIO, no overlap/DMA */
    outb(d->io + ATA_REG_SECCOUNT, (uint8_t)(byte_count & 0xFF));
    outb(d->io + ATA_REG_LBA0, (uint8_t)(byte_count >> 8));
    outb(d->io + ATA_REG_COMMAND, ATA_CMD_PACKET);

    /* CDB delivery: the drive raises DRQ (with BSY clear) for it. */
    int have_drq = 0;
    for (uint32_t i = 0; i < ATA_WAIT_SPINS; i++) {
        uint8_t st = inb(d->io + ATA_REG_STATUS);
        if (st == 0xFF)
            return -1; /* floating bus */
        if (st & ATA_SR_BSY)
            continue;
        if (st & ATA_SR_ERR)
            return atapi_fail(d, pkt[0]);
        if (st & ATA_SR_DRQ) {
            have_drq = 1;
            break;
        }
    }
    if (!have_drq) {
        dbg_puts("ATAPI: CDB timeout cmd=");
        dbg_puts_hexn(pkt[0], 2);
        dbg_puts("\r\n");
        atapi_soft_reset(d);
        return -1;
    }

    for (int i = 0; i < 6; i++) /* the packet, as words */
        outw(d->io + ATA_REG_DATA, (uint16_t)(pkt[i * 2] | ((uint16_t)pkt[i * 2 + 1] << 8)));

    /* Data phase.  A CD spin-up can take seconds, hence the larger budget. */
    uint32_t words = (nbytes + 1) / 2;
    for (uint32_t i = 0; i < ATA_WAIT_SPINS * 200; i++) {
        uint8_t st = inb(d->io + ATA_REG_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & ATA_SR_BSY)
            continue;
        if (st & ATA_SR_ERR)
            return atapi_fail(d, pkt[0]);
        if (st & ATA_SR_DRQ) {
            for (uint32_t j = 0; j < words; j++) {
                uint16_t w   = inw(d->io + ATA_REG_DATA);
                uint32_t off = j * 2;
                buf[off]     = (uint8_t)(w & 0xFF);
                if (off + 1 < nbytes)
                    buf[off + 1] = (uint8_t)(w >> 8);
            }

            int fin = ata_wait_ready(d);
            if (fin < 0 || (fin & (ATA_SR_ERR | ATA_SR_DF)))
                return atapi_fail(d, pkt[0]);
            return 0;
        }
        /* neither BSY nor DRQ: the command finished without data */
        break;
    }

    dbg_puts("ATAPI: data timeout cmd=");
    dbg_puts_hexn(pkt[0], 2);
    dbg_puts("\r\n");
    atapi_soft_reset(d);
    return -1;
}

/* READ(10) over ATAPI: `nsect` 2048-byte sectors starting at `lba`. */
int atapi_read(const ata_disk_t *d, uint64_t lba, uint32_t nsect, uint8_t *buf)
{
    while (nsect) {
        /* 32 * 2048 is the largest transfer the 16-bit byte count holds, so
         * one packet per 32 sectors and no more. */
        uint32_t now     = nsect > 32u ? 32u : nsect;
        uint8_t  pkt[12] = {
            GPCMD_READ_10,
            0,
            (uint8_t)(lba >> 24),
            (uint8_t)(lba >> 16),
            (uint8_t)(lba >> 8),
            (uint8_t)lba,
            0,
            (uint8_t)(now >> 8),
            (uint8_t)now,
            0,
            0,
            0,
        };
        uint32_t nbytes = now * d->secsize;

        if (atapi_packet_read(d, pkt, nbytes, buf) < 0)
            return -1;
        buf += nbytes;
        lba += now;
        nsect -= now;
    }
    return 0;
}
