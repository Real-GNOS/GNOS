/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysfs.c — /sys, the kernel's exported object hierarchy. (GPLv2)
 *
 * Modelled after Linux sysfs the way debugfs mirrors debugfs: a path-prefix
 * pseudo-filesystem where files are generated on demand and directories
 * exist as the path components of registered attributes.  The tree is small
 * and aimed at what user space actually asks for:
 *
 *   /sys/kernel/ostype            "GNOS"
 *   /sys/kernel/osrelease         kernel release string
 *   /sys/kernel/uptime            seconds since boot, rendered live
 *   /sys/class/drm/card0/status   "enabled"/"disabled" (registered by DRM)
 *   /sys/class/drm/card0/enabled  same, udev's favourite question
 *
 * Attributes may accept writes through an optional put handler; the value
 * is delivered as the raw buffer the user wrote.
 */
#include <stdint.h>

#include "sysfs.h"
#include "kstring.h"
#include "timer.h"
#include "vfs.h"

#define SYSFS_MAX_FILES 32
#define SYSFS_BUF       512
#define SYSFS_MAX_DEPTH 4          /* slashes in relpath: a/b/c/d */

typedef struct {
    char       rel[96];            /* path below /sys, e.g. "kernel/ostype" */
    sysfs_gen_t gen;               /* renders content; never NULL */
    sysfs_put_t put;               /* optional write handler */
} sysfs_ent_t;

static sysfs_ent_t g_ents[SYSFS_MAX_FILES];
static int         g_nents;

/* ---- small renderers ----------------------------------------------------- */

static void put_str(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    while (*s && *len + 1 < cap)
        buf[(*len)++] = *s++;
    if (*len < cap)
        buf[*len] = '\0';
}

static void put_dec(char *buf, uint32_t cap, uint32_t *len, uint64_t v)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n && *len + 1 < cap)
        buf[(*len)++] = tmp[--n];
    if (*len < cap)
        buf[*len] = '\0';
}

/* ---- built-in generators ------------------------------------------------ */

static void gen_ostype(char *buf, uint32_t cap, uint32_t *len)
{
    put_str(buf, cap, len, "GNOS\n");
}

static void gen_osrelease(char *buf, uint32_t cap, uint32_t *len)
{
    put_str(buf, cap, len, "0.1\n");
}

static void gen_uptime(char *buf, uint32_t cap, uint32_t *len)
{
    put_dec(buf, cap, len, timer_ticks() / SCHED_HZ);
    put_str(buf, cap, len, "\n");
}

/* ---- registration ------------------------------------------------------- */

static int depth_of(const char *s)
{
    int d = 0;
    for (; *s; s++)
        if (*s == '/')
            d++;
    return d;
}

int sysfs_add_file(const char *relpath, sysfs_gen_t gen, sysfs_put_t put)
{
    if (!relpath || !gen)
        return -E_INVAL;
    size_t rlen = strlen(relpath);
    if (relpath[0] == '/' || rlen == 0 || rlen >= sizeof(g_ents[0].rel))
        return -E_INVAL;
    if (depth_of(relpath) > SYSFS_MAX_DEPTH)
        return -E_INVAL;
    for (int i = 0; i < g_nents; i++)
        if (strcmp(g_ents[i].rel, relpath) == 0)
            return -E_EXIST;

    if (g_nents >= SYSFS_MAX_FILES)
        return -E_NOSPC;

    sysfs_ent_t *e = &g_ents[g_nents++];
    strncpy(e->rel, relpath, sizeof(e->rel) - 1);
    e->rel[sizeof(e->rel) - 1] = '\0';
    e->gen = gen;
    e->put = put;
    return 0;
}

void sysfs_init(void)
{
    sysfs_add_file("kernel/ostype", gen_ostype, NULL);
    sysfs_add_file("kernel/osrelease", gen_osrelease, NULL);
    sysfs_add_file("kernel/uptime", gen_uptime, NULL);
}

/* ---- helpers ------------------------------------------------------------ */

static sysfs_ent_t *ent_for(const char *relpath)
{
    for (int i = 0; i < g_nents; i++)
        if (strcmp(g_ents[i].rel, relpath) == 0)
            return &g_ents[i];
    return NULL;
}

/* Is `relpath` a directory, i.e. the prefix of at least one registered
 * attribute ("kernel" from "kernel/ostype")? */
static int is_dir(const char *relpath)
{
    size_t n = strlen(relpath);
    for (int i = 0; i < g_nents; i++)
        if (strncmp(g_ents[i].rel, relpath, n) == 0 &&
            g_ents[i].rel[n] == '/')
            return 1;
    return 0;
}


/* ---- read / write ------------------------------------------------------- */

static int32_t sysfs_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    static char scratch[SYSFS_BUF];

    sysfs_ent_t *e = (sysfs_ent_t *)n->priv;
    if (!e || !e->gen)
        return -E_INVAL;

    uint32_t used = 0;
    e->gen(scratch, SYSFS_BUF, &used);
    if (used > SYSFS_BUF)
        used = SYSFS_BUF;
    if (off >= used)
        return 0;

    uint32_t avail = used - (uint32_t)off;
    if (len > avail)
        len = avail;
    memcpy(buf, scratch + off, len);
    return (int32_t)len;
}

