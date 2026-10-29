/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ide.c — legacy ATA (IDE) disks over programmed I/O. (GPLv2)
 *
 * The protocol, in one paragraph.  Each channel owns eight command registers
 * at a fixed base (0x1F0 for the primary, 0x170 for the secondary) plus a
 * control register a little higher up (0x3F6 / 0x376).  You pick a drive by
 * writing the drive/head register, load a sector count and a sector address,
 * write a command byte, and then wait: the status register's BSY bit says the
 * drive is thinking and DRQ says it has a sector's worth of data waiting.
 * Each sector then moves through the 16-bit data register, 256 `in`/`out`
 * instructions at a time.  That is the whole interface, and it has been
 * compatible since 1986.
 *
 * Three details cause more bugs here than everything else combined:
 *
 *   - After selecting a drive you must let 400 ns pass before the status
 *     register means anything, and the canonical way to spend it is four
 *     reads of the *alternate* status port (which, unlike the real one, does
 *     not acknowledge interrupts).
 *
 *   - An absent drive does not report itself absent; the bus floats and reads
 *     back 0xFF.  Every wait loop is therefore bounded, because the failure
 *     mode of an unbounded one is a kernel that hangs at boot on hardware
 *     that merely does not exist.
 *
 *   - A CD-ROM answers on the same ports and fails IDENTIFY with a signature
 *     in the two LBA registers.  QEMU puts the boot ISO on the secondary
 *     master, so this is not a theoretical case: it is on every boot.
 *
 * This file is the ATA half only.  The packet layer lives in atapi.c and the
 * /dev nodes and partition table in ata_block.c, mirroring Uinxed's
 * drivers/block/ata/pata split.
 */
#include <stdint.h>

#include "ide.h"
#include "atapi.h"
#include "io.h"

/* ---- low-level waiting ------------------------------------------------- */
void ata_delay400(const ata_disk_t *d)
{
    for (int i = 0; i < 4; i++)
        (void)inb(d->ctrl);
}

int ata_wait_ready(const ata_disk_t *d)
{
    for (uint32_t i = 0; i < ATA_WAIT_SPINS; i++) {
        uint8_t st = inb(d->io + ATA_REG_STATUS);
        if (st == 0xFF) /* floating bus: nothing is there */
            return -1;
        if (!(st & ATA_SR_BSY))
            return st;
    }
    return -1;
}

int ata_wait_drq(const ata_disk_t *d)
{
    for (uint32_t i = 0; i < ATA_WAIT_SPINS; i++) {
        uint8_t st = inb(d->io + ATA_REG_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & (ATA_SR_ERR | ATA_SR_DF))
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            return 0;
    }
    return -1;
}

void ata_select(const ata_disk_t *d, uint8_t head_bits)
{
    outb(d->io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | head_bits));
    ata_delay400(d);
}

/* ---- IDENTIFY ---------------------------------------------------------- */
/*
 * IDENTIFY DEVICE returns 256 words describing the drive.  The fields that
 * matter here:
 *
 *   words 27..46  model string, ASCII, byte-swapped within each word
 *   words 60..61  capacity in LBA28 sectors (0 if the drive is too big)
 *   word  83 b10  "supports 48-bit addressing"
 *   words 100..103 capacity in LBA48 sectors
 */
