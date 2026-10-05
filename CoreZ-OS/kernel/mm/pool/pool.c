#include "kernel/mm/pool/pool.h"
#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "lib/rand/rand.h"
#include "kernel/sched/percpu.h"
#include "kernel/sched/sync.h"
#include "kernel/sched/thread.h"
#include "kernel/userprog/process.h"
#include "kernel/mm/access.h"
#include "kernel/init/mb2.h"

/**
 * 内存池并发模型。
 *
 * @remarks
 * 原为单把 mem_lock 串行一切，现按关注点拆成两把：
 *  - pool_lock 只保护物理页位图 kernel_pool.pool_bitmap 与空闲页计数；
 *  - map_lock  只保护页表结构与虚拟地址位图（kernel_vaddr / userprog_vaddr），
 *              同时也是 COW 引用计数 frame_owner 的保护者。
 * 获取顺序固定为 map_lock -> pool_lock（页表增长需要向物理池要页），不存在反向
 * 路径，因此不会死锁
 */
static struct SCHED_LOCK pool_lock;
static struct SCHED_LOCK map_lock;

/**
 * 每 CPU 单页缓存（magazine）。
 *
 * @remarks
 * 单页 palloc/pfree 优先在本地缓存完成，仅缓存补充/回吐时才触碰 pool_lock，
 * 避免全局位图成为唯一串行点。缓存中的页仍计入 pool_free_pages（对调用方而言
 * 仍是空闲资源）。仅在调度器已启动时启用，见 pcpu_cache_cpu
 */
#define PCP_CACHE_MAX 64
static uint32_t pcpu_page_cache[NR_CPU][PCP_CACHE_MAX];
static uint32_t pcpu_cache_count[NR_CPU];
static volatile uint32_t pool_free_pages;

#define PML4_INDEX(v) (((uint64_t)(v) >> 39) & 0x1ff)
#define PDPT_INDEX(v) (((uint64_t)(v) >> 30) & 0x1ff)
#define PD_INDEX(v) (((uint64_t)(v) >> 21) & 0x1ff)
#define PT_INDEX(v) (((uint64_t)(v) >> 12) & 0x1ff)

static uint8_t kernel_pool_bitmap[(MAX_PHYS_MEM - MEMORY_BASE) / PAGE_SIZE / 8];
static uint8_t kernel_vaddr_bitmap[0x1000000 / PAGE_SIZE / 8];
struct MM_POOL kernel_pool;
struct MM_VADDR kernel_vaddr;
#define FRAME_IDX(phy) (((phy) - MEMORY_BASE) / PAGE_SIZE)
#define FRAME_IDX_MAX ((MAX_PHYS_MEM - MEMORY_BASE) / PAGE_SIZE)
static volatile uint8_t frame_owner[FRAME_IDX_MAX];
uint64_t kernel_pml4;
uint32_t kernel_kphys;

static int fr_arm = 1;
static uint32_t fr_va[FRAME_IDX_MAX];
static int fr_va_reports;

static void fr_va_bad(const char *tag, uint32_t vaddr, uint32_t prev,
                      uint32_t phy) {
    if (fr_va_reports >= 40) {
        return;
    }
    fr_va_reports++;
    struct TASK *c = current;
    kprintf("[fr-va] %s va=%x prev=%x phy=%x pid=%d\n", tag, vaddr, prev, phy,
            c != NULL ? (int)c->pid : -1);
}

static void fr_va_map(uint32_t vaddr, uint32_t phy, const char *tag) {
    if (phy < MEMORY_BASE || phy >= MAX_PHYS_MEM) {
        return;
    }
    uint32_t idx = FRAME_IDX(phy);
    uint32_t prev = fr_va[idx];
    if (prev != 0 && prev != vaddr && frame_owner[idx] == 0) {
        fr_va_bad(tag, vaddr, prev, phy);
    }
    fr_va[idx] = vaddr;
}

static void fr_va_unmap(uint32_t vaddr, uint32_t phy) {
    if (phy < MEMORY_BASE || phy >= MAX_PHYS_MEM) {
        return;
    }
    uint32_t idx = FRAME_IDX(phy);
    uint32_t prev = fr_va[idx];
    if (prev != 0 && prev != vaddr && frame_owner[idx] == 0) {
        fr_va_bad("wrongfree", vaddr, prev, phy);
        return;
    }
    if (prev == vaddr) {
        fr_va[idx] = 0;
    }
}

static uint32_t e820_mem_upper(void) {
    uint64_t top = mb2_mem_top();
    if (top >= 0xFFFFFFFFull)
        return 0xFFFFFFFFu;
    if (top != 0)
        return (uint32_t)top;
    uint32_t count = *(uint32_t *)0x6000;
    uint8_t *p = (uint8_t *)0x6004;
    uint32_t upper = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint64_t base = *(uint64_t *)p;
        uint64_t len = *(uint64_t *)(p + 8);
        uint32_t type = *(uint32_t *)(p + 16);
        if (type == 1 && (uint32_t)(base + len) > upper) {
            upper = (uint32_t)(base + len);
        }
        p += 24;
    }
    return upper;
}

