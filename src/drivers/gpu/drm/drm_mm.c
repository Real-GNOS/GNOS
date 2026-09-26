/*
 * drm_mm.c - ranges carved out of a fixed span, for very small spans. (GPLv2)
 *
 * The gaps between allocated nodes are the allocator's free list, computed
 * rather than stored: walk the nodes in address order and every gap between
 * consecutive ones (plus the gap before the first and after the last) is
 * free space.  With tens of nodes that walk costs nothing and there is no
 * free list to keep consistent.
 *
 * See drm_mm.h for what each entry point promises.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm_mm.h"
#include "heap.h"
#include "rbtree.h"
#include "smp.h"
#include "vfs.h"

/* One byte past the end of @node. */
static inline uint64_t mm_node_end(const struct drm_mm_node *node)
{
    return node->start + node->size;
}

/* Round @x up to a multiple of @alignment (0 means "no requirement"). */
static inline uint64_t mm_align_up(uint64_t x, uint64_t alignment)
{
    if (alignment == 0) { return x; }
    return (x + alignment - 1) & ~(alignment - 1);
}

/* Order nodes by address. */
static int mm_less(const rb_node_t *a, const rb_node_t *b)
{
    const struct drm_mm_node *left  = rb_entry(a, struct drm_mm_node, rb);
    const struct drm_mm_node *right = rb_entry(b, struct drm_mm_node, rb);

    return left->start < right->start;
}

/*
 * Keep each node's summary current: the highest end address anywhere in its
 * subtree.  The tree refreshes this on the way up after every insertion,
 * removal and rotation.
 */
static void mm_augment(rb_node_t *rb, void *data)
{
    struct drm_mm_node *node = rb_entry(rb, struct drm_mm_node, rb);
    uint64_t            last = mm_node_end(node);

    (void)data;

    if (rb->left != NULL && rb->left->min_vruntime > last) { last = rb->left->min_vruntime; }
    if (rb->right != NULL && rb->right->min_vruntime > last) { last = rb->right->min_vruntime; }

    node->__subtree_last = last;
    rb->min_vruntime     = last;
}

void drm_mm_init(struct drm_mm *mm, uint64_t start, uint64_t size)
{
    rb_init_root(&mm->interval_tree);
    mm->lock        = (spinlock_t) {0};
    mm->start       = start;
    mm->size        = size;
    mm->alignment   = 0;
    mm->scan_active = 0;
}

void drm_mm_clean(struct drm_mm *mm)
{
    (void)mm;
}

bool drm_mm_clean_check(const struct drm_mm *mm)
{
    return rb_is_empty((rb_root_t *)&mm->interval_tree) != 0;
}

struct drm_mm_node *drm_mm_first(const struct drm_mm *mm)
{
    rb_node_t *rb = rb_first((rb_root_t *)&mm->interval_tree);

    return (rb != NULL) ? rb_entry(rb, struct drm_mm_node, rb) : NULL;
}

struct drm_mm_node *drm_mm_next(const struct drm_mm_node *node)
{
    rb_node_t *rb = rb_next((rb_node_t *)&node->rb);

    return (rb != NULL) ? rb_entry(rb, struct drm_mm_node, rb) : NULL;
}

int drm_mm_insert_node(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size)
{
    return drm_mm_insert_node_in_range(mm, node, size, 0, mm->start, mm->start + mm->size, DRM_MM_INSERT_DEFAULT);
}

