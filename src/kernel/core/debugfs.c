/*
 * debugfs.c — /debug, generated on read. (GPLv2)
 *
 * A developer-facing pseudo-filesystem, modelled after Linux's debugfs.
 * Every file is rendered on demand from live kernel state; nothing is
 * stored.  The set is small and aimed at the people building the kernel,
 * not at user space: memory statistics, SMP topology, timer state, and
 * the subsystem registry.
 */
#include <stdint.h>

#include "debugfs.h"
#include "sysnum.h"
#include "kstring.h"
#include "pmm.h"
#include "timer.h"
#include "smp.h"
#include "subsys.h"
#include "vfs.h"

/* One render never exceeds this; the largest file is the subsystem table. */
#define DEBUGFS_BUF 2048

/* ---- a tiny append-only string builder -------------------------------- */
typedef debugfs_sbuf_t dsbuf_t;

static void ds_char(dsbuf_t *s, char c)
{
    if (s->pos < s->cap)
        s->buf[s->pos] = c;
    s->pos++;
}

static void ds_str(dsbuf_t *s, const char *str)
{
    while (*str)
        ds_char(s, *str++);
}

static void ds_dec(dsbuf_t *s, uint64_t v, int width)
{
    char tmp[24];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (int i = n; i < width; i++)
        ds_char(s, ' ');
    while (n--)
        ds_char(s, tmp[n]);
}

/* ---- the generators ---------------------------------------------------- */
static void gen_version(dsbuf_t *s)
{
    ds_str(s, "GNOS debugfs\n");
    ds_str(s, "Kernel: GNOSKr.elf (x86_64)\n");
}

static void gen_pmm(dsbuf_t *s)
{
    uint64_t total = pmm_total_frames();
    uint64_t free  = pmm_free_frames();
    uint64_t used  = total - free;

    ds_str(s, "Physical Memory:\n");
    ds_str(s, "  Total:     ");
    ds_dec(s, total * 4, 0);
    ds_str(s, " kB\n");
    ds_str(s, "  Free:      ");
    ds_dec(s, free * 4, 0);
    ds_str(s, " kB\n");
    ds_str(s, "  Used:      ");
    ds_dec(s, used * 4, 0);
    ds_str(s, " kB\n");
    ds_str(s, "  Frames:    ");
    ds_dec(s, total, 0);
    ds_str(s, " total, ");
    ds_dec(s, free, 0);
    ds_str(s, " free\n");
}

static void gen_timer(dsbuf_t *s)
{
    uint64_t ticks = timer_ticks();

    ds_str(s, "Timer:\n");
    ds_str(s, "  Ticks:     ");
    ds_dec(s, ticks, 0);
    ds_str(s, "\n");
    ds_str(s, "  Uptime:    ");
    ds_dec(s, ticks / 100, 0);
    ds_char(s, '.');
    ds_dec(s, (ticks % 100) / 10, 0);
    ds_dec(s, ticks % 10, 0);
    ds_str(s, " seconds\n");
    ds_str(s, "  Frequency: 100 Hz\n");
}

static void gen_smp(dsbuf_t *s)
{
    ds_str(s, "SMP:\n");
    ds_str(s, "  CPUs online: ");
    ds_dec(s, smp_online_count(), 0);
    ds_str(s, "\n");
}

static void gen_subsystems(dsbuf_t *s)
{
    ds_str(s, "Registered subsystems:\n");
    ds_str(s, "  Name               Class        State      Dev\n");
    ds_str(s, "  ----               -----        -----      ---\n");
    for (int i = 0; i < subsys_count(); i++) {
        const subsys_t *d = subsys_get(i);
        ds_char(s, ' ');
        /* Pad name to 19 chars */
        int len = 0;
        for (const char *p = d->name; *p; p++) len++;
        ds_str(s, d->name);
        for (int j = len; j < 19; j++) ds_char(s, ' ');

        ds_str(s, subsys_class_name(d->cls));
        int clen = 0;
        for (const char *p = subsys_class_name(d->cls); *p; p++) clen++;
        for (int j = clen; j < 12; j++) ds_char(s, ' ');

        ds_str(s, d->state == SUBSYS_STATE_LIVE   ? "live"
                : d->state == SUBSYS_STATE_FAILED ? "failed"
                                                  : "registered");
        ds_char(s, ' ');
        ds_str(s, d->dev[0] ? d->dev : "-");
        ds_char(s, '\n');
    }
}

/* ---- the file table ---------------------------------------------------- */
typedef debugfs_gen_t debugfs_gen_fn;

typedef struct {
    const char   *name;   /* basename only, e.g. "pmm" */
    debugfs_gen_fn gen;
} debugfs_file_t;

static const debugfs_file_t g_files[] = {
    { "version",      gen_version     },
    { "pmm",          gen_pmm         },
    { "timer",        gen_timer       },
    { "smp",          gen_smp         },
    { "subsystems",   gen_subsystems  },
};
#define NFILES ((int)(sizeof(g_files) / sizeof(g_files[0])))

/* ---- blobs (static data registered at boot) ---------------------------- */
#define MAX_BLOBS 8

typedef struct {
    const char     *name;
    const uint8_t  *data;
    uint32_t        size;
} debugfs_blob_t;

static debugfs_blob_t g_blobs[MAX_BLOBS];
static int            g_nblobs;

int debugfs_add_blob(const char *name, const void *data, uint32_t size)
{
    if (!name || !data || size == 0)
        return -E_INVAL;
    if (g_nblobs >= MAX_BLOBS)
        return -E_NOSPC;

    debugfs_blob_t *b = &g_blobs[g_nblobs];
    b->name = name;
    b->data = (const uint8_t *)data;
    b->size = size;
    g_nblobs++;
    return 0;
}

