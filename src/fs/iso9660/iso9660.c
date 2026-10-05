/* SPDX-License-Identifier: GPL-2.0 */
/*
 * iso9660.c — read-only ISO9660 for CD-ROMs. (GPLv2)
 *
 * On-disk layout (2048-byte logical sectors, reference Linux fs/isofs):
 *   LBA 16       Primary Volume Descriptor ("\1CD001\1", one or more
 *                descriptors follow until the type-255 terminator)
 *   PVD+156      34-byte root directory record
 *   dir record:  [0] len  [2] extent LBA (LE/BE pair)  [10] size (LE/BE)
 *                [25] flags (bit 1 = directory)  [28] name_len @32 name[]
 *   Names carry ";1" version suffixes; directories have no extension.
 * Records never cross a sector boundary, which is what makes a whole
 * directory readable as one buffer.  Files are contiguous extents: a file
 * read is a device read at extent*LBA.
 */
#include <stdint.h>
#include <stddef.h>

#include "iso9660.h"
#include "heap.h"
#include "kstring.h"
#include "debugcon.h"
#include "vfs.h"

#define ISO_SECTOR 2048
#define ISO_VD_LBA 16

static vfs_node_t *g_iso_dev;     /* the CD-ROM block device          */
static uint64_t    g_iso_sectors; /* capacity, 2048-byte sectors      */
static int         g_iso_ok;
static char        g_iso_mnt[256];

/* One mounted file: where its extent starts and how long it is.  Both are
 * in ISO units (2048-byte sectors / bytes). */
typedef struct {
    uint64_t extent; /* first LBA of the data            */
    uint64_t size;   /* bytes                            */
    uint8_t  is_dir;
} iso_node_t;

