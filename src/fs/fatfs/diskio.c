/* SPDX-License-Identifier: GPL-2.0 */
/*
 * diskio.c — the GNOS side of FatFs: a block device as a FAT volume.
 * (GPLv2)
 *
 * FatFs talks to the world through five functions, and this file is all
 * there is to them: the "drive" is whatever /dev node the mount path
 * attached (fatfs_attach), a sector is 512 bytes, and every access is a
 * synchronous read or write at a byte offset.  There is no DMA bounce
 * buffer to worry about -- the caller owns the memory -- and no partition
 * table parsing here either: the VFS already publishes partitions as
 * their own /dev nodes with their own lengths.
 */
#include <stdint.h>

#include "ff.h"
#include "diskio.h"
#include "vfs.h"
#include "kstring.h"
#include "debugcon.h"

#define FAT_SECTOR_SIZE 512

/* The device a mounted FAT volume lives on (NULL until fatfs_attach). */
static vfs_node_t *g_fat_dev;
static uint64_t    g_fat_sectors;

/* Attach a /dev node as FatFs drive 0.  `sectors` is the capacity the
 * node reported; it is what GET_SECTOR_COUNT answers with. */
int fatfs_attach(vfs_node_t *dev, uint64_t sectors)
{
    if (!dev)
        return -1;
    g_fat_dev     = dev;
    g_fat_sectors = sectors;
    return 0;
}

void fatfs_detach(void)
{
    g_fat_dev     = NULL;
    g_fat_sectors = 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return g_fat_dev ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    if (!g_fat_dev || !g_fat_dev->ops || !g_fat_dev->ops->read)
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_fat_dev || !g_fat_dev->ops || !g_fat_dev->ops->read)
        return RES_NOTRDY;
    if (g_fat_sectors && (uint64_t)count + sector > g_fat_sectors)
        return RES_PARERR;

    uint64_t off = (uint64_t)sector * FAT_SECTOR_SIZE;
    uint32_t len = (uint32_t)count * FAT_SECTOR_SIZE;

    int32_t n = g_fat_dev->ops->read(g_fat_dev, off, buff, len);
    if (n < 0)
        return RES_ERROR;
    {
        /* TEMP: first six reads: offset, length, sig when page-aligned */
        static int calls;
        if (calls < 6) {
            calls++;
            dbg_puts("FATDISK[");
            dbg_puts_dec((uint32_t)calls);
            dbg_puts("] off=");
            dbg_puts_hex(off);
            dbg_puts(" n=");
            dbg_puts_dec((uint32_t)n);
            if (off % 512 == 0 && n >= 512) {
                dbg_puts(" sig:");
                dbg_puts_hexn(buff[510], 2);
                dbg_puts_hexn(buff[511], 2);
                dbg_puts(" oem:");
                for (int i = 3; i < 11; i++) {
                    char c[2] = { (char)buff[i], 0 };
                    dbg_puts(c);
                }
            }
            dbg_puts("\r\n");
        }
    }
    /* A short read means the device ended before the volume did; zero-fill
     * the rest so FatFs sees a gap rather than stale buffer contents. */
    if ((uint32_t)n < len)
        memset(buff + n, 0, len - (uint32_t)n);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!g_fat_dev || !g_fat_dev->ops)
        return RES_NOTRDY;
    if (!g_fat_dev->ops->write)
        return RES_WRPRT;                /* read-only device */
    if (g_fat_sectors && (uint64_t)count + sector > g_fat_sectors)
        return RES_PARERR;

    uint64_t off = (uint64_t)sector * FAT_SECTOR_SIZE;
    uint32_t len = (uint32_t)count * FAT_SECTOR_SIZE;

    int32_t n = g_fat_dev->ops->write(g_fat_dev, off, buff, len);
    if (n < 0)
        return RES_ERROR;
    if ((uint32_t)n < len)
        return RES_ERROR;                /* a partial write loses data */
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    if (!g_fat_dev)
        return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;                   /* every write is synchronous */
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)g_fat_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = FAT_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;              /* erase block granularity: none */
        return RES_OK;
    case CTRL_TRIM:
        return RES_OK;                   /* nothing to discard */
    default:
        return RES_PARERR;
    }
}