/* ---- dynamic files (registered at boot) -------------------------------- */
#define MAX_FILES (NFILES + 8)   /* static table + room for driver additions */

static debugfs_file_t g_dyn[MAX_FILES - NFILES];
static int            g_ndyn;

int debugfs_add_file(const char *name, debugfs_gen_t gen)
{
    if (!name || !gen)
        return -E_INVAL;
    if (g_ndyn >= MAX_FILES - NFILES)
        return -E_NOSPC;

    g_dyn[g_ndyn].name = name;
    g_dyn[g_ndyn].gen  = gen;
    g_ndyn++;
    return 0;
}

/* ---- init -------------------------------------------------------------- */
void debugfs_init(void)
{
    /* nothing to do -- the tables are statically initialised */
}

/* ---- read -------------------------------------------------------------- */
static int32_t debugfs_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    static char scratch[DEBUGFS_BUF];

    const debugfs_file_t *f = (const debugfs_file_t *)n->priv;
    if (!f)
        return -E_INVAL;

    dsbuf_t s = { scratch, DEBUGFS_BUF, 0 };
    f->gen(&s);

    uint32_t size = (s.pos < DEBUGFS_BUF) ? s.pos : DEBUGFS_BUF;
    if (off >= size)
        return 0;

    uint32_t avail = size - (uint32_t)off;
    if (len > avail)
        len = avail;
    memcpy(buf, scratch + off, len);
    return (int32_t)len;
}

static const vfs_ops_t g_debugfs_ops = { .read = debugfs_read, .write = NULL };

/* ---- blob read --------------------------------------------------------- */
static int32_t blob_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    const debugfs_blob_t *b = (const debugfs_blob_t *)n->priv;
    if (!b)
        return -E_INVAL;
    if (off >= b->size)
        return 0;
    uint32_t avail = b->size - (uint32_t)off;
    if (len > avail)
        len = avail;
    memcpy(buf, b->data + off, len);
    return (int32_t)len;
}

static const vfs_ops_t g_blob_ops = { .read = blob_read, .write = NULL };

/* ---- directory ops (read returns EISDIR) -------------------------------- */
static int32_t debugdir_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_ISDIR;
}

static const vfs_ops_t g_debugdir_ops = { .read = debugdir_read, .write = NULL };

/* ---- basename ---------------------------------------------------------- */
static const char *basename_of(const char *path)
{
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            slash = p + 1;
    return slash;
}

/* ---- lookup ------------------------------------------------------------ */
int debugfs_resolve(const char *path, vfs_node_t *out)
{
    if (!path || strncmp(path, "/debug", 6) != 0)
        return -E_INVAL;
    if (path[6] != '\0' && path[6] != '/')
        return -E_INVAL;

    /* /debug itself */
    if (path[6] == '\0') {
        memset(out, 0, sizeof(*out));
        strncpy(out->name, "debug", VFS_NAME_MAX - 1);
        out->kind = VFS_DIR;
        out->ops  = &g_debugdir_ops;
        return 0;
    }

    /* Only direct children: /debug/<name> */
    const char *base = basename_of(path);
    if (base == path + 1)   /* no '/' before name */
        return -E_NOENT;

    /* Must be exactly one component below /debug (no subdirs) */
    for (const char *p = base; *p; p++)
        if (*p == '/')
            return -E_NOENT;

    /* Check static file table */
    for (int i = 0; i < NFILES; i++) {
        if (strcmp(base, g_files[i].name) == 0) {
            memset(out, 0, sizeof(*out));
            strncpy(out->name, g_files[i].name, VFS_NAME_MAX - 1);
            out->kind = VFS_FILE;
            out->ops  = &g_debugfs_ops;
            out->priv = (void *)&g_files[i];
            return 0;
        }
    }

    /* Check dynamic files */
    for (int i = 0; i < g_ndyn; i++) {
        if (strcmp(base, g_dyn[i].name) == 0) {
            memset(out, 0, sizeof(*out));
            strncpy(out->name, g_dyn[i].name, VFS_NAME_MAX - 1);
            out->kind = VFS_FILE;
            out->ops  = &g_debugfs_ops;
            out->priv = (void *)&g_dyn[i];
            return 0;
        }
    }

    /* Check blobs */
    for (int i = 0; i < g_nblobs; i++) {
        if (strcmp(base, g_blobs[i].name) == 0) {
            memset(out, 0, sizeof(*out));
            strncpy(out->name, g_blobs[i].name, VFS_NAME_MAX - 1);
            out->kind = VFS_FILE;
            out->ops  = &g_blob_ops;
            out->priv = (void *)&g_blobs[i];
            out->size = g_blobs[i].size;
            return 0;
        }
    }

    return -E_NOENT;
}

/* ---- readdir ------------------------------------------------------------ */
int debugfs_readdir(const char *dirpath, uint32_t index, char *name,
                    uint8_t *type)
{
    if (strcmp(dirpath, "/debug") != 0)
        return -E_NOTDIR;

    uint32_t n = 0;
    if (index == n++) {
        strncpy(name, ".", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }
    if (index == n++) {
        strncpy(name, "..", VFS_NAME_MAX - 1);
        *type = DT_DIR;
        return 0;
    }

    /* Static files */
    for (int i = 0; i < NFILES; i++) {
        if (index == n++) {
            strncpy(name, g_files[i].name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }

    /* Dynamic files */
    for (int i = 0; i < g_ndyn; i++) {
        if (index == n++) {
            strncpy(name, g_dyn[i].name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }

    /* Blobs */
    for (int i = 0; i < g_nblobs; i++) {
        if (index == n++) {
            strncpy(name, g_blobs[i].name, VFS_NAME_MAX - 1);
            *type = DT_REG;
            return 0;
        }
    }

    return -E_NOENT;
}
