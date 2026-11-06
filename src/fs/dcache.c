/* SPDX-License-Identifier: GPL-2.0 */
/* dcache.c — the dentry cache: path → resolved VFS node. (GPLv2)
 *
 * See dcache.h for the design.  Implementation notes:
 *
 *  - Open hashing over 512 buckets; the bucket index is FNV-1a over the
 *    path with the follow flag folded in, so a symlink's followed and
 *    unfollowed resolutions live side by side without colliding on key.
 *  - Collisions replace the oldest entry (a full bucket means the working
 *    set is bigger than the cache; eviction policy barely matters).
 *  - Entries are refilled lazily: a miss just re-runs resolve().
 *  - The generation counter makes flush O(1); entries compare their
 *    generation on lookup and are reclaimed on touch.
 */
#include <stddef.h>
#include <stdint.h>

#include "dcache.h"
#include "debugcon.h"
#include "heap.h"
#include "kstring.h"
#include "smp.h"

#define DC_BUCKETS 512
#define DC_MAX     1024 /* total entries; keeps kmalloc pressure bounded */

typedef struct dentry {
    struct dentry *next; /* bucket chain */
    uint64_t       gen;  /* generation this entry was filled at */
    uint32_t       hash; /* full hash, for cheap mismatch rejection */
    uint8_t        follow;
    vfs_node_t     node;
    char           path[]; /* NUL-terminated, flexible array */
} dentry_t;

static dentry_t  *g_buckets[DC_BUCKETS];
static uint64_t   g_generation = 1; /* 0 means "no entry is valid" */
static uint32_t   g_count;
static spinlock_t g_dcache_lock;

static uint32_t path_hash(const char *path, int follow)
{
    uint32_t h = 2166136261u ^ (uint32_t)follow;
    while (*path) {
        h ^= (uint8_t)*path++;
        h *= 16777619u;
    }
    return h;
}

/* Only static trees may be cached: the pseudo-filesystems regenerate their
 * nodes (and tmpfs mutates outside this file), so their results must be
 * re-resolved every time. */
static const char *const g_dynamic_prefixes[] = {
    "/proc", "/sys", "/dev", "/tmp", "/run", NULL,
};

int dcache_cacheable(const char *path)
{
    if (!path || path[0] != '/')
        return 0;
    for (int i = 0; g_dynamic_prefixes[i]; i++) {
        const char *p = g_dynamic_prefixes[i];
        if (path[0] == p[0] && strncmp(path, p, strlen(p)) == 0 &&
            (path[strlen(p)] == '/' || path[strlen(p)] == '\0'))
            return 0;
    }
    return 1;
}

int dcache_lookup(const char *path, int follow, vfs_node_t *out)
{
    if (!dcache_cacheable(path))
        return 0;

    uint32_t h   = path_hash(path, follow);
    uint32_t b   = h & (DC_BUCKETS - 1);
    int      hit = 0;

    spin_lock_irq(&g_dcache_lock);
    dentry_t **pp = &g_buckets[b];
    while (*pp) {
        dentry_t *d = *pp;
        if (d->hash == h && d->follow == (uint8_t)follow && d->gen == g_generation &&
            strcmp(d->path, path) == 0) {
            *out = d->node;
            hit  = 1;
            break;
        }
        /* Stale or mismatched: reclaim while we are here. */
        if (d->gen != g_generation) {
            *pp = d->next;
            kfree(d);
            g_count--;
            continue;
        }
        pp = &d->next;
    }
    spin_unlock_irq(&g_dcache_lock);
    return hit;
}

void dcache_fill(const char *path, int follow, const vfs_node_t *out)
{
    if (!dcache_cacheable(path) || !out || !out->ops)
        return;
    if (g_count >= DC_MAX)
        dcache_flush(); /* cheaper than tracking an LRU order */

    size_t    len = strlen(path) + 1;
    dentry_t *d   = kmalloc(sizeof(*d) + len);
    if (!d)
        return;

    uint32_t h = path_hash(path, follow);
    d->gen     = g_generation;
    d->hash    = h;
    d->follow  = (uint8_t)follow;
    d->node    = *out;
    memcpy(d->path, path, len);

    spin_lock_irq(&g_dcache_lock);
    uint32_t b = h & (DC_BUCKETS - 1);
    /* Replace the chain head: buckets hold at most a handful of entries in
     * practice, and head replacement needs no traversal. */
    dentry_t *old = g_buckets[b];
    if (old)
        g_count--;
    d->next      = old ? old->next : NULL;
    g_buckets[b] = d;
    g_count++;
    spin_unlock_irq(&g_dcache_lock);
    kfree(old);
}

void dcache_flush(void)
{
    spin_lock_irq(&g_dcache_lock);
    g_generation++;
    spin_unlock_irq(&g_dcache_lock);
    /* Stale entries are freed lazily on lookup, so a flush never walks the
     * table -- mutation paths stay O(1). */
}

void dcache_selftest(void)
{
    vfs_node_t n;
    memset(&n, 0, sizeof(n));
    strncpy(n.name, "selftest", VFS_NAME_MAX - 1);
    n.kind = VFS_FILE;
    n.ops  = (const vfs_ops_t *)(uintptr_t)1; /* sentinel, never dereferenced */

    dcache_flush();
    if (dcache_lookup("/selftest-probe", 0, &n))
        goto fail; /* nothing cached yet */
    dcache_fill("/selftest-probe", 0, &n);
    vfs_node_t got;
    if (!dcache_lookup("/selftest-probe", 0, &got) || got.kind != VFS_FILE)
        goto fail;
    if (dcache_lookup("/selftest-probe", 1, &got))
        goto fail; /* different key: follow flag must not collide */
    dcache_flush();
    if (dcache_lookup("/selftest-probe", 0, &got))
        goto fail; /* generation bump invalidated it */

    dbg_puts("DCACHE: self-test PASS (");
    dbg_puts_dec((uint32_t)g_count);
    dbg_puts(" entries live)\r\n");
    return;
fail:
    dbg_puts("DCACHE: self-test FAIL\r\n");
}