static void mark_used(uint32_t start, uint32_t size) {
    uint32_t end = start + size;
    while (start < end) {
        uint32_t idx = (start - kernel_pool.phy_addr_start) / PAGE_SIZE;
        if (idx < kernel_pool.pool_bitmap.btmp_bytes_len * 8) {
            bitmap_set(&kernel_pool.pool_bitmap, idx, 1);
        }
        start += PAGE_SIZE;
    }
}

static void mark_free(uint32_t start, uint32_t size) {
    uint32_t end = start + size;
    while (start < end) {
        uint32_t idx = (start - kernel_pool.phy_addr_start) / PAGE_SIZE;
        if (idx < kernel_pool.pool_bitmap.btmp_bytes_len * 8) {
            bitmap_set(&kernel_pool.pool_bitmap, idx, 0);
        }
        start += PAGE_SIZE;
    }
}

#define EFER_MSR 0xc0000080u
#define EFER_NXE (1ull << 11)
#define CPUID_NX (1u << 20)

static int cpuid_has_nx(void) {
    uint32_t a = 0x80000001, d;
    __asm__ volatile("cpuid" : "+a"(a), "=d"(d) : : "ebx", "ecx");
    return (d & CPUID_NX) != 0;
}

int g_nx_usable = 0;

void pae_init(void) {
    if (cpuid_has_nx()) {
        uint64_t efer = asm_rdmsr(EFER_MSR);
        asm_wrmsr(EFER_MSR, efer | EFER_NXE);
        g_nx_usable = (asm_rdmsr(EFER_MSR) & EFER_NXE) != 0;
    } else {
        g_nx_usable = 0;
    }
}

static uint32_t pool_bitmap_free_bits(const struct MM_BITMAP *btmp) {
    const uint64_t *words = (const uint64_t *)btmp->bits;
    uint32_t nwords = btmp->btmp_bytes_len >> 3;
    uint32_t n = 0;
    for (uint32_t i = 0; i < nwords; i++) {
        n += (uint32_t)__builtin_popcountll(~words[i]);
    }
    for (uint32_t byte = nwords << 3; byte < btmp->btmp_bytes_len; byte++) {
        n += 8 - (uint32_t)__builtin_popcount(btmp->bits[byte]);
    }
    return n;
}

