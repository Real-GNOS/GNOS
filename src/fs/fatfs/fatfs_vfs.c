/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fatfs_vfs.c — FAT through FatFs, as a VFS mount. (GPLv2)
 *
 * The mount owns one FATFS object and one mount point; every path below it
 * is answered through FatFs calls on the volume.  Files are opened per
 * operation rather than kept in an open-file table: the VFS hands a node
 * around, not a descriptor, and a node that carried a live FIL would leak
 * one whenever a path is resolved just to be stat()ed.  The cost is an
 * open/seek/close per read or write -- for a filesystem we use to read a
 * USB stick, that is the right trade.
 */
#include <stdint.h>

#include "ff.h"
#include "fatfs_vfs.h"
#include "vfs.h"
#include "heap.h"
#include "kstring.h"
#include "debugcon.h"

#define FAT_MOUNT_PATH_MAX 256

static FATFS  g_fat;
static int    g_fat_ok;
static char   g_fat_mnt[FAT_MOUNT_PATH_MAX];

/* ---- node payload ------------------------------------------------------
 * A resolved node remembers the path it was resolved from, relative to the
 * volume root and always '/'-rooted, which is what FatFs wants. */
static void node_release(struct vfs_node *n)
{
    if (n && n->priv) {
        kfree(n->priv);
        n->priv = NULL;
    }
}

static int node_set_rel(struct vfs_node *n, const char *rel)
{
    size_t len = strlen(rel) + 1;
    char *p = kmalloc((uint32_t)len);
    if (!p)
        return -1;
    memcpy(p, rel, len);
    n->priv = p;
    return 0;
}

/* ---- file operations --------------------------------------------------- */

static int32_t fat_file_read(struct vfs_node *n, uint64_t off, void *buf,
                             uint32_t len)
{
    const char *rel = n->priv;
    if (!rel)
        return -E_INVAL;
    if (!len)
        return 0;

    FIL fp;
    if (f_open(&fp, rel, FA_READ) != FR_OK)
        return -E_NOENT;
    if (off && f_lseek(&fp, (FSIZE_t)off) != FR_OK) {
        f_close(&fp);
        return -E_INVAL;
    }
    UINT got = 0;
    FRESULT r = f_read(&fp, buf, len, &got);
    f_close(&fp);
    if (r != FR_OK)
        return -E_IO;
    return (int32_t)got;                 /* 0 at end of file */
}

static int32_t fat_file_write(struct vfs_node *n, uint64_t off,
                              const void *buf, uint32_t len)
{
    const char *rel = n->priv;
    if (!rel)
        return -E_INVAL;
    if (!len)
        return 0;

    FIL fp;
    /* Writing at 0 to a name that is not there yet creates it; writing
     * anywhere else must land in an existing file rather than silently
     * truncating one. */
    BYTE mode = FA_WRITE;
    if (off == 0) {
        FILINFO fno;
        if (f_stat(rel, &fno) != FR_OK)
            mode = FA_WRITE | FA_CREATE_ALWAYS;
    }
    if (f_open(&fp, rel, mode) != FR_OK)
        return -E_NOENT;
    if (off && f_lseek(&fp, (FSIZE_t)off) != FR_OK) {
        f_close(&fp);
        return -E_INVAL;
    }
    UINT wrote = 0;
    FRESULT r = f_write(&fp, buf, len, &wrote);
    f_close(&fp);
    if (r != FR_OK)
        return -E_IO;
    return (int32_t)wrote;
}

/* ---- directory operations ---------------------------------------------- */

static int32_t fat_dir_read(struct vfs_node *n, uint64_t off, void *buf,
                            uint32_t len)
{
    const char *rel = n->priv;
    const uint32_t rec = (uint32_t)sizeof(gdirent_t);
    if (!rel || off % rec || len < rec)
        return -E_INVAL;

    uint32_t skip = (uint32_t)(off / rec);
    uint32_t room = len / rec;

    DIR d;
    FILINFO fno;
    if (f_opendir(&d, rel) != FR_OK)
        return -E_NOENT;

    gdirent_t *out = (gdirent_t *)buf;
    uint32_t seen = 0, got = 0;
    while (got < room && f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if (seen++ < skip)
            continue;
        memset(&out[got], 0, rec);
        strncpy(out[got].name, fno.fname, GDIRENT_NAME - 1);
        out[got].name[GDIRENT_NAME - 1] = 0;
        out[got].size = (uint32_t)fno.fsize;
        out[got].kind = (fno.fattrib & AM_DIR) ? GK_DIR : GK_FILE;
        got++;
    }
    f_closedir(&d);
    return (int32_t)(got * rec);
}

