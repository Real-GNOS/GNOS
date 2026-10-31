/* buddy.h — the physical page buddy allocator. (GPLv2)
 *
 * A power-of-two page allocator that sits on top of the PMM bitmap: it
 * requests big chunks from pmm_alloc_contiguous on demand, keeps them in
 * order-indexed free lists, and serves larger contiguous requests (where
 * the bitmap's first-fit scan is O(total frames)) by splitting and
 * merging.  Single-frame allocations keep using the bitmap fast path.
 *
 * All functions deal in physical addresses; convert with pmm_virt().
 * Every entry point takes and releases the PMM lock's own spinlock domain
 * is NOT shared — buddy has its own, and must not be called while a PMM
 * lock is held (the integration point in pmm.c drops it first).
 */
#ifndef GNUCOS_BUDDY_H
#define GNUCOS_BUDDY_H

#include <stdint.h>

/* Largest span the buddy will carve or hold: 2^BUDDY_MAX_ORDER frames.
 * 512 frames (2 MiB with 4 KiB pages) keeps the chunk the base allocator
 * has to find in one piece small enough to succeed even on a fragmented
 * map, while covering every contiguous request the kernel actually makes
 * (dumb buffers, DMA rings, module images). */
#define BUDDY_MAX_ORDER 9

/* Bring the allocator up.  Safe to call twice; the second call is a no-op.
 * Nothing is taken from the PMM until the first allocation, so early boot
 * (where the bitmap is still being parked) is unaffected. */
void buddy_init(void);

/* Allocate 2^order contiguous frames.  Returns the physical address of the
 * first frame, or 0 when no span of that size is available. */
uint64_t buddy_alloc(unsigned order);

/* Return a span of 2^order frames obtained from buddy_alloc.  Passing an
 * order or address this allocator did not hand out corrupts the freelists;
 * callers keep the order alongside the allocation, like pmm does for its
 * record-keeping allocations. */
void buddy_free(uint64_t phys, unsigned order);

/* Frames the buddy currently holds free (not counting chunks it has not
 * yet requested from the PMM). */
uint64_t buddy_free_frames(void);

/* Boot-time self-test: alloc/free/split/merge round trip.  Prints
 * "BUDDY: self-test ... PASS/FAIL" on the debug console. */
void buddy_selftest(void);

#endif /* GNUCOS_BUDDY_H */
