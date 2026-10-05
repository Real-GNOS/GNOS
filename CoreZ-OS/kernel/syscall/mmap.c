#include "kernel/syscall/mmap.h"
#include "kernel/assert.h"
#include "kernel/syscall/linux_abi.h"
#include "lib/str/str.h"
#include "kernel/mm/bitmap/bitmap.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/access.h"
#include "drivers/char/console/io.h"
#define MMAP_MAX_BYTES 0x10000000u
#define PROT_MASK (PROT_READ | PROT_WRITE | PROT_EXEC)
#define OFF_MASK 0x000ffffffffff000ull
static int32_t unmap_pages(uint32_t addr, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++)
        free_user_page(addr + i * PAGE_SIZE);
    return 0;
}

int page_is_mapped(uint32_t v) {
    uint64_t *pde = pde_ptr(v);
    if (pde == NULL)
        return 0;
    if (*pde & PTE_PS)
        return 1;
    uint64_t *pte = pte_ptr(v);
    return (pte != NULL && (*pte & PTE_P)) ? 1 : 0;
}

static void apply_prot(uint32_t v, uint32_t prot) {
    uint64_t *pte = pte_ptr(v);
    if (pte == NULL || !(*pte & PTE_P))
        return;
    *pte = (*pte & OFF_MASK) | pte_wx(PTE_P | PTE_U, !!(prot & PROT_WRITE),
                                      !!(prot & PROT_EXEC));
    __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
}

static uint32_t map_run(uint32_t base, uint32_t pages, uint32_t prot) {
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t v = base + i * PAGE_SIZE;
        if (map_reserved_page(v) == 0) {
            unmap_pages(base, i);
            return 0;
        }
        apply_prot(v, prot);
    }
    return base;
}

static void fill_file(uint32_t fd, uint32_t off, uint32_t base, uint32_t len) {
    int32_t old = sys_lseek((int32_t)fd, 0, SEEK_CUR);
    if (sys_lseek((int32_t)fd, (int32_t)off, SEEK_SET) < 0)
        return;
    for (uint32_t done = 0; done < len;) {
        int32_t n =
            (int32_t)read_file((int32_t)fd, (void *)(base + done), len - done);
        if (n <= 0)
            break;
        done += (uint32_t)n;
    }
    if (old >= 0)
        sys_lseek((int32_t)fd, old, 0);
}

uint32_t sys_mmap(const struct SYS_MMAP_ARGS *a) {
    if (a == NULL)
        return -LINUX_EFAULT;
    uint32_t len = a->len;
    if (len == 0 || len > MMAP_MAX_BYTES ||
        (a->prot & ~PROT_MASK))
        return -LINUX_EINVAL;
    uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    struct TASK *cur = current;
    uint32_t span = pages * PAGE_SIZE;
    uint32_t base;
    if (a->flags & MAP_FIXED) {
        if (a->addr == 0 || (a->addr & (PAGE_SIZE - 1)) ||
            a->addr < cur->userprog_v_addr.vaddr_start)
            return -LINUX_EINVAL;
        if (span > USER_SPACE_END - a->addr ||
            (a->addr < USER_HIGH_MMIO_END && a->addr + span > USER_LOW_CEILING))
            return -LINUX_ENOMEM;
        unmap_pages(a->addr, pages);
        if (vaddr_reserve_at(a->addr, pages) != 0)
            return -LINUX_ENOMEM;
        base = a->addr;
    } else {
        base = vaddr_reserve_run(pages);
    }
    if (base == 0)
        return -LINUX_ENOMEM;
    if (map_run(base, pages, PROT_READ | PROT_WRITE) == 0) {
        unmap_pages(base, pages);
        vaddr_unreserve(base, pages);
        return -LINUX_ENOMEM;
    }
    if (!(a->flags & MAP_ANONYMOUS) && (int32_t)a->fd >= 0)
        fill_file(a->fd, a->offset, base, len);

    for (uint32_t i = 0; i < pages; i++)
        apply_prot(base + i * PAGE_SIZE, a->prot);
    return base;
}

uint32_t sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags,
                   uint32_t fd, uint32_t offset) {
    struct SYS_MMAP_ARGS a = {addr, len, prot, flags, fd, offset << 12};
    return sys_mmap(&a);
}

int32_t sys_munmap(uint32_t addr, uint32_t len) {
    if (addr == 0 || len == 0 || (addr & (PAGE_SIZE - 1)))
        return -LINUX_EINVAL;
    if (addr < USER_VADDR_START || addr >= USER_SPACE_END ||
        len > USER_SPACE_END - addr)
        return -LINUX_EINVAL;
    uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (addr < USER_HIGH_MMIO_END && addr + pages * PAGE_SIZE > USER_LOW_CEILING)
        return -LINUX_EINVAL;
    unmap_pages(addr, pages);
    return 0;
}

int32_t sys_mprotect(uint32_t addr, uint32_t len, uint32_t prot) {
    if (addr & (PAGE_SIZE - 1) || (prot & ~PROT_MASK))
        return -LINUX_EINVAL;
    if (len == 0)
        return 0;
    if (addr < USER_VADDR_START || addr >= USER_SPACE_END ||
        len > USER_SPACE_END - addr)
        return -LINUX_EINVAL;
    uint32_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (addr < USER_HIGH_MMIO_END && addr + pages * PAGE_SIZE > USER_LOW_CEILING)
        return -LINUX_EINVAL;
    for (uint32_t i = 0; i < pages; i++)
        apply_prot(addr + i * PAGE_SIZE, prot);
    return 0;
}
