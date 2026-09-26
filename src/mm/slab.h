/* SPDX-License-Identifier: GPL-2.0 */
/*
 * slab.h — kmem_cache: fixed-size object caches over the kernel heap.
 * (GPLv2)
 *
 * The boundary-tag allocator in heap.c is general but slow: every small
 * allocation walks a free list and every free coalesces neighbours.  The
 * kernel instead allocates the same handful of structures over and over --
 * process objects, VFS nodes, anonymous-fd nodes, DRM GEM objects -- and
 * for those Jeff Bonwick's slab allocator is the right machine:
 *
 *   - a kmem_cache_t hands out objects of one size from pre-constructed
 *     slabs, so allocation is a pointer pop, not a list search;
 *   - each CPU keeps a small array cache of hot objects (alloc pops it,
 *     free pushes it), so the common case never touches the slab lists;
 *   - every slab colours its object area by a different offset within a
 *     64-byte cache line, which spreads the sets that different fields of
 *     successive objects map to and measurably cuts L1 conflicts.
 */
#ifndef GNUCOS_SLAB_H
#define GNUCOS_SLAB_H

#include <stdint.h>

typedef struct kmem_cache kmem_cache_t;

/* Creation flags. */
#define SLAB_HWCACHE_ALIGN  0x01   /* round the object size to a cache line */
#define SLAB_PANIC          0x02   /* panic instead of returning NULL       */
#define SLAB_ZEROED         0x04   /* memory handed out pre-zeroed          */

/* Optional constructor: called once per object when a slab is carved. */
typedef void (*kmem_ctor_t)(void *obj, kmem_cache_t *cache);

/*
 * Create a cache of `size`-byte objects named `name`.  `align` may be 0 for
 * the default (16-byte, or a full cache line with SLAB_HWCACHE_ALIGN).
 * Returns NULL on failure unless SLAB_PANIC.
 */
kmem_cache_t *kmem_cache_create(const char *name, uint32_t size, uint32_t align,
                                uint32_t flags, kmem_ctor_t ctor);

/* Destroy a cache: every object must have been handed back first. */
void kmem_cache_destroy(kmem_cache_t *cache);

void *kmem_cache_alloc(kmem_cache_t *cache);
void  kmem_cache_free(kmem_cache_t *cache, void *obj);
/* alloc + memset(0): only meaningful for caches without a constructor. */
void *kmem_cache_zalloc(kmem_cache_t *cache);

/* /proc/slabinfo support: iterate every live cache. */
typedef struct {
    const char *name;
    uint32_t    obj_size, objs_per_slab;
    uint32_t    active_objs, num_objs;
    uint32_t    num_slabs, num_full, num_partial;
    uint32_t    ac_hits, slab_hits;
} kmem_slabinfo_t;

int   kmem_slabinfo_next(int *iter, kmem_slabinfo_t *out);
void  slab_selftest(void);

/* The one lock the slab layer takes; heap.c owns the region underneath. */
#include "heap.h"

#endif
