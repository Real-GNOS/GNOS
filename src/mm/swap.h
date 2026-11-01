/* swap.h — page swap backing on the root image. (GPLv2)
 *
 * Anonymous user pages can be evicted to a swap area at the tail of the
 * root ext2 image (/swapfile, slot-granular 4 KiB blocks).  mm/swap.c owns
 * only the slot table and the block I/O; the eviction scan and the
 * PTE-level swap encoding live in vmm.c, which owns the page tables.
 *
 * Encoding (in vmm.c): a swapped-out page's PTE has PTE_P clear, PTE_AVL
 * set as the swap marker, and the slot number in the address field.
 *
 * The backing file must exist before swap_init(); a missing file leaves
 * swap disabled and every entry point a no-op, so a root image without
 * /swapfile simply never swaps.
 */
#ifndef GNUCOS_SWAP_H
#define GNUCOS_SWAP_H

#include <stdint.h>

/* Swap-in this many free frames or fewer.  64 MiB of headroom: swapping
 * starts well before the bitmap allocator starts failing allocations. */
#define SWAP_LOWMARK_FRAMES 16384

/* Bring the swap area up: resolve /swapfile on the root image and size the
 * slot table from it.  Safe to call once, at boot. */
void swap_init(void);

/* Non-zero when a backing file is attached. */
int swap_ready(void);

/* Total usable slots (4 KiB each). */
uint32_t swap_slots(void);

/* Slot table.  Returns the slot number, or (uint32_t)-1 when full/disabled. */
uint32_t swap_alloc_slot(void);
void     swap_free_slot(uint32_t slot);

/* Move one 4 KiB frame between memory and its slot.  Both take the physical
 * frame address; conversion to a kernel pointer happens here.  Returns 0 on
 * success, negative errno on I/O failure. */
int swap_write_frame(uint64_t frame_phys, uint32_t slot);
int swap_read_frame(uint32_t slot, uint64_t frame_phys);

/* Boot-time self-test: slot round trip through the backing file. */
void swap_selftest(void);

#endif /* GNUCOS_SWAP_H */
