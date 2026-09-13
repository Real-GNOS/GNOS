/*
 * drm_mm.h - ranges carved out of a fixed span, for very small spans. (GPLv2)
 *
 * The smallest honest description of what this does is "malloc over someone
 * else's memory": given a span of addresses (VRAM, an aperture), hand out
 * and take back non-overlapping pieces of it, aligned as asked.  Users are
 * the DRM memory managers: GEM buffers today, scanout reservations when a
 * mode needs them.
 *
 * Allocated nodes live in a tree ordered by start address, so finding room
 * is a walk over the gaps between consecutive nodes -- linear in the number
 * of allocations rather than in the size of the span, which is the right
 * trade when there are tens of them.
 */

#ifndef INCLUDE_DRM_DRM_MM_H_
#define INCLUDE_DRM_DRM_MM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rbtree.h"
#include "smp.h"

/* Where insert looks for a gap. */
enum drm_mm_insert_mode {
    DRM_MM_INSERT_DEFAULT = 0, /* lowest gap that fits */
    DRM_MM_INSERT_LOW     = 1, /* same as DEFAULT */
    DRM_MM_INSERT_HIGH    = 2, /* highest gap that fits */
    DRM_MM_INSERT_TOPDOWN = DRM_MM_INSERT_HIGH,
    DRM_MM_INSERT_BEST    = 3, /* tightest gap */
    DRM_MM_INSERT_ONCE    = 4, /* stop at the first candidate gap */
    DRM_MM_INSERT_EVICT   = 5, /* reserved for eviction */
};

#define DRM_MM_INSERT_MODE_FLAGS (DRM_MM_INSERT_ONCE | DRM_MM_INSERT_EVICT)

struct drm_mm;

/* One reservation in a drm_mm.  Callers embed this; the allocator fills it
 * in and never frees it. */
struct drm_mm_node {
    rb_node_t      rb;             /* link in the by-address tree          */
    uint64_t       start;          /* first byte                           */
    uint64_t       size;           /* length in bytes                      */
    uint64_t       __subtree_last; /* highest end anywhere below this node */
    bool           allocated;      /* in the tree                          */
    bool           scanned_block;  /* marked during an active scan         */
    unsigned long  color;          /* caller's own marker                  */
    struct drm_mm *mm;             /* owning allocator                     */
};

/* State for the two-phase "scan then commit" allocation path. */
struct drm_mm_scan {
    uint64_t                size;
    uint64_t                alignment;
    uint64_t                range_start;
    uint64_t                range_end;
    uint64_t                hit_start; /* first free byte of the chosen gap */
    uint64_t                hit_end;   /* one past its last free byte       */
    enum drm_mm_insert_mode mode;
    unsigned long           color;
    bool                    check_range;
    bool                    once;
};

struct drm_mm {
    rb_root_t     interval_tree;
    spinlock_t    lock;
    uint64_t      start;       /* first byte of the managed span */
    uint64_t      size;        /* its length in bytes            */
    uint64_t      alignment;
    unsigned long scan_active; /* non-zero while a scan is running */
};

#define drm_mm_initialized(mm) ((mm)->size != 0)

static inline uint64_t drm_mm_node_end(const struct drm_mm_node *node)
{
    return node->start + node->size;
}

static inline bool drm_mm_node_allocated(const struct drm_mm_node *node)
{
    return node->allocated;
}

/* Begin managing [start, start + size). */
void drm_mm_init(struct drm_mm *mm, uint64_t start, uint64_t size);

/* Stop managing the span.  Nothing is freed -- nodes belong to callers. */
void drm_mm_clean(struct drm_mm *mm);

/* True when nothing is allocated from @mm. */
bool drm_mm_clean_check(const struct drm_mm *mm);

/*
 * Reserve @size bytes for @node, aligned to @alignment (0 or a power of
 * two) and taken from within [range_start, range_end).
 * Returns 0, -EINVAL for nonsense arguments, or -ENOSPC when no gap fits.
 */
int drm_mm_insert_node_in_range(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size, uint64_t alignment,
                                uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode);

/* Reserve @size bytes anywhere in the span. */
int drm_mm_insert_node(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size);

/* Give @node's bytes back. */
void drm_mm_remove_node(struct drm_mm_node *node);

/* Move @old's reservation to @new_node, keeping its address and colour. */
void drm_mm_replace_node(struct drm_mm_node *old, struct drm_mm_node *new_node);

/* Begin a scan over the whole span. */
void drm_mm_init_scan(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                      enum drm_mm_insert_mode mode);

/* Begin a scan restricted to [range_start, range_end). */
void drm_mm_init_scan_with_range(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                                 uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode);

/* Record @node as reserved-for-now during a scan. */
bool drm_mm_scan_add_block(struct drm_mm_scan *scan, struct drm_mm_node *node);

/* Drop that record again. */
void drm_mm_scan_remove_block(struct drm_mm_scan *scan, struct drm_mm_node *node);

/* True when the scan came up with a usable gap. */
bool drm_mm_scan_color_evict(struct drm_mm_scan *scan);

/* Lowest node in @mm, or NULL when nothing is allocated. */
struct drm_mm_node *drm_mm_first(const struct drm_mm *mm);

/* Next node above @node, or NULL. */
struct drm_mm_node *drm_mm_next(const struct drm_mm_node *node);

/* Walk every allocated node from low to high. */
#define drm_mm_for_each_node(entry, mm) \
    for ((entry) = drm_mm_first(mm); (entry) != NULL; (entry) = drm_mm_next(entry))

#endif /* INCLUDE_DRM_DRM_MM_H_ */