int ata_identify(ata_disk_t *d)
{
    /* Interrupts off for this channel: the driver polls, and a drive that
     * asserts IRQ 14 with nobody listening leaves the PIC latched. */
    outb(d->ctrl, ATA_DCR_NIEN);

    ata_select(d, 0);
    outb(d->io + ATA_REG_SECCOUNT, 0);
    outb(d->io + ATA_REG_LBA0, 0);
    outb(d->io + ATA_REG_LBA1, 0);
    outb(d->io + ATA_REG_LBA2, 0);
    outb(d->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400(d);

    /*
     * A status of zero means "no device here at all" -- distinct from the
     * floating 0xFF, and equally final.
     */
    uint8_t st = inb(d->io + ATA_REG_STATUS);
    if (st == 0 || st == 0xFF)
        return 0;

    if (ata_wait_ready(d) < 0)
        return 0;

    /*
     * ATAPI (a CD-ROM) refuses IDENTIFY and leaves its signature behind in
     * the LBA mid/high registers: 0x14/0xEB for ATAPI, 0x69/0x96 for the
     * SATA bridge signature some controllers report.  Send IDENTIFY PACKET
     * DEVICE instead and keep going -- the drive is worth having (it is the
     * boot media).
     */
    uint8_t lba1      = inb(d->io + ATA_REG_LBA1);
    uint8_t lba2      = inb(d->io + ATA_REG_LBA2);
    uint8_t is_packet = 0;
    if ((lba1 == 0x14 && lba2 == 0xEB) || (lba1 == 0x69 && lba2 == 0x96))
        is_packet = 1;
    else if (lba1 || lba2) /* some other non-ATA signature */
        return 0;

    outb(d->io + ATA_REG_COMMAND, is_packet ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY);
    if (ata_wait_drq(d) < 0)
        return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(d->io + ATA_REG_DATA);

    d->atapi   = is_packet;
    d->secsize = is_packet ? 2048u : 512u;
    d->lba48   = (id[83] & (1u << 10)) != 0;

    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[103] << 48);
    d->sectors = (d->lba48 && lba48) ? lba48 : lba28;
    if (d->atapi && !d->sectors) {
        /* Removable media: IDENTIFY PACKET carries no capacity -- ask the
         * drive with READ CAPACITY (10). */
        uint8_t cap[12] = {GPCMD_READ_CAPACITY, 0};
        uint8_t r[8];
        if (atapi_packet_read(d, cap, 8, r) == 0) {
            uint64_t last = ((uint64_t)r[0] << 24) | ((uint64_t)r[1] << 16) |
                            ((uint64_t)r[2] << 8) | (uint64_t)r[3];
            d->sectors = last + 1;
        }
    }
    if (!d->sectors)
        return 0;

    /* The model string is stored big-endian within each word, so it reads as
     * "TQMEU DAHRDDI SK" until the halves are swapped back. */
    for (int i = 0; i < 20; i++) {
        d->model[i * 2]     = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = 0;

    return 1;
}

/* ---- sector transfer --------------------------------------------------- */
/*
 * Address one sector and issue a command.  LBA48 is used whenever the drive
 * has it, not only when the address needs it: the two register layouts differ
 * only in that the 48-bit one writes the high bytes first, and picking a
 * single path removes a whole class of "works until the disk gets big" bug.
 */
static int ata_cmd_one(const ata_disk_t *d, uint64_t lba, uint8_t cmd28, uint8_t cmd48)
{
    if (ata_wait_ready(d) < 0)
        return -1;

    if (d->lba48) {
        outb(d->io + ATA_REG_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
        ata_delay400(d);
        outb(d->io + ATA_REG_SECCOUNT, 0); /* count high */
        outb(d->io + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        outb(d->io + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        outb(d->io + ATA_REG_LBA2, (uint8_t)(lba >> 40));
        outb(d->io + ATA_REG_SECCOUNT, 1); /* count low  */
        outb(d->io + ATA_REG_LBA0, (uint8_t)(lba));
        outb(d->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + ATA_REG_COMMAND, cmd48);
    } else {
        /* In LBA28 the top four address bits live in the drive register,
         * which is also where the master/slave bit is. */
        outb(d->io + ATA_REG_DRIVE, (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        ata_delay400(d);
        outb(d->io + ATA_REG_FEATURES, 0);
        outb(d->io + ATA_REG_SECCOUNT, 1);
        outb(d->io + ATA_REG_LBA0, (uint8_t)(lba));
        outb(d->io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + ATA_REG_COMMAND, cmd28);
    }
    ata_delay400(d);
    return 0;
}

int ata_read_sector(const ata_disk_t *d, uint64_t lba, uint8_t *buf)
{
    if (ata_cmd_one(d, lba, ATA_CMD_READ_PIO, ATA_CMD_READ_PIO_EXT) < 0)
        return -1;
    if (ata_wait_drq(d) < 0)
        return -1;

    /*
     * Assemble the words byte by byte.  The buffer may be a partition
     * window's caller-supplied pointer, which is not guaranteed to be
     * 2-byte aligned; casting it to uint16_t * would be undefined behaviour
     * even though x86 tolerates the load.
     */
    for (int i = 0; i < ATA_SECTOR; i += 2) {
        uint16_t w = inw(d->io + ATA_REG_DATA);
        buf[i]     = (uint8_t)(w & 0xFF);
        buf[i + 1] = (uint8_t)(w >> 8);
    }

    int st = ata_wait_ready(d);
    if (st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)))
        return -1;
    return 0;
}

int ata_write_sector(const ata_disk_t *d, uint64_t lba, const uint8_t *buf)
{
    if (ata_cmd_one(d, lba, ATA_CMD_WRITE_PIO, ATA_CMD_WRITE_PIO_EXT) < 0)
        return -1;
    if (ata_wait_drq(d) < 0)
        return -1;

    /*
     * The data-out loop must not be tightened into a `rep outsw`.  The ATA
     * specification requires a gap between consecutive word writes -- real
     * drives latch on the trailing edge and can drop a word without one --
     * and a jmp-to-next-instruction is the traditional way to provide it.
     */
    for (int i = 0; i < ATA_SECTOR; i += 2) {
        uint16_t w = (uint16_t)(buf[i] | ((uint16_t)buf[i + 1] << 8));
        outw(d->io + ATA_REG_DATA, w);
        io_delay();
    }

    /*
     * The drive goes busy while it commits the sector; the FLUSH command
     * must not be written until it is ready again, or a strict device is
     * entitled to abort it.
     */
    int st = ata_wait_ready(d);
    if (st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)))
        return -1;

    /*
     * The write is in the drive's cache until it is flushed; without this a
     * power cut (or a QEMU exit) between the write and the next boot loses
     * data the caller was told had landed.
     */
    outb(d->io + ATA_REG_COMMAND, d->lba48 ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
    ata_delay400(d);
    st = ata_wait_ready(d);
    if (st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)))
        return -1;
    return 0;
}