void mm_init(void) {
    pae_init();
    uint32_t upper = e820_mem_upper();
    kernel_pool.phy_addr_start = MEMORY_BASE;
    if (upper <= MEMORY_BASE) {
        upper = MEMORY_BASE + 0x100000;
    }
    if (upper > MAX_PHYS_MEM) {
        upper = MAX_PHYS_MEM;
    }
    kernel_pool.pool_size = upper - MEMORY_BASE;
    kernel_pool.pool_bitmap.bits = kernel_pool_bitmap;
    kernel_pool.pool_bitmap.btmp_bytes_len = sizeof(kernel_pool_bitmap);
    bitmap_init(&kernel_pool.pool_bitmap);
    mark_used(kernel_pool.phy_addr_start, kernel_pool.pool_size);
    {
        const struct MB2_INFO *mb2 = mb2_get();
        int freed_any = 0;
        if (mb2 != NULL && mb2->has_mmap) {
            for (uint32_t i = 0; i < mb2->mmap_count; i++) {
                const struct MB2_MMAP_ENTRY *e = &mb2->mmap[i];
                if (e->type != MB2_MMAP_AVAILABLE) {
                    continue;
                }
                uint64_t s = e->addr;
                uint64_t t = e->addr + e->len;
                if (s < kernel_pool.phy_addr_start) {
                    s = kernel_pool.phy_addr_start;
                }
                if (t > upper) {
                    t = upper;
                }
                s = (s + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                t &= ~(uint64_t)(PAGE_SIZE - 1);
                if (s >= t) {
                    continue;
                }
                mark_free((uint32_t)s, (uint32_t)(t - s));
                freed_any = 1;
            }
        }
        if (!freed_any) {
            uint32_t legacy_end =
                upper < 0x1000000u ? upper : 0x1000000u;
            if (legacy_end > kernel_pool.phy_addr_start) {
                mark_free(kernel_pool.phy_addr_start,
                          legacy_end - kernel_pool.phy_addr_start);
            }
        }
    }
    extern char _kernel_phys_start;
    extern char _kernel_phys_end;
    {

        uint32_t koff = (uint32_t)(uintptr_t)&_kernel_phys_start - 0x200000u;
        uint32_t kspan =
            (uint32_t)((uintptr_t)&_kernel_phys_end -
                       (uintptr_t)&_kernel_phys_start);
        mark_used(kernel_kphys + koff, kspan);
    }

    {
        uint64_t *pml4 = phys_to_virt(asm_read_cr3());
        for (int i = 0; i < 512; i++) {
            uint64_t e = pml4[i];
            if (!(e & 1))
                continue;
            uint64_t pdp_phys = PTE_PHYS(e);
            mark_used((uint32_t)pdp_phys, PAGE_SIZE);
            uint64_t *pdp = phys_to_virt(pdp_phys);
            for (int j = 0; j < 512; j++) {
                uint64_t e2 = pdp[j];
                if (!(e2 & 1))
                    continue;
                if (e2 & 0x80)
                    continue;
                uint64_t pd_phys = PTE_PHYS(e2);
                mark_used((uint32_t)pd_phys, PAGE_SIZE);
                uint64_t *pd = phys_to_virt(pd_phys);
                for (int k = 0; k < 512; k++) {
                    uint64_t e3 = pd[k];
                    if (!(e3 & 1))
                        continue;
                    if (e3 & 0x80)
                        continue;
                    mark_used((uint32_t)PTE_PHYS(e3), PAGE_SIZE);
                }
            }
        }
    }
    uint32_t shadow_end = (uint32_t)(((uintptr_t)&_kernel_phys_end + 0x1FFFFFu) &
                                     ~(uintptr_t)0x1FFFFFu);
    mark_used(0x200000, shadow_end - 0x200000);
    mark_used(PER_CPU_BASE, NR_CPU * PAGE_SIZE);
    {
        uint32_t pool_pages = kernel_pool.pool_size / PAGE_SIZE;
        for (uint32_t i = pool_pages; i < FRAME_IDX_MAX; i++) {
            bitmap_set(&kernel_pool.pool_bitmap, i, 1);
        }
    }
    kernel_vaddr.vaddr_start = KERNEL_VADDR_START;
    kernel_vaddr.vaddr_bitmap.bits = kernel_vaddr_bitmap;
    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = sizeof(kernel_vaddr_bitmap);
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    kernel_pml4 = asm_read_cr3();
    lock_init(&pool_lock);
    lock_init(&map_lock);
    for (uint32_t i = 0; i < NR_CPU; i++) {
        pcpu_cache_count[i] = 0;
    }
    pool_free_pages = pool_bitmap_free_bits(&kernel_pool.pool_bitmap);

    {
        uint64_t *pd98 = (uint64_t *)VIRT_OF(0x98000);
        pd98[4] = (uint64_t)0x00800000u | 0x83;
        pd98[7] = (uint64_t)0x00E00000u | 0x83;
    }
}

static uint32_t palloc_raw(struct MM_POOL *pool) {
    int idx = bitmap_scan(&pool->pool_bitmap, 1);
    if (idx == -1) {
        return 0;
    }
    bitmap_set(&pool->pool_bitmap, (uint32_t)idx, 1);
    uint32_t phy = pool->phy_addr_start + (uint32_t)idx * PAGE_SIZE;
    ASSERT((phy & 0xfffu) == 0);
    return phy;
}

static void pfree_raw(struct MM_POOL *pool, uint32_t phy_addr) {
    if (phy_addr < pool->phy_addr_start) {
        return;
    }
    uint32_t idx = (phy_addr - pool->phy_addr_start) / PAGE_SIZE;
    ASSERT(idx < pool->pool_bitmap.btmp_bytes_len * 8);
    ASSERT((phy_addr & 0xfffu) == 0);
    bitmap_set(&pool->pool_bitmap, idx, 0);
}

static uint32_t palloc_pages_raw(struct MM_POOL *pool, uint32_t cnt) {
    int idx = bitmap_scan(&pool->pool_bitmap, cnt);
    if (idx == -1) {
        return 0;
    }
    ASSERT(idx + cnt <= (int)(pool->pool_bitmap.btmp_bytes_len * 8));
    for (uint32_t i = 0; i < cnt; i++) {
        bitmap_set(&pool->pool_bitmap, (uint32_t)idx + i, 1);
    }
    uint32_t phy = pool->phy_addr_start + (uint32_t)idx * PAGE_SIZE;
    ASSERT((phy & 0xfffu) == 0);
    return phy;
}

/**
 * 从每 CPU 缓存补充一批页。
 *
 * @param c 目标缓存下标
 *
 * @remarks
 * 位图 -> 缓存只是"空闲页换个地方放"，对调用方仍是空闲资源，因此不改变
 * pool_free_pages。内部自行获取 pool_lock，直到缓存填满或物理池耗尽
 */
static void pcpu_refill(uint32_t c) {
    lock_acquire(&pool_lock);
    while (pcpu_cache_count[c] < PCP_CACHE_MAX) {
        uint32_t phy = palloc_raw(&kernel_pool);
        if (phy == 0) {
            break;
        }
        pcpu_page_cache[c][pcpu_cache_count[c]++] = phy;
    }
    lock_release(&pool_lock);
}

/**
 * 判断当前 CPU 是否可使用本地页缓存，并输出缓存下标。
 *
 * @param cpu 输出参数：缓存下标
 * @returns 1 表示可用；0 表示不可用（调度器未启动或 cpu_id 越界）
 *
 * @remarks
 * current 为 0 时 GS 基址尚未建立，cpu_id() 不可信，故禁用缓存并退回全局锁路径
 */
static int pcpu_cache_cpu(uint32_t *cpu) {
    if (current == 0) {
        return 0;
    }
    uint32_t c = cpu_id();
    if (c >= NR_CPU) {
        return 0;
    }
    *cpu = c;
    return 1;
}

/**
 * 从本地缓存弹出一个页。
 *
 * @param c 缓存下标
 * @returns 物理页地址；缓存与位图都无空闲页时返回 0
 *
 * @warning
 * 调用者必须已关中断，以保证本 CPU 缓存的独占访问
 */
static uint32_t pcpu_pop(uint32_t c) {
    if (pcpu_cache_count[c] == 0) {
        pcpu_refill(c);
        if (pcpu_cache_count[c] == 0) {
            return 0;
        }
    }
    uint32_t phy = pcpu_page_cache[c][--pcpu_cache_count[c]];
    cpu_xadd32(&pool_free_pages, (uint32_t)-1);
    return phy;
}

/**
 * 把页压入本地缓存。
 *
 * @param c   缓存下标
 * @param phy 页物理地址
 * @returns 1 表示已入缓存；0 表示缓存已满，由调用者负责归还位图
 *
 * @warning
 * 调用者必须已关中断，以保证本 CPU 缓存的独占访问
 */
static int pcpu_push(uint32_t c, uint32_t phy) {
    if (pcpu_cache_count[c] >= PCP_CACHE_MAX) {
        return 0;
    }
    pcpu_page_cache[c][pcpu_cache_count[c]++] = phy;
    cpu_xadd32(&pool_free_pages, 1);
    return 1;
}

/**
 * 分配单个物理页。
 *
 * @returns 物理页地址（页已从空闲计数中扣除）；无空闲页返回 0
 *
 * @remarks
 * 优先走本 CPU 缓存，缓存补充失败时退回 pool_lock + 位图
 */
static uint32_t pool_alloc_page(void) {
    uint32_t c;
    if (pcpu_cache_cpu(&c)) {
        uint32_t old = asm_save_eflags();
        asm_cli();
        uint32_t phy = pcpu_pop(c);
        asm_restore_eflags(old);
        if (phy != 0) {
            return phy;
        }
    }
    lock_acquire(&pool_lock);
    uint32_t phy = palloc_raw(&kernel_pool);
    if (phy != 0) {
        cpu_xadd32(&pool_free_pages, (uint32_t)-1);
    }
    lock_release(&pool_lock);
    if (phy != 0) {
    }
    return phy;
}

/**
 * 归还单个物理页。
 *
 * @param phy_addr 页物理地址，必须页对齐
 *
 * @remarks
 * 优先压回本 CPU 缓存；缓存已满或不可用时归还位图。地址低于池起点时忽略
 */
static void fr_selftest(int cmode, uint32_t c) {
    int fi = bitmap_scan(&kernel_pool.pool_bitmap, 1);
    if (fi == -1) {
        return;
    }
    uint32_t fphy = kernel_pool.phy_addr_start + (uint32_t)fi * PAGE_SIZE;
    kprintf("[fr-arm] selftest phy=%x\n", fphy);
    lock_acquire(&pool_lock);
    pfree_raw(&kernel_pool, fphy);
    lock_release(&pool_lock);
    if (cmode) {
        uint32_t old = asm_save_eflags();
        asm_cli();
        uint32_t save = pcpu_cache_count[c];
        int p1 = pcpu_push(c, fphy);
        int p2 = pcpu_push(c, fphy);
        pcpu_cache_count[c] = save;
        asm_restore_eflags(old);
        cpu_xadd32(&pool_free_pages, (uint32_t)(0u - (uint32_t)(p1 + p2)));
    }
    kprintf("[fr-arm] selftest end\n");
}

static void pool_free_page(uint32_t phy_addr) {
    if (phy_addr < kernel_pool.phy_addr_start) {
        return;
    }
    uint32_t c;
    int cmode = pcpu_cache_cpu(&c);
    if (fr_arm) {
        fr_arm = 0;
        fr_selftest(cmode, c);
    }
    if (cmode) {
        uint32_t old = asm_save_eflags();
        asm_cli();
        int ok = pcpu_push(c, phy_addr);
        asm_restore_eflags(old);
        if (ok) {
            return;
        }
    }
    lock_acquire(&pool_lock);
    pfree_raw(&kernel_pool, phy_addr);
    cpu_xadd32(&pool_free_pages, 1);
    lock_release(&pool_lock);
}

/**
 * 分配一个页表结构页（PT / PD / PDPT），并同步空闲页计数。
 *
 * @returns 物理页地址；无空闲页返回 0
 *
 * @warning
 * 调用者必须持有 pool_lock
 */
static uint32_t palloc_pt_raw(void) {
    uint32_t pa = palloc_raw(&kernel_pool);
    if (pa != 0) {
        cpu_xadd32(&pool_free_pages, (uint32_t)-1);
    }
    return pa;
}

static uint64_t cur_pml4(void) {
    if (current && current->pml4_phys) {
        return (uint64_t)current->pml4_phys;
    }
    return asm_read_cr3();
}

static uint64_t *pte_query(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = (uint64_t *)VIRT_OF(pml4_phys);
    uint64_t e = pml4[PML4_INDEX(vaddr)];
    if (!(e & 1))
        return 0;
    uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(e));
    e = pdp[PDPT_INDEX(vaddr)];
    if (!(e & 1))
        return 0;
    uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(e));
    e = pd[PD_INDEX(vaddr)];
    if (!(e & 1))
        return 0;
    uint64_t *pt = (uint64_t *)VIRT_OF(PTE_PHYS(e));
    return &pt[PT_INDEX(vaddr)];
}