static int32_t sysfs_write(vfs_node_t *n, uint64_t off, const void *buf,
                           uint32_t len)
{
    sysfs_ent_t *e = (sysfs_ent_t *)n->priv;
    if (!e)
        return -E_INVAL;
    if (!e->put)
        return -E_ROFS;
    if (off != 0)
        return -E_INVAL;
    return e->put((const char *)buf, len);
}

static const vfs_ops_t g_sysfs_ops = { .read = sysfs_read, .write = sysfs_write };

static int32_t sysdir_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_sysdir_ops = { .read = sysdir_read, .write = NULL };

/* Fill in a directory node named by the last component of `relpath`. */
static void fill_dir(vfs_node_t *out, const char *relpath)
{
    memset(out, 0, sizeof(*out));
    const char *base = relpath;
    for (const char *p = relpath; *p; p++)
        if (*p == '/')
            base = p + 1;
    strncpy(out->name, base, VFS_NAME_MAX - 1);
    out->name[VFS_NAME_MAX - 1] = '\0';
    out->kind = VFS_DIR;
    out->ops  = &g_sysdir_ops;
}

static void fill_file(vfs_node_t *out, const char *relpath, sysfs_ent_t *e)
{
    memset(out, 0, sizeof(*out));
    const char *base = relpath;
    for (const char *p = relpath; *p; p++)
        if (*p == '/')
            base = p + 1;
    strncpy(out->name, base, VFS_NAME_MAX - 1);
    out->name[VFS_NAME_MAX - 1] = '\0';
    out->kind = VFS_FILE;
    out->ops  = &g_sysfs_ops;
    out->priv = e;
}

/* ---- resolve ------------------------------------------------------------ */

int sysfs_resolve(const char *path, vfs_node_t *out)
{
    if (!path || strncmp(path, "/sys", 4) != 0)
        return -E_INVAL;
    if (path[4] != '\0' && path[4] != '/')
        return -E_INVAL;

    /* /sys itself */
    if (path[4] == '\0') {
        memset(out, 0, sizeof(*out));
        strncpy(out->name, "sys", VFS_NAME_MAX - 1);
        out->kind = VFS_DIR;
        out->ops  = &g_sysdir_ops;
        return 0;
    }

    const char *rel = path + 5;                 /* skip "/sys/" */
    if (rel[0] == '\0')
        return -E_NOENT;

    /* leaf? */
    sysfs_ent_t *e = ent_for(rel);
    if (e) {
        fill_file(out, rel, e);
        return 0;
    }

    /* directory? any registered attribute below it */
    if (is_dir(rel)) {
        fill_dir(out, rel);
        return 0;
    }

    return -E_NOENT;
}

/* ---- readdir ------------------------------------------------------------ */

int sysfs_readdir(const char *dirpath, uint32_t index, char *name,
                  uint8_t *type)
{
    uint32_t n = 0;

    if (strcmp(dirpath, "/sys") == 0) {
        if (index == n++) { strncpy(name, ".", VFS_NAME_MAX - 1);     *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "..", VFS_NAME_MAX - 1);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "kernel", VFS_NAME_MAX);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "class", VFS_NAME_MAX);     *type = DT_DIR; return 0; }
        return -E_NOENT;
    }

    if (strcmp(dirpath, "/sys/kernel") == 0) {
        if (index == n++) { strncpy(name, ".", VFS_NAME_MAX - 1);     *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "..", VFS_NAME_MAX - 1);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "ostype", VFS_NAME_MAX);    *type = DT_REG; return 0; }
        if (index == n++) { strncpy(name, "osrelease", VFS_NAME_MAX); *type = DT_REG; return 0; }
        if (index == n++) { strncpy(name, "uptime", VFS_NAME_MAX);    *type = DT_REG; return 0; }
        return -E_NOENT;
    }

    if (strcmp(dirpath, "/sys/class") == 0) {
        if (index == n++) { strncpy(name, ".", VFS_NAME_MAX - 1);     *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "..", VFS_NAME_MAX - 1);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "drm", VFS_NAME_MAX);       *type = DT_DIR; return 0; }
        return -E_NOENT;
    }

    if (strcmp(dirpath, "/sys/class/drm") == 0) {
        if (index == n++) { strncpy(name, ".", VFS_NAME_MAX - 1);     *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "..", VFS_NAME_MAX - 1);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "card0", VFS_NAME_MAX);     *type = DT_DIR; return 0; }
        return -E_NOENT;
    }

    if (strcmp(dirpath, "/sys/class/drm/card0") == 0) {
        if (index == n++) { strncpy(name, ".", VFS_NAME_MAX - 1);     *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "..", VFS_NAME_MAX - 1);    *type = DT_DIR; return 0; }
        if (index == n++) { strncpy(name, "status", VFS_NAME_MAX);    *type = DT_REG; return 0; }
        if (index == n++) { strncpy(name, "enabled", VFS_NAME_MAX);   *type = DT_REG; return 0; }
        return -E_NOENT;
    }

    return -E_NOTDIR;
}
