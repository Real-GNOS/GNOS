/* dcache.h — the dentry cache: path → resolved VFS node. (GPLv2)
 *
 * resolve() routes a path through every mounted filesystem on every
 * syscall that names one; the root image alone walks directory blocks per
 * component.  The cache stores the positive result keyed by (path,
 * follow); any namespace or size mutation bumps a generation counter and
 * the whole cache goes stale at once -- coarse, but always correct, and
 * mutations are rare next to lookups.
 *
 * Only static trees are cached: paths under the dynamic pseudo-filesystems
 * (/proc, /sys, /dev, /tmp, /run) are refused by dcache_cacheable(), so
 * generated nodes and tmpfs state never read stale.
 */
#ifndef GNUCOS_DCACHE_H
#define GNUCOS_DCACHE_H

#include <stdint.h>
#include "vfs.h"

/* Non-zero when this path may be cached (static tree only). */
int dcache_cacheable(const char *path);

/* Look up a cached resolution.  Returns 1 and fills *out on a hit, 0 on a
 * miss or when the entry predates the current generation. */
int dcache_lookup(const char *path, int follow, vfs_node_t *out);

/* Store a successful resolution.  Refused silently for uncacheable paths
 * or when the table is under memory pressure. */
void dcache_fill(const char *path, int follow, const vfs_node_t *out);

/* Invalidate everything (bump the generation).  Call after any namespace
 * mutation or file-size change. */
void dcache_flush(void);

/* Boot-time self-test. */
void dcache_selftest(void);

#endif /* GNUCOS_DCACHE_H */