static uint64_t *pte_make(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pml4 = (uint64_t *)VIRT_OF(pml4_phys);
    uint32_t idx = PML4_INDEX(vaddr);
    if (!(pml4[idx] & 1)) {
        uint32_t pa = palloc_pt_raw();
        if (pa == 0)
            return 0;
        pml4[idx] = pa | PTE_P | PTE_W | PTE_U;
        memset((void *)VIRT_OF(pa), 0, PAGE_SIZE);
    }
    uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(pml4[idx]));
    idx = PDPT_INDEX(vaddr);
    if (!(pdp[idx] & 1)) {
        uint32_t pa = palloc_pt_raw();
        if (pa == 0)
            return 0;
        pdp[idx] = pa | PTE_P | PTE_W | PTE_U;
        memset((void *)VIRT_OF(pa), 0, PAGE_SIZE);
    }
    uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(pdp[idx]));
    idx = PD_INDEX(vaddr);
    if (!(pd[idx] & 1)) {
        uint32_t pa = palloc_pt_raw();
        if (pa == 0)
            return 0;
        pd[idx] = pa | PTE_P | PTE_W | PTE_U;
        memset((void *)VIRT_OF(pa), 0, PAGE_SIZE);
    }
    uint64_t *pt = (uint64_t *)VIRT_OF(PTE_PHYS(pd[idx]));
    return &pt[PT_INDEX(vaddr)];
}

