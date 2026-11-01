/* SPDX-License-Identifier: GPL-2.0 */
/* buddy.c — the physical page buddy allocator. (GPLv2)
 *
 * Sits on top of the PMM bitmap: it requests 2 MiB chunks from
 * pmm_alloc_contiguous on demand, keeps them in order-indexed free lists,
 * and serves larger contiguous requests (where the bitmap's first-fit scan
 * is O(total frames)) by splitting and merging.  Single-frame allocations
 * keep using the bitmap fast path.
 *
 * Free blocks are linked through their first word (the frame is free, so
 * nobody reads it): *(uint64_t *)pmm_virt(phys) = next head.
 *
 * A block is "free" iff its chunk bitmap bit is clear AND it is reachable
 * from g_free_list[its order]; merging therefore requires the buddy to be
 * the current head of its list, the classic buddy-invariant that keeps the
 * lists free of duplicates without a second bitmap.
 */
#include <stddef.h>
#include <stdint.h>

#include "buddy.h"
#include "pmm.h"
#include "panic.h"
#include "debugcon.h"
#include "kstring.h"
#include "smp.h"

/* Frames per chunk: 2^BUDDY_MAX_ORDER (512 frames = 2 MiB at 4 KiB pages).
 * 16 chunks cap the pool at 32 MiB — plenty for GEM backings, DMA rings and
 * module images, while the static metadata stays a few hundred bytes. */
#define BUDDY_CHUNK_FRAMES (1u << BUDDY_MAX_ORDER)
#define BUDDY_MAX_CHUNKS   16

typedef struct {
    uint64_t base; /* physical address of frame 0; 0 = slot unused */
    uint8_t  used[BUDDY_CHUNK_FRAMES / 8]; /* bit i: frame i allocated */
} buddy_chunk_t;

static buddy_chunk_t g_chunks[BUDDY_MAX_CHUNKS];
static uint64_t      g_free_list[BUDDY_MAX_ORDER + 1]; /* head phys, 0 = empty */
static uint64_t      g_free_count;
static int           g_inited;
static spinlock_t    g_buddy_lock;

static inline uint64_t frame_bytes(unsigned order)
{
    return (uint64_t)1 << (order + 12);
}

/* Which chunk (if any) owns this physical address. */
static buddy_chunk_t *chunk_of(uint64_t phys)
{
    for (int i = 0; i < BUDDY_MAX_CHUNKS; i++)
        if (g_chunks[i].base && phys >= g_chunks[i].base &&
            phys < g_chunks[i].base + frame_bytes(BUDDY_MAX_ORDER))
            return &g_chunks[i];
    return NULL;
}

static void list_push(unsigned order, uint64_t phys)
{
    *(uint64_t *)pmm_virt(phys) = g_free_list[order];
    g_free_list[order]          = phys;
    g_free_count += frame_bytes(order) >> 12;
}

/* Remove the head of an order's list; returns its phys or 0. */
static uint64_t list_pop(unsigned order)
{
    uint64_t phys = g_free_list[order];
    if (phys) {
        g_free_list[order] = *(uint64_t *)pmm_virt(phys);
        g_free_count -= frame_bytes(order) >> 12;
    }
    return phys;
}

void buddy_init(void)
{
    if (g_inited)
        return;
    g_inited = 1;
}

/* Carve a fresh chunk from the PMM and seed its free lists: one block of
 * each order 0..MAX-1 at offset (order << order), the whole chunk at MAX. */
static int chunk_new(void)
{
    for (int i = 0; i < BUDDY_MAX_CHUNKS; i++) {
        if (g_chunks[i].base)
            continue;
        uint64_t phys = pmm_alloc_contiguous(BUDDY_CHUNK_FRAMES);
        if (!phys)
            return -1;
        buddy_chunk_t *c = &g_chunks[i];
        c->base          = phys;
        memset(c->used, 0, sizeof(c->used));
        for (int o = BUDDY_MAX_ORDER - 1; o >= 0; o--)
            list_push((unsigned)o, phys + ((uint64_t)o << (o + 12)));
        g_free_list[BUDDY_MAX_ORDER] = 0; /* whole-chunk entry is split */
        return 0;
    }
    return -1; /* pool full */
}