int drm_mm_insert_node_in_range(struct drm_mm *mm, struct drm_mm_node *node, uint64_t size, uint64_t alignment,
                                uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode)
{
    struct drm_mm_node *prev, *entry;
    uint64_t            chosen    = 0;
    uint64_t            best_gap  = UINT64_MAX;
    int                 found     = 0;
    int                 first_fit;
    int                 stop_at_first_gap;

    if (size == 0) { return -EINVAL; }
    if (range_start >= range_end) { return -EINVAL; }

    stop_at_first_gap = (mode & DRM_MM_INSERT_ONCE) != 0;
    mode             &= ~DRM_MM_INSERT_MODE_FLAGS;

    /* Only best-fit has to see every gap; everything else can stop at the
     * one it would have taken. */
    first_fit = (mode != DRM_MM_INSERT_BEST && mode != DRM_MM_INSERT_HIGH);

    spin_lock(&mm->lock);

    prev = NULL;
    entry = drm_mm_first(mm);
    while (1) {
        uint64_t gap_start = (prev != NULL) ? mm_node_end(prev) : mm->start;
        uint64_t gap_end   = (entry != NULL) ? entry->start : mm->start + mm->size;
        uint64_t start;

        if (gap_start < range_start) { gap_start = range_start; }
        if (gap_end > range_end) { gap_end = range_end; }

        if (gap_start < gap_end) {
            start = mm_align_up(gap_start, alignment);

            if (start + size <= gap_end) {
                found = 1;

                if (mode == DRM_MM_INSERT_BEST) {
                    uint64_t gap = gap_end - gap_start;
                    if (gap < best_gap) {
                        best_gap = gap;
                        chosen   = start;
                    }
                } else {
                    chosen = start; /* top-down keeps taking later gaps */
                }

                if (first_fit) { break; }
            }
        }

        if (entry == NULL) { break; }
        prev  = entry;
        entry = drm_mm_next(entry);
        if (stop_at_first_gap && found) { break; }
    }

    if (found) {
        node->start          = chosen;
        node->size           = size;
        node->mm             = mm;
        node->allocated      = true;
        node->scanned_block  = false;
        node->color          = 0;
        node->__subtree_last = chosen + size;

        rb_insert_augmented(&mm->interval_tree, &node->rb, mm_less, mm_augment, NULL);
        spin_unlock(&mm->lock);
        return 0;
    }

    spin_unlock(&mm->lock);
    return -ENOSPC;
}

void drm_mm_remove_node(struct drm_mm_node *node)
{
    if (!node->allocated) { return; }

    spin_lock(&node->mm->lock);
    rb_erase_augmented(&node->mm->interval_tree, &node->rb, mm_augment, NULL);
    node->allocated = false;
    spin_unlock(&node->mm->lock);
}

void drm_mm_replace_node(struct drm_mm_node *old, struct drm_mm_node *new_node)
{
    spin_lock(&old->mm->lock);

    new_node->start          = old->start;
    new_node->size           = old->size;
    new_node->allocated      = old->allocated;
    new_node->__subtree_last = old->__subtree_last;
    new_node->color          = old->color;
    new_node->mm             = old->mm;

    rb_erase_augmented(&old->mm->interval_tree, &old->rb, mm_augment, NULL);
    rb_insert_augmented(&new_node->mm->interval_tree, &new_node->rb, mm_less, mm_augment, NULL);

    spin_unlock(&old->mm->lock);
}

void drm_mm_init_scan(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                      enum drm_mm_insert_mode mode)
{
    drm_mm_init_scan_with_range(mm, scan, size, alignment, mm->start, mm->start + mm->size, mode);
    scan->check_range = false;
}

void drm_mm_init_scan_with_range(struct drm_mm *mm, struct drm_mm_scan *scan, uint64_t size, uint64_t alignment,
                                 uint64_t range_start, uint64_t range_end, enum drm_mm_insert_mode mode)
{
    scan->size        = size;
    scan->alignment   = alignment;
    scan->range_start = range_start;
    scan->range_end   = range_end;
    scan->hit_start   = 0;
    scan->hit_end     = 0;
    scan->mode        = mode;
    scan->color       = 0;
    scan->check_range = true;
    scan->once        = false;
    mm->scan_active++;
}

bool drm_mm_scan_add_block(struct drm_mm_scan *scan, struct drm_mm_node *node)
{
    (void)scan;

    node->scanned_block = true;
    return true;
}

void drm_mm_scan_remove_block(struct drm_mm_scan *scan, struct drm_mm_node *node)
{
    (void)scan;

    node->scanned_block = false;
}

bool drm_mm_scan_color_evict(struct drm_mm_scan *scan)
{
    return scan->hit_end > scan->hit_start;
}