static uint64_t pte_zero;

uint64_t *pde_ptr(uint32_t vaddr) {
    uint64_t *pml4 = (uint64_t *)VIRT_OF(cur_pml4());
    uint64_t e = pml4[PML4_INDEX(vaddr)];
    if (!(e & 1))
        return NULL;
    uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(e));
    e = pdp[PDPT_INDEX(vaddr)];
    if (!(e & 1))
        return NULL;
    uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(e));
    return &pd[PD_INDEX(vaddr)];
}

uint64_t *pte_ptr(uint32_t vaddr) {
    uint64_t *pte = pte_query(cur_pml4(), (uint64_t)vaddr);
    return pte ? pte : &pte_zero;
}

void page_table_dump(uint32_t vaddr) {
    uint64_t pml4_phys = cur_pml4();
    uint64_t *pml4 = (uint64_t *)VIRT_OF(pml4_phys);
    uint64_t e0 = pml4[PML4_INDEX(vaddr)];
    uint64_t e1 = 0, e2 = 0, e3 = 0;
    if (e0 & 1) {
        uint64_t *pdp = (uint64_t *)VIRT_OF(PTE_PHYS(e0));
        e1 = pdp[PDPT_INDEX(vaddr)];
        if (e1 & 1) {
            uint64_t *pd = (uint64_t *)VIRT_OF(PTE_PHYS(e1));
            e2 = pd[PD_INDEX(vaddr)];
            if ((e2 & 1) && !(e2 & (1ull << 7))) {
                uint64_t *pt = (uint64_t *)VIRT_OF(PTE_PHYS(e2));
                e3 = pt[PT_INDEX(vaddr)];
            }
        }
    }

    kprintf("  [pgtbl] nx_usable=%d efer=0x%x\n", g_nx_usable,
            (uint32_t)asm_rdmsr(EFER_MSR));
    kprintf("  [pgtbl] cr3=0x%x vaddr=0x%x\n", (uint32_t)pml4_phys, vaddr);
    kprintf("  [pgtbl] PML4[%d]=%x%x\n", (int)PML4_INDEX(vaddr),
            (uint32_t)(e0 >> 32), (uint32_t)e0);
    kprintf("  [pgtbl] PDPT[%d]=%x%x\n", (int)PDPT_INDEX(vaddr),
            (uint32_t)(e1 >> 32), (uint32_t)e1);
    kprintf("  [pgtbl] PD[%d]=%x%x PS=%d\n", (int)PD_INDEX(vaddr),
            (uint32_t)(e2 >> 32), (uint32_t)e2, (int)((e2 >> 7) & 1));
    kprintf("  [pgtbl] PT[%d]=%x%x (P=%d W=%d U=%d PCD=%d PAT=%d G=%d "
            "NX=%d phys=%#x)\n",
            (int)PT_INDEX(vaddr), (uint32_t)(e3 >> 32), (uint32_t)e3,
            (int)(e3 & 1),
            (int)((e3 >> 1) & 1), (int)((e3 >> 2) & 1), (int)((e3 >> 4) & 1),
            (int)((e3 >> 7) & 1),             (int)((e3 >> 8) & 1), (int)((e3 >> 63) & 1),
            (uint32_t)(e3 & 0x000ffffffffff000ull));
    if (pml4_phys != kernel_pml4) {
        uint64_t *kpml4 = (uint64_t *)VIRT_OF(kernel_pml4);
        uint64_t ke0 = kpml4[PML4_INDEX(vaddr)];
        uint64_t ke1 = 0, ke2 = 0, ke3 = 0;
        if (ke0 & 1) {
            uint64_t *kpdp = (uint64_t *)VIRT_OF(PTE_PHYS(ke0));
            ke1 = kpdp[PDPT_INDEX(vaddr)];
            if (ke1 & 1) {
                uint64_t *kpd = (uint64_t *)VIRT_OF(PTE_PHYS(ke1));
                ke2 = kpd[PD_INDEX(vaddr)];
                if ((ke2 & 1) && !(ke2 & (1ull << 7))) {
                    uint64_t *kpt = (uint64_t *)VIRT_OF(PTE_PHYS(ke2));
                    ke3 = kpt[PT_INDEX(vaddr)];
                }
            }
        }
        kprintf("  [pgtbl] kernel PML4=%x: L1=%x L2=%x L3=%x L4=%x\n",
                (uint32_t)kernel_pml4, (uint32_t)ke0, (uint32_t)ke1,
                (uint32_t)ke2, (uint32_t)ke3);
    }
}