static int32_t fat_dir_write(struct vfs_node *n, uint64_t off,
                             const void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_fat_file_ops = {
    .read = fat_file_read, .write = fat_file_write, .release = node_release
};
static const vfs_ops_t g_fat_dir_ops = {
    .read = fat_dir_read, .write = fat_dir_write, .release = node_release
};

const vfs_ops_t *const fatfs_file_ops(void) { return &g_fat_file_ops; }
const vfs_ops_t *const fatfs_dir_ops(void)  { return &g_fat_dir_ops; }

/* ---- mounting ---------------------------------------------------------- */

int fatfs_mount_bdev(const char *path, struct vfs_node *dev, uint64_t sectors)
{
    if (!path || !dev)
        return -E_INVAL;
    if (fatfs_attach(dev, sectors) < 0)
        return -E_NODEV;

    /* Invalidate the sector window before the first mount: a zeroed FATFS
     * object reads as "sector 0 already cached", and check_fs would then
     * inspect a zero-filled window instead of the boot sector. */
    g_fat.winsect = (LBA_t)-1;
    FRESULT r = f_mount(&g_fat, "", 1);   /* 1 = mount immediately */
    if (r != FR_OK) {
        fatfs_detach();
        dbg_puts("VFS: not a FAT volume (fr=");
        dbg_puts_dec((uint32_t)r);
        dbg_puts(")\r\n");
        return -E_INVAL;
    }
    g_fat_ok = 1;
    strncpy(g_fat_mnt, path, FAT_MOUNT_PATH_MAX - 1);
    g_fat_mnt[FAT_MOUNT_PATH_MAX - 1] = 0;
    return 0;
}

int fatfs_umount(const char *path)
{
    if (!g_fat_ok || strcmp(g_fat_mnt, path))
        return -E_INVAL;
    f_unmount("");
    fatfs_detach();
    g_fat_ok = 0;
    g_fat_mnt[0] = 0;
    return 0;
}

/* Does `abs` live under the FAT mount?  The mount point itself is included
 * and resolves as the volume root. */
int fatfs_route(const char *abs, char *rel)
{
    if (!g_fat_ok)
        return 0;
    size_t ml = strlen(g_fat_mnt);
    if (strncmp(abs, g_fat_mnt, ml))
        return 0;
    const char *rest = abs + ml;
    if (*rest == 0) {
        rel[0] = '/';
        rel[1] = 0;
        return 1;
    }
    if (*rest != '/')
        return 0;                        /* /mntx is not /mnt */
    size_t i = 0;
    for (; rest[i]; i++)
        rel[i] = rest[i];
    rel[i] = 0;
    return 1;
}

int fatfs_resolve(const char *rel, struct vfs_node *out)
{
    FILINFO fno;
    if (f_stat(rel, &fno) != FR_OK)
        return -E_NOENT;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, fno.fname, VFS_NAME_MAX - 1);
    out->name[VFS_NAME_MAX - 1] = 0;
    if (out->name[0] == 0)
        strncpy(out->name, "volume", VFS_NAME_MAX - 1);
    out->kind = (fno.fattrib & AM_DIR) ? VFS_DIR : VFS_FILE;
    out->size = fno.fsize;
    out->ops  = (out->kind == VFS_DIR) ? &g_fat_dir_ops : &g_fat_file_ops;
    if (node_set_rel(out, rel) < 0)
        return -E_NOMEM;
    return 0;
}

/* ---- management -------------------------------------------------------- */

int fatfs_unlink(const char *rel)
{
    FRESULT r = f_unlink(rel);
    return r == FR_OK ? 0 : -E_NOENT;
}

int fatfs_mkdir(const char *rel)
{
    FRESULT r = f_mkdir(rel);
    return r == FR_OK ? 0 : -E_EXIST;
}

int fatfs_rename(const char *from, const char *to)
{
    FRESULT r = f_rename(from, to);
    return r == FR_OK ? 0 : -E_NOENT;
}

int fatfs_statfs(uint64_t *total, uint64_t *free)
{
    if (!g_fat_ok)
        return -E_NODEV;
    FATFS *fs = &g_fat;
    DWORD nclst = 0;
    if (f_getfree("", &nclst, &fs) != FR_OK)
        return -E_IO;
    uint64_t per = (uint64_t)fs->csize * 512u;
    if (total) *total = (uint64_t)(fs->n_fatent - 2) * per;
    if (free)  *free  = (uint64_t)nclst * per;
    return 0;
}
