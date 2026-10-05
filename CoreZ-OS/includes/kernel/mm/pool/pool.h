#ifndef POOL_H
#define POOL_H

#include "kernel/mm/bitmap/bitmap.h"
#include <stdint.h>

#define PAGE_SIZE 0x1000
#define MEMORY_BASE 0x100000
#define MAX_PHYS_MEM 0x20000000

struct MM_POOL {
    struct MM_BITMAP pool_bitmap;
    uint32_t phy_addr_start;
    uint32_t pool_size;
};

struct MM_VADDR {
    struct MM_BITMAP vaddr_bitmap;
    uint32_t vaddr_start;
};

extern struct MM_POOL kernel_pool;
extern struct MM_VADDR kernel_vaddr;
extern uint64_t kernel_pml4;
extern uint32_t kernel_kphys;

void pae_init(void);
void mm_init(void);
void *palloc(struct MM_POOL *pool);
uint32_t kernel_pool_free_count(void);
void pfree(struct MM_POOL *pool, uint32_t phy_addr);
uint32_t palloc_pages(struct MM_POOL *pool, uint32_t cnt);

#define COW_FLAG (1u << 9)

int page_cow_resolve(uint32_t vaddr, uint64_t pte_val);
void page_cow_share(uint32_t phy_addr);

#define VIRT_OF(phys) ((phys) + 0xC0000000ull)

#define KERNEL_VADDR_START 0x40400000u
#define KERNEL_VADDR_SIZE  0x1000000u
#define PHY_OF(vaddr) ((uint32_t)((vaddr) - 0xC0000000ull))
#define PTE_PHYS(e) ((uint64_t)(e) & 0x000ffffffffff000ull)

#define DIV_ROUND_UP(x, step) (((x) + (step) - 1) / (step))

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)
#define PTE_PS (1ull << 7)
#define PTE_NX (1ull << 63)

extern int g_nx_usable;

static inline uint64_t pte_wx(uint64_t base, int writable, int executable) {
    uint64_t nx = (uint64_t)(!(executable && !writable)) << 63;
    base = (base & ~(PTE_W | PTE_NX)) | ((uint64_t)!!writable << 1);
    if (!g_nx_usable) {
        nx = 0;
    }
    return base | nx;
}

void page_free_or_decref(uint32_t phy_addr);
void page_free_or_decref_va(uint32_t phy_addr, uint32_t vaddr);

uint64_t *pte_ptr(uint32_t vaddr);
uint64_t *pde_ptr(uint32_t vaddr);
void page_table_dump(uint32_t vaddr);
void *get_a_page(uint32_t vaddr);
int ensure_user_page(uint32_t vaddr);
void *get_kernel_pages(uint32_t pg_cnt);
uint32_t vaddr_reserve_run(uint32_t pages);
int vaddr_reserve_at(uint32_t base, uint32_t pages);
void vaddr_unreserve(uint32_t base, uint32_t pages);
void *map_reserved_page(uint32_t vaddr);

void *ioremap(uint32_t phy_addr, uint32_t size);
void free_kernel_page(uint32_t vaddr);
void free_user_page(uint32_t vaddr);
uint32_t user_remap_page(uint32_t vaddr);
uint64_t *phys_to_virt(uint64_t phys);

#endif