uint64_t buddy_alloc(unsigned order)
{
    if (!g_inited || order > BUDDY_MAX_ORDER)
        return 0;

    uint64_t flags; (void)flags; spin_lock_irq(&g_buddy_lock);
    uint64_t phys  = 0;
    for (;;) {
        if (g_free_list[order]) {
            phys = list_pop(order);
            buddy_chunk_t *c = chunk_of(phys);
            if (!c) /* cannot happen: every list block lives in a chunk */
                panic("buddy: list block outside pool");
            uint64_t idx = (phys - c->base) >> 12;
            c->used[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            break;
        }
        if (order == BUDDY_MAX_ORDER) {
            if (chunk_new() < 0)
                break; /* pool full or PMM exhausted */
            continue; /* chunk_new seeded every order's list */
        }
        /* Split: take a block of the next order up, serve its upper half,
         * keep its lower half on this order's list.  (Parking the upper
         * half instead would strand the lower one: popped from the higher
         * list and pushed nowhere -- half the pool leaked per split.) */
        if (!g_free_list[order + 1]) {
            if (chunk_new() < 0)
                break; /* pool full or PMM exhausted */
            continue;
        }
        uint64_t big = list_pop((unsigned)(order + 1));
        list_push(order, big); /* lower half stays free */
        phys = big + frame_bytes(order); /* upper half is the answer */
        buddy_chunk_t *c = chunk_of(phys);
        if (!c)
            panic("buddy: split block outside pool");
        uint64_t idx = (phys - c->base) >> 12;
        c->used[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        break;
    }
    spin_unlock_irq(&g_buddy_lock);
    return phys;
}

void buddy_free(uint64_t phys, unsigned order)
{
    if (!g_inited || order > BUDDY_MAX_ORDER || !phys)
        return;

    uint64_t flags; (void)flags; spin_lock_irq(&g_buddy_lock);
    buddy_chunk_t *c = chunk_of(phys);
    if (!c) {
        spin_unlock_irq(&g_buddy_lock);
        panic("buddy: free of foreign span");
    }
    uint64_t idx  = (phys - c->base) >> 12;
    uint64_t rel  = idx; /* frame index inside the chunk */

    for (;;) {
        c->used[rel >> 3] &= (uint8_t)~(1u << (rel & 7));

        if (order == BUDDY_MAX_ORDER)
            break; /* chunk-sized blocks stay pooled, never returned */

        /* Merge upward while the buddy is free and sitting at the head of
         * its list (the head-only rule keeps blocks unique per order).
         * Accounting is list-driven: list_pop removes the buddy's count,
         * the final list_push re-adds the merged size -- no manual tweaks. */
        uint64_t bud_rel = rel ^ ((uint64_t)1 << order);
        uint8_t  bit     = c->used[bud_rel >> 3] & (1u << (bud_rel & 7));
        uint64_t bud     = c->base + (bud_rel << 12);
        if (bit || g_free_list[order] != bud)
            break;

        list_pop(order); /* removes the buddy */
        rel  = (rel < bud_rel) ? rel : bud_rel;
        phys = c->base + (rel << 12);
        order++;
    }

    list_push(order, c->base + (rel << 12));
    spin_unlock_irq(&g_buddy_lock);
}

uint64_t buddy_free_frames(void)
{
    return g_free_count;
}

/* Is this physical address inside a buddy-managed chunk?  Used by
 * pmm_free_span to route a release to the right allocator. */
int buddy_owns(uint64_t phys)
{
    return chunk_of(phys) != NULL;
}

void buddy_selftest(void)
{
    buddy_init();

    /* Warm the pool: the first allocation pulls a chunk from the PMM and
     * seeds it, which legitimately changes the free count.  The checks
     * below compare against a baseline taken on a warm pool. */
    uint64_t warm = buddy_alloc(3);
    if (!warm) {
        dbg_puts("BUDDY: self-test SKIP (no chunk available)\r\n");
        return;
    }
    buddy_free(warm, 3);

    /* Round trip: take a span, give it back, the pool must be untouched. */
    uint64_t before = g_free_count;
    uint64_t a = buddy_alloc(3); /* 8 frames */
    if (!a) {
        dbg_puts("BUDDY: self-test FAIL (alloc)\r\n");
        return;
    }
    buddy_free(a, 3);
    if (g_free_count != before) {
        dbg_puts("BUDDY: self-test FAIL (accounting)\r\n");
        return;
    }

    /* Merge: the second order-2 allocation is produced by splitting an
     * order-3 block (its lower half stays on the order-2 list), so freeing
     * it back must coalesce both halves into the order-3 block again. */
    uint64_t a2 = buddy_alloc(2); /* consumes the seeded order-2 block */
    uint64_t b2 = buddy_alloc(2); /* split product of an order-3 block */
    if (!a2 || !b2) {
        dbg_puts("BUDDY: self-test FAIL (merge alloc)\r\n");
        return;
    }
    buddy_free(b2, 2);
    if (g_free_list[3] == 0) {
        dbg_puts("BUDDY: self-test FAIL (merge)\r\n");
        return;
    }
    list_pop(3); /* restore the pool to the pre-test shape */
    buddy_free(a2, 2);

    dbg_puts("BUDDY: self-test PASS (pool ");
    dbg_puts_dec((uint32_t)(g_free_count >> 10));
    dbg_puts("K free)\r\n");
}