static int page_table_add_raw(uint32_t vaddr, uint32_t phy_addr) {
    uint64_t *pte = pte_make(cur_pml4(), (uint64_t)vaddr);
    if (pte == 0)
        return -1;
    *pte = (uint64_t)phy_addr | pte_wx(PTE_P | PTE_U, 1, 0);
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 0;
}

static void page_table_add_no_cache(uint32_t vaddr, uint32_t phy_addr) {
    uint64_t *pte = pte_make(kernel_pml4, (uint64_t)vaddr);
    if (pte == 0) {
        kprintf("[ptadd] pte_make FAILED vaddr=%x\n", vaddr);
        return;
    }
    *pte = (uint64_t)phy_addr | pte_wx(PTE_P | 0x10, 1, 0);
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)VIRT_OF(phys);
}

void *ioremap(uint32_t phy_addr, uint32_t size) {
    uint32_t phy = phy_addr & ~0xfff;
    uint32_t cnt = (phy_addr + size - 1) / PAGE_SIZE - phy / PAGE_SIZE + 1;
    lock_acquire(&map_lock);
    int bit = bitmap_scan(&kernel_vaddr.vaddr_bitmap, cnt);
    if (bit == -1) {
        lock_release(&map_lock);
        return 0;
    }
    uint32_t vaddr = kernel_vaddr.vaddr_start + (uint32_t)bit * PAGE_SIZE;

    lock_acquire(&pool_lock);
    for (uint32_t i = 0; i < cnt; i++) {
        bitmap_set(&kernel_vaddr.vaddr_bitmap, (uint32_t)bit + i, 1);
        page_table_add_no_cache(vaddr + i * PAGE_SIZE, phy + i * PAGE_SIZE);
    }
    lock_release(&pool_lock);
    lock_release(&map_lock);
    return (void *)(vaddr + (phy_addr & 0xfff));
}

void *get_a_page(uint32_t vaddr) {
    struct TASK *cur = current;
    uint32_t bit_idx = (vaddr - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
    if (bit_idx >= cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
        if (vaddr >= 0xbf000000u) {
            kprintf("[oob-map] get_a_page pid=%d va=%x bit=%d/%d\n", cur->pid,
                    vaddr, (int)bit_idx,
                    (int)(cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8));
        }
        return 0;
    }
    lock_acquire(&map_lock);
    {
        uint64_t *op = pte_ptr(vaddr);
        if (op != 0 && (*op & 1)) {
            kprintf("[clobber-map] get_a_page pid=%d va=%x oldpte=%x bit=%d\n",
                    cur->pid, vaddr, (uint32_t)*op,
                    (int)bitmap_scan_test(&cur->userprog_v_addr.vaddr_bitmap,
                                          bit_idx));
        }
    }
    if (bitmap_scan_test(&cur->userprog_v_addr.vaddr_bitmap, bit_idx) == 1) {
        lock_release(&map_lock);
        return 0;
    }
    bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bit_idx, 1);
    uint32_t phy = pool_alloc_page();
    if (phy == 0) {
        bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bit_idx, 0);
        lock_release(&map_lock);
        return 0;
    }
    lock_acquire(&pool_lock);
    int rc = page_table_add_raw(vaddr, phy);
    lock_release(&pool_lock);
    if (rc != 0) {
        bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bit_idx, 0);
        lock_release(&map_lock);
        pool_free_page(phy);
        return 0;
    }
    memset((void *)vaddr, 0, PAGE_SIZE);
    fr_va_map(vaddr, phy, "get_a_page");
    lock_release(&map_lock);
    return (void *)vaddr;
}