static int iso_read_lba(uint64_t lba, void *buf)
{
    if (!g_iso_dev || !g_iso_dev->ops || !g_iso_dev->ops->read)
        return -1;
    int32_t n = g_iso_dev->ops->read(g_iso_dev, lba * ISO_SECTOR, buf, ISO_SECTOR);
    return n == ISO_SECTOR ? 0 : -1;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse one directory record at p (in a directory buffer of `dirsz`).
 * Fills out, returns the record length (0 = end of sector padding). */
static uint32_t iso_parse_rec(const uint8_t *p, const uint8_t *end, iso_node_t *out, char *name,
                              uint32_t namesz)
{
    if (p >= end)
        return 0;
    uint8_t len = p[0];
    if (len == 0)
        return 0; /* sector padding to the boundary */
    if (p + len > end)
        return 0;

    out->extent = rd32le(p + 2);
    out->size   = rd32le(p + 10);
    out->is_dir = (p[25] & 0x02) != 0;

    uint8_t nlen = p[32];
    if (name) {
        uint32_t k = 0;
        for (uint32_t i = 0; i < nlen && k < namesz - 1; i++) {
            /* strip the ";1" version suffix and any trailing dot */
            if (p[33 + i] == ';')
                break;
            name[k++] = p[33 + i];
        }
        if (k && name[k - 1] == '.')
            k--;
        name[k] = 0;
    }
    return len;
}

static int iso_read_dir(uint64_t extent, uint64_t size, uint8_t **bufp, uint64_t *bufsz)
{
    uint64_t nbytes = (size + ISO_SECTOR - 1) & ~(uint64_t)(ISO_SECTOR - 1);
    if (nbytes > 4u * 1024 * 1024)
        return -1; /* a directory is never this big */
    uint8_t *b = kmalloc((uint32_t)nbytes);
    if (!b)
        return -1;
    for (uint64_t s = 0; s < nbytes / ISO_SECTOR; s++)
        if (iso_read_lba(extent + s, b + s * ISO_SECTOR) < 0) {
            kfree(b);
            return -1;
        }
    *bufp  = b;
    *bufsz = nbytes;
    return 0;
}

/* Look up one path component inside a directory buffer. */
static int iso_dir_find(const uint8_t *buf, uint64_t bufsz, const char *name, iso_node_t *out)
{
    const uint8_t *p   = buf;
    const uint8_t *end = buf + bufsz;
    while (p < end) {
        iso_node_t rec;
        char       rname[96];
        uint32_t   len = iso_parse_rec(p, end, &rec, rname, sizeof rname);
        if (len == 0) {
            /* skip to the next sector: records do not cross boundaries */
            uint64_t off = (uint64_t)(p - buf);
            p            = buf + ((off + ISO_SECTOR) & ~(uint64_t)(ISO_SECTOR - 1));
            if (p >= end)
                break;
            continue;
        }
        if (rname[0] && strcmp(rname, name) == 0) {
            *out = rec;
            return 0;
        }
        p += len;
    }
    return -1;
}

/* ---- mounting ---------------------------------------------------------- */

int iso9660_mount_bdev(const char *path, struct vfs_node *dev, uint64_t sectors)
{
    if (!path || !dev)
        return -E_INVAL;
    g_iso_dev     = dev;
    g_iso_sectors = sectors;

    /* scan volume descriptors from LBA 16 for the PVD */
    uint8_t  vd[ISO_SECTOR];
    uint64_t root_extent = 0, root_size = 0;
    int      found = 0;
    for (uint64_t lba = ISO_VD_LBA; lba < ISO_VD_LBA + 32 && lba < sectors; lba++) {
        if (iso_read_lba(lba, vd) < 0)
            break;
        {
            /* TEMP: first PVD scan diagnostic */
            if (lba == ISO_VD_LBA) {
                dbg_puts("ISO: vd[0..7]:");
                for (int k = 0; k < 8; k++) {
                    dbg_puts(" ");
                    dbg_puts_hexn(vd[k], 2);
                }
                dbg_puts(" sig:");
                dbg_puts_hexn(vd[510], 2);
                dbg_puts_hexn(vd[511], 2);
                dbg_puts("\r\n");
            }
        }
        if (memcmp(vd + 1, "CD001", 5) != 0)
            break; /* not an ISO volume at all */
        if (vd[0] == 255)
            break;        /* terminator */
        if (vd[0] == 1) { /* primary volume descriptor */
            const uint8_t *root = vd + 156;
            root_extent         = rd32le(root + 2);
            root_size           = rd32le(root + 10);
            found               = 1;
        }
    }
    if (!found || !root_size) {
        dbg_puts("VFS: not an ISO9660 volume\\r\\n");
        g_iso_dev = NULL;
        return -E_INVAL;
    }

    /* the root record itself must read back sanely */
    iso_node_t rn;
    uint8_t   *rb;
    uint64_t   rsz;
    if (iso_read_dir(root_extent, root_size, &rb, &rsz) < 0) {
        g_iso_dev = NULL;
        return -E_IO;
    }
    kfree(rb);

    g_iso_ok = 1;
    strncpy(g_iso_mnt, path, sizeof g_iso_mnt - 1);
    g_iso_mnt[sizeof g_iso_mnt - 1] = 0;
    return 0;
}

int iso9660_umount(const char *path)
{
    if (!g_iso_ok || strcmp(g_iso_mnt, path))
        return -E_INVAL;
    g_iso_ok     = 0;
    g_iso_mnt[0] = 0;
    g_iso_dev    = NULL;
    return 0;
}

int iso9660_route(const char *abs, char *rel)
{
    if (!g_iso_ok)
        return 0;
    size_t ml = strlen(g_iso_mnt);
    if (strncmp(abs, g_iso_mnt, ml))
        return 0;
    const char *rest = abs + ml;
    if (*rest == 0) {
        rel[0] = '/';
        rel[1] = 0;
        return 1;
    }
    if (*rest != '/')
        return 0;
    size_t i = 0;
    for (; rest[i]; i++)
        rel[i] = rest[i];
    rel[i] = 0;
    return 1;
}

/* Walk `rel` ('/'-rooted) through the directory tree. */
int iso9660_resolve(const char *rel, struct vfs_node *out)
{
    uint64_t extent = 0, size = 0;
    uint8_t  is_dir = 1;

    if (strcmp(rel, "/") == 0) {
        /* root: re-read from the PVD's root record (kept simple: find the
         * PVD again, which is one sector read) */
        uint8_t vd[ISO_SECTOR];
        if (iso_read_lba(ISO_VD_LBA, vd) < 0 || memcmp(vd + 1, "CD001", 5))
            return -E_NOENT;
        const uint8_t *root = vd + 156;
        extent              = rd32le(root + 2);
        size                = rd32le(root + 10);
    } else {
        uint64_t dext = 0, dsz = 0;
        { /* root extent */
            uint8_t vd[ISO_SECTOR];
            if (iso_read_lba(ISO_VD_LBA, vd) < 0 || memcmp(vd + 1, "CD001", 5))
                return -E_NOENT;
            const uint8_t *root = vd + 156;
            dext                = rd32le(root + 2);
            dsz                 = rd32le(root + 10);
        }
        char        comp[96];
        const char *p = rel + 1; /* skip the leading '/' */
        while (*p) {
            uint32_t k = 0;
            while (*p && *p != '/' && k < sizeof comp - 1)
                comp[k++] = *p++;
            comp[k] = 0;
            if (*p == '/')
                p++;

            uint8_t *db;
            uint64_t dbsz;
            if (iso_read_dir(dext, dsz, &db, &dbsz) < 0)
                return -E_NOENT;
            iso_node_t rec;
            int        r = iso_dir_find(db, dbsz, comp, &rec);
            kfree(db);
            if (r < 0)
                return -E_NOENT;
            dext   = rec.extent;
            dsz    = rec.size;
            is_dir = rec.is_dir;
        }
        extent = dext;
        size   = is_dir ? dsz : dsz;
    }

    memset(out, 0, sizeof(*out));
    out->kind = is_dir ? VFS_DIR : VFS_FILE;
    out->size = size;
    out->ops  = is_dir ? iso9660_dir_ops() : iso9660_file_ops();

    iso_node_t *pr = kmalloc(sizeof *pr);
    if (!pr)
        return -E_NOMEM;
    pr->extent = extent;
    pr->size   = size;
    pr->is_dir = is_dir;
    out->priv  = pr;
    return 0;
}

/* ---- file and directory operations ------------------------------------- */

static void iso_release(struct vfs_node *n)
{
    if (n && n->priv) {
        kfree(n->priv);
        n->priv = NULL;
    }
}

static int32_t iso_file_read(struct vfs_node *n, uint64_t off, void *buf, uint32_t len)
{
    iso_node_t *f = n->priv;
    if (!f)
        return -E_INVAL;
    if (off >= f->size || !len)
        return 0;
    if (off + len > f->size)
        len = (uint32_t)(f->size - off);

    /* the extent is contiguous; reads must stay sector-aligned at the
     * device, so round down/up and copy the middle directly */
    uint8_t *mid   = buf;
    uint64_t lba   = f->extent + off / ISO_SECTOR;
    uint32_t inoff = (uint32_t)(off % ISO_SECTOR);

    static uint8_t sec[ISO_SECTOR];
    uint32_t       done = 0;

    /* head: the partial first sector */
    if (inoff) {
        if (iso_read_lba(lba, sec) < 0)
            return -E_IO;
        uint32_t n = ISO_SECTOR - inoff;
        if (n > len)
            n = len;
        memcpy(buf, sec + inoff, n);
        done += n;
        len -= n;
        lba++;
    }
    /* middle: whole sectors straight into the caller's buffer */
    while (len >= ISO_SECTOR) {
        if (iso_read_lba(lba, mid + done) < 0)
            return done ? (int32_t)done : -E_IO;
        done += ISO_SECTOR;
        len -= ISO_SECTOR;
        lba++;
    }
    /* tail */
    if (len) {
        if (iso_read_lba(lba, sec) < 0)
            return done ? (int32_t)done : -E_IO;
        memcpy(mid + done, sec, len);
        done += len;
    }
    (void)mid;
    return (int32_t)done;
}

static int32_t iso_dir_read(struct vfs_node *n, uint64_t off, void *buf, uint32_t len)
{
    iso_node_t    *d   = n->priv;
    const uint32_t rec = (uint32_t)sizeof(gdirent_t);
    if (!d || !d->is_dir || off % rec || len < rec)
        return -E_INVAL;

    uint32_t skip = (uint32_t)(off / rec);
    uint32_t room = len / rec;

    uint8_t *db;
    uint64_t dbsz;
    if (iso_read_dir(d->extent, d->size, &db, &dbsz) < 0)
        return -E_NOENT;

    gdirent_t     *out  = (gdirent_t *)buf;
    uint32_t       seen = 0, got = 0;
    const uint8_t *p   = db;
    const uint8_t *end = db + d->size;
    while (got < room && p < end) {
        iso_node_t recn;
        char       rname[96];
        uint32_t   rlen = iso_parse_rec(p, end, &recn, rname, sizeof rname);
        if (rlen == 0) {
            p = db + (((p - db) / ISO_SECTOR) + 1) * ISO_SECTOR;
            if (p >= end)
                break;
            continue;
        }
        p += rlen;
        if (rname[0] == 0)
            continue; /* "." and ".." have empty names */
        if (seen++ < skip)
            continue;
        memset(&out[got], 0, rec);
        strncpy(out[got].name, rname, GDIRENT_NAME - 1);
        out[got].kind = recn.is_dir ? GK_DIR : GK_FILE;
        out[got].size = (uint32_t)recn.size;
        got++;
    }
    kfree(db);
    return (int32_t)(got * rec);
}

static const vfs_ops_t g_iso_file_ops = {.read = iso_file_read, .release = iso_release};
static const vfs_ops_t g_iso_dir_ops  = {.read = iso_dir_read, .release = iso_release};
const vfs_ops_t *const iso9660_file_ops(void)
{
    return &g_iso_file_ops;
}
const vfs_ops_t *const iso9660_dir_ops(void)
{
    return &g_iso_dir_ops;
}

int iso9660_statfs(uint64_t *total, uint64_t *free)
{
    if (!g_iso_ok)
        return -E_NODEV;
    if (total)
        *total = g_iso_sectors * ISO_SECTOR;
    if (free)
        *free = 0; /* read-only media */
    return 0;
}
