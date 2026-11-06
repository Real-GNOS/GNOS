/* swap.c — page swap backing on the root image. (GPLv2)
 *
 * See swap.h for the design.  This file owns the slot table and the block
 * I/O against /swapfile on the root ext2 image; vmm.c encodes swapped-out
 * PTEs and decides what to evict.
 *
 * /swapfile is expected to exist in the root image (created when the image
 * is built).  Without it swap_init leaves swap disabled: every entry point
 * then fails closed, and the fault path never sees swap-marked PTEs.
 */
#include <stddef.h>
#include <stdint.h>

#include "swap.h"
#include "vfs.h"
#include "ext2.h"
#include "pmm.h"
#include "kstring.h"
#include "debugcon.h"

/* The root image hosts the backing file; vfs.c owns the instance. */
void *vfs_root_image(void);

#define SWAP_PATH      "/swapfile"
#define SWAP_MAX_SLOTS 16384 /* 64 MiB at 4 KiB slots */

static ext2_dirent_t g_swap_ent; /* the backing file's dirent (ino, size) */
static uint8_t       g_slot_used[SWAP_MAX_SLOTS / 8];
static uint32_t      g_nslots;
static int           g_ready;

void swap_init(void)
{
    vfs_node_t n;
    if (vfs_resolve_node(SWAP_PATH, &n, 0) < 0 || !n.ops || n.kind != VFS_FILE) {
        dbg_puts("SWAP: disabled (no " SWAP_PATH " in the root image)\r\n");
        return;
    }
    g_swap_ent     = n.e2;
    uint64_t bytes = g_swap_ent.size & ~(uint64_t)0xFFF;
    g_nslots       = (uint32_t)(bytes >> 12);
    if (g_nslots > SWAP_MAX_SLOTS)
        g_nslots = SWAP_MAX_SLOTS;
    if (!g_nslots) {
        dbg_puts("SWAP: disabled (" SWAP_PATH " is empty)\r\n");
        return;
    }
    g_ready = 1;
    dbg_puts("SWAP: ready, ");
    dbg_puts_dec(g_nslots);
    dbg_puts(" slots (");
    dbg_puts_dec((uint32_t)(g_nslots >> 2));
    dbg_puts("K) on " SWAP_PATH "\r\n");
}

int swap_ready(void)
{
    return g_ready;
}

uint32_t swap_slots(void)
{
    return g_nslots;
}

uint32_t swap_alloc_slot(void)
{
    if (!g_ready)
        return (uint32_t)-1;
    for (uint32_t i = 0; i < g_nslots; i++) {
        if (!(g_slot_used[i >> 3] & (1u << (i & 7)))) {
            g_slot_used[i >> 3] |= (uint8_t)(1u << (i & 7));
            return i;
        }
    }
    return (uint32_t)-1; /* swap full: eviction must stop, not fail */
}

void swap_free_slot(uint32_t slot)
{
    if (!g_ready || slot >= g_nslots)
        return;
    g_slot_used[slot >> 3] &= (uint8_t) ~(1u << (slot & 7));
}

int swap_write_frame(uint64_t frame_phys, uint32_t slot)
{
    if (!g_ready || slot >= g_nslots)
        return -1;
    extern void *pmm_virt(uint64_t);
    uint32_t     done = ext2_write((ext2_fs_t *)vfs_root_image(), &g_swap_ent, slot << 12,
                                   pmm_virt(frame_phys), 4096);
    return done == 4096 ? 0 : -2;
}

int swap_read_frame(uint32_t slot, uint64_t frame_phys)
{
    if (!g_ready || slot >= g_nslots)
        return -1;
    extern void *pmm_virt(uint64_t);
    uint32_t     done = ext2_read((ext2_fs_t *)vfs_root_image(), &g_swap_ent, slot << 12,
                                  pmm_virt(frame_phys), 4096);
    return done == 4096 ? 0 : -2;
}

void swap_selftest(void)
{
    if (!g_ready) {
        dbg_puts("SWAP: self-test SKIP (no backing file)\r\n");
        return;
    }
    uint64_t frame = pmm_alloc_zeroed();
    if (!frame) {
        dbg_puts("SWAP: self-test SKIP (no frame)\r\n");
        return;
    }
    extern void *pmm_virt(uint64_t);
    uint8_t     *p = pmm_virt(frame);
    for (int i = 0; i < 4096; i++)
        p[i] = (uint8_t)(i * 31 + 7);

    uint32_t slot = swap_alloc_slot();
    if (slot == (uint32_t)-1 || swap_write_frame(frame, slot) < 0) {
        dbg_puts("SWAP: self-test FAIL (write)\r\n");
        pmm_free(frame);
        return;
    }
    memset(p, 0, 4096);
    int rc = swap_read_frame(slot, frame);
    for (int i = 0; i < 4096 && !rc; i++)
        if (p[i] != (uint8_t)(i * 31 + 7))
            rc = -3;
    swap_free_slot(slot);
    pmm_free(frame);
    if (rc) {
        dbg_puts("SWAP: self-test FAIL (read-back)\r\n");
        return;
    }
    dbg_puts("SWAP: self-test PASS (slot round trip)\r\n");
}