uint32_t vaddr_reserve_run(uint32_t pages) {
    struct TASK *cur = current;
    uint32_t start = cur->userprog_v_addr.vaddr_start;
    uint32_t limit = USER_LOW_CEILING;
    uint32_t total = (limit - start) / PAGE_SIZE;
    uint32_t slots = cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8;
    if (total > slots)
        total = slots;
    if (pages == 0 || pages > total)
        return 0;
    lock_acquire(&map_lock);
    uint32_t offset = rand_u32() % total;
    uint32_t run = 0;
    uint32_t last = 0;
    uint32_t base = 0;
    for (uint32_t i = 0; i < total && base == 0; i++) {
        uint32_t idx = (offset + total - 1 - i) % total;
        uint32_t v = start + idx * PAGE_SIZE;
        if (bitmap_scan_test(&cur->userprog_v_addr.vaddr_bitmap, idx) ||
            page_is_mapped(v) || (run != 0 && v + PAGE_SIZE != last)) {
            run = 0;
            continue;
        }
        last = v;
        if (++run == pages)
            base = v;
    }
    if (base != 0) {
        uint32_t bidx = (base - start) / PAGE_SIZE;
        for (uint32_t i = 0; i < pages; i++)
            bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bidx + i, 1);
    }
    lock_release(&map_lock);
    return base;
}

int vaddr_reserve_at(uint32_t base, uint32_t pages) {
    struct TASK *cur = current;
    if (base < cur->userprog_v_addr.vaddr_start)
        return -1;
    uint32_t bidx0 = (base - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
    lock_acquire(&map_lock);
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t idx = bidx0 + i;
        if (idx >= cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8 ||
            bitmap_scan_test(&cur->userprog_v_addr.vaddr_bitmap, idx)) {
            for (uint32_t j = 0; j < i; j++)
                bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bidx0 + j, 0);
            lock_release(&map_lock);
            return -1;
        }
        bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, idx, 1);
    }
    lock_release(&map_lock);
    return 0;
}

int ensure_user_page(uint32_t vaddr) {
    uint64_t *pte = pte_ptr(vaddr);
    if (pte != NULL && (*pte & 1))
        return 0;
    return get_a_page(vaddr & ~0xfffu) != 0 ? 0 : -1;
}

uint32_t user_remap_page(uint32_t vaddr) {
    struct TASK *cur = current;
    lock_acquire(&map_lock);
    uint64_t *pte = pte_query(cur_pml4(), (uint64_t)vaddr);
    if (pte == NULL || !(*pte & 1)) {
        lock_release(&map_lock);
        return 0;
    }
    uint64_t old = *pte;
    uint32_t oldphy = (uint32_t)(old & 0xfffff000ull);
    uint32_t phy = pool_alloc_page();
    if (phy == 0) {
        lock_release(&map_lock);
        return 0;
    }
    memcpy((void *)(uintptr_t)VIRT_OF(phy), (void *)(uintptr_t)VIRT_OF(oldphy),
           PAGE_SIZE);
    *pte = ((uint64_t)phy) | (old & 0xfffull);
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    uint32_t bit_idx = (vaddr - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
    if (bit_idx < cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
        bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bit_idx, 1);
    }
    fr_va_map(vaddr, phy, "remap");
    lock_release(&map_lock);
    return phy;
}

void vaddr_unreserve(uint32_t base, uint32_t pages) {
    struct TASK *cur = current;
    if (base < cur->userprog_v_addr.vaddr_start)
        return;
    uint32_t bidx0 = (base - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
    lock_acquire(&map_lock);
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t idx = bidx0 + i;
        if (idx < cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8)
            bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, idx, 0);
    }
    lock_release(&map_lock);
}

void *map_reserved_page(uint32_t vaddr) {
    struct TASK *cur = current;
    uint32_t bit_idx = (vaddr - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
    if (bit_idx >= cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
        return 0;
    }
    lock_acquire(&map_lock);
    {
        uint64_t *op = pte_ptr(vaddr);
        if (op != 0 && (*op & 1)) {
            kprintf("[clobber-map] map_reserved pid=%d va=%x oldpte=%x\n",
                    cur->pid, vaddr, (uint32_t)*op);
        }
    }
    uint32_t phy = pool_alloc_page();
    if (phy == 0) {
        lock_release(&map_lock);
        return 0;
    }
    lock_acquire(&pool_lock);
    int rc = page_table_add_raw(vaddr, phy);
    lock_release(&pool_lock);
    if (rc != 0) {
        lock_release(&map_lock);
        pool_free_page(phy);
        return 0;
    }
    memset((void *)vaddr, 0, PAGE_SIZE);
    fr_va_map(vaddr, phy, "map_res");
    lock_release(&map_lock);
    return (void *)vaddr;
}

void *get_kernel_pages(uint32_t pg_cnt) {
    uint32_t phy = palloc_pages(&kernel_pool, pg_cnt);
    if (phy == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < pg_cnt; i++) {
        memset((void *)(VIRT_OF(phy) + i * PAGE_SIZE), 0, PAGE_SIZE);
    }
    return (void *)(uintptr_t)VIRT_OF(phy);
}

void *palloc(struct MM_POOL *pool) {
    if (pool == &kernel_pool) {
        uint32_t phy = pool_alloc_page();
        return phy ? (void *)(uintptr_t)phy : 0;
    }
    lock_acquire(&pool_lock);
    void *r = (void *)palloc_raw(pool);
    lock_release(&pool_lock);
    return r;
}

uint32_t kernel_pool_free_count(void) {
    return cpu_atomic_load32(&pool_free_pages);
}

void pfree(struct MM_POOL *pool, uint32_t phy_addr) {
    if (pool == &kernel_pool) {
        pool_free_page(phy_addr);
        return;
    }
    lock_acquire(&pool_lock);
    pfree_raw(pool, phy_addr);
    lock_release(&pool_lock);
}

uint32_t palloc_pages(struct MM_POOL *pool, uint32_t cnt) {
    lock_acquire(&pool_lock);
    uint32_t r = palloc_pages_raw(pool, cnt);
    lock_release(&pool_lock);
    if (r != 0 && pool == &kernel_pool) {
        cpu_xadd32(&pool_free_pages, (uint32_t)(0u - cnt));
    }
    return r;
}

void free_kernel_page(uint32_t vaddr) {
    pool_free_page(PHY_OF(vaddr));
}

void free_user_page(uint32_t vaddr) {
    struct TASK *cur = current;
    lock_acquire(&map_lock);
    uint64_t *pte = pte_ptr(vaddr);
    if (*pte & 1) {
        uint32_t phy = (uint32_t)(*pte & 0xfffff000ull);
        *pte = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
        uint32_t bit_idx =
            (vaddr - cur->userprog_v_addr.vaddr_start) / PAGE_SIZE;
        if (bit_idx < cur->userprog_v_addr.vaddr_bitmap.btmp_bytes_len * 8) {
            bitmap_set(&cur->userprog_v_addr.vaddr_bitmap, bit_idx, 0);
        }
        lock_release(&map_lock);
        page_free_or_decref_va(phy, vaddr);
        return;
    }
    lock_release(&map_lock);
}

void page_cow_share(uint32_t phy_addr) {
    if (phy_addr < MEMORY_BASE || phy_addr >= MAX_PHYS_MEM) {
        return;
    }
    uint32_t idx = FRAME_IDX(phy_addr);
    lock_acquire(&map_lock);
    frame_owner[idx] = (frame_owner[idx] == 0) ? 2 : (uint8_t)(frame_owner[idx] + 1);
    lock_release(&map_lock);
}

int page_cow_resolve(uint32_t vaddr, uint64_t pte_val) {
    uint32_t phy = (uint32_t)PTE_PHYS(pte_val);
    if (phy < MEMORY_BASE || phy >= MAX_PHYS_MEM) {
        return 0;
    }
    uint32_t idx = FRAME_IDX(phy);
    lock_acquire(&map_lock);
    uint64_t *pte = pte_ptr(vaddr);
    if (pte == NULL || !(*pte & 1)) {
        lock_release(&map_lock);
        return 0;
    }
    if (!(*pte & COW_FLAG)) {
        uint32_t ok = (*pte & PTE_W) ? 1 : 0;
        lock_release(&map_lock);
        return ok;
    }
    if (frame_owner[idx] > 1) {
        uint32_t new_phy = pool_alloc_page();
        if (new_phy == 0) {
            lock_release(&map_lock);
            return 0;
        }
        memcpy((void *)VIRT_OF(new_phy), (void *)VIRT_OF(phy), PAGE_SIZE);
        frame_owner[idx]--;
        *pte = (uint64_t)new_phy |
               (pte_val & (PTE_P | PTE_U | PTE_NX | 0x0f0)) | PTE_W;
        fr_va_map(vaddr & ~0xfffu, new_phy, "cow_res");
    } else {
        *pte = (*pte & ~(uint64_t)COW_FLAG) | PTE_W;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    lock_release(&map_lock);
    return 1;
}

void page_free_or_decref(uint32_t phy_addr) {
    if (phy_addr < MEMORY_BASE || phy_addr >= MAX_PHYS_MEM) {
        return;
    }
    uint32_t idx = FRAME_IDX(phy_addr);
    lock_acquire(&map_lock);
    uint8_t owner = frame_owner[idx];
    if (owner > 1) {
        frame_owner[idx] = (uint8_t)(owner - 1);
        lock_release(&map_lock);
        return;
    }
    frame_owner[idx] = 0;
    lock_release(&map_lock);
    pfree(&kernel_pool, phy_addr);
}

void page_free_or_decref_va(uint32_t phy_addr, uint32_t vaddr) {
    fr_va_unmap(vaddr, phy_addr);
    page_free_or_decref(phy_addr);
}
