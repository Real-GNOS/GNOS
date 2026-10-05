/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmm.c — per-process virtual address spaces. (GPLv2)
 */
#include <stddef.h>
#include <stdint.h>

#include "vmm.h"
#include "tmpfs.h"
#include "pmm.h"
#include "panic.h"
#include "kstring.h"
#include "debugcon.h"
#include "sysnum.h" /* PROT_READ/WRITE/EXEC for vmm_protect */
#include "cgroup.h"
#include "smp.h" /* spinlock_t guarding the frame refcounts */

#define PTE_P   0x001
#define PTE_RW  0x002
#define PTE_U   0x004
#define PTE_PWT 0x008 /* write-through */
#define PTE_PCD 0x010 /* cache-disable (set => uncacheable) */
#define PTE_PS  0x080
#define PTE_AVL (1ULL << 9) /* available-to-software: SysV shm marker */
/* Available-to-software: the page is shared after fork() and nobody may
 * write to it without breaking the copy first.  See the invariants listed
 * above vmm_cow_break() -- the three together are what make the sharing
 * safe, and every site that touches a leaf PTE has to preserve them. */
#define PTE_COW  (1ULL << 10)
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

/* One static address space object per possible process; the kernel has no
 * heap, and a fixed table keeps allocation honest. */
#define MAX_ADDRSPACES 32
static addrspace_t g_spaces[MAX_ADDRSPACES];
static int         g_used[MAX_ADDRSPACES];

/* ---- physical-frame refcounts -------------------------------------------
 * Two very different things need "how many page-table entries point at this
 * frame?" and they share this one table:
 *
 *   mmap(MAP_SHARED|MAP_ANONYMOUS) hands every mapper the SAME frames, or
 *   POSIX shared memory (and the futexes people park on it) silently becomes
 *   per-process private memory.  Frames reached that way are marked PTE_AVL
 *   so unmap and teardown stop freeing them.
 *
 *   fork() shares every private writable page instead of copying it (copy-on-
 *   write): parent and child both point at the frame read-only, and whoever
 *   writes first takes the copy.  Those frames are marked PTE_COW.
 *
 * In both cases the table owns the frame's lifetime and the last PTE that
 * goes away frees it.
 *
 * The count is per_ENTRY_, not per address space: a frame mapped twice
 * inside one address space (aliasing) is referenced twice, and vmm_clone()
 * therefore increments once per PTE it shares.  That is what keeps an alias
 * from being freed while a second mapping still uses it.
 *
 * Locking: the rest of this file is serialised by the big kernel lock, but a
 * page fault taken in ring 0 (a kernel-internal write to a user buffer)
 * reaches here without the BKL held, so the table has its own lock. */
#define FRAME_SLOTS 65536
#define SLOT_FREE   0
#define SLOT_USED   1
#define SLOT_TOMB   2 /* deleted: probe past it, but reuse it */

typedef struct {
    uint64_t frame;
    uint32_t refs;
    int      used;
} frameref_t;
static frameref_t g_shm[FRAME_SLOTS];
static spinlock_t g_shm_lock;
static uint32_t   g_frame_slots_used; /* how many entries are SLOT_USED */

static unsigned shm_hash(uint64_t frame)
{
    uint64_t h = (frame >> 12) * 0x9E3779B97F4A7C15ULL;
    return (unsigned)((h >> 32) & (FRAME_SLOTS - 1));
}

/* Caller holds g_shm_lock.  A tombstone is not a stop: entries are inserted
 * by linear probing too, so deleting one out of the middle would otherwise
 * make everything after it unreachable. */
static frameref_t *shm_find_locked(uint64_t frame)
{
    unsigned i = shm_hash(frame);
    for (unsigned n = 0; n < FRAME_SLOTS; n++) {
        frameref_t *s = &g_shm[(i + n) & (FRAME_SLOTS - 1)];
        if (s->used == SLOT_FREE)
            return NULL;
        if (s->used == SLOT_USED && s->frame == frame)
            return s;
    }
    return NULL;
}

/* Record a frame as shared with one owner.  1 = registered now,
 * 0 = it was already tracked, -1 = table full (caller must fail). */
int vmm_share_frame(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s = shm_find_locked(frame);
    if (s) {
        spin_unlock_irq(&g_shm_lock);
        return 0; /* already tracked */
    }
    unsigned    i         = shm_hash(frame);
    frameref_t *free_slot = NULL;
    for (unsigned n = 0; n < FRAME_SLOTS; n++) {
        frameref_t *c = &g_shm[(i + n) & (FRAME_SLOTS - 1)];
        if (c->used != SLOT_USED) {
            free_slot = c;
            break; /* tombstone is fine to reuse */
        }
    }
    int rc = -1;
    if (free_slot) {
        free_slot->frame = frame;
        free_slot->refs  = 1;
        free_slot->used  = SLOT_USED;
        g_frame_slots_used++;
        rc = 1;
    }
    spin_unlock_irq(&g_shm_lock);
    return rc;
}

int vmm_share_ref(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s = shm_find_locked(frame);
    if (s)
        s->refs++;
    spin_unlock_irq(&g_shm_lock);
    return s ? 0 : -1;
}

/* Drop one mapper; the last one frees the frame. */
int vmm_share_unref(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s = shm_find_locked(frame);
    if (!s) {
        spin_unlock_irq(&g_shm_lock);
        return -1;
    }
    int freed = 0;
    if (--s->refs == 0) {
        s->used  = SLOT_TOMB;
        s->frame = 0;
        g_frame_slots_used--;
        freed = 1;
    }
    spin_unlock_irq(&g_shm_lock);
    if (freed)
        pmm_free(frame); /* outside the lock: pmm may be slow */
    return 0;
}

/* Add one reference to `frame`, registering it first if this is the first
 * PTE ever to point at it.  0 on success, -1 if the table is full. */
int vmm_frame_ref_inc(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s = shm_find_locked(frame);
    if (s) {
        s->refs++;
        spin_unlock_irq(&g_shm_lock);
        return 0;
    }
    unsigned    i         = shm_hash(frame);
    frameref_t *free_slot = NULL;
    for (unsigned n = 0; n < FRAME_SLOTS; n++) {
        frameref_t *c = &g_shm[(i + n) & (FRAME_SLOTS - 1)];
        if (c->used != SLOT_USED) {
            free_slot = c;
            break;
        }
    }
    if (!free_slot) {
        spin_unlock_irq(&g_shm_lock);
        return -1;
    }
    free_slot->frame = frame;
    free_slot->refs  = 1;
    free_slot->used  = SLOT_USED;
    g_frame_slots_used++;
    spin_unlock_irq(&g_shm_lock);
    return 0;
}

/* Current reference count, or -1 if the frame is not tracked at all (which
 * means exactly one private owner). */
int vmm_frame_ref_count(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s    = shm_find_locked(frame);
    int         refs = s ? (int)s->refs : -1;
    spin_unlock_irq(&g_shm_lock);
    return refs;
}

uint32_t vmm_frame_ref_slots(void)
{
    return g_frame_slots_used;
}

static uint64_t g_kernel_pml4_phys;
static int      g_watchmap_count; /* TEMPORARY Xorg debugging */

static uint64_t *table(uint64_t phys)
{
    return (uint64_t *)pmm_virt(phys);
}

void vmm_init(void)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_pml4_phys = cr3 & PTE_ADDR;

    /* Enable the No-Execute bit (EFER.NXE).  Without it the CPU treats PTE
     * bit 63 as a reserved bit, so every non-executable mapping -- the kernel
     * BSS, every user stack and anonymous mmap -- faults the moment it is
     * touched.  This must happen before any such mapping is created. */
    uint32_t e_lo, e_hi;
    asm volatile("rdmsr" : "=a"(e_lo), "=d"(e_hi) : "c"(0xC0000080));
    e_lo |= (1u << 11); /* EFER.NXE */
    asm volatile("wrmsr" : : "c"(0xC0000080), "a"(e_lo), "d"(e_hi));

    /* Enable FPU/SSE state management.  CR4.OSFXSR lets user programs execute
     * SSE/SSE2/SSE3 instructions (without it every one of them raises #UD,
     * vector 6 -- this is exactly what was killing musl's libc) and lets the
     * kernel use FXSAVE/FXRSTOR to preserve each process's XMM registers
     * across a context switch.  OSXSAVE (guarded by CPUID) is harmless here
     * and leaves the door open for AVX later; writing it on a CPU that lacks
     * it is simply ignored, so we only set it when supported. */
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9); /* CR4.OSFXSR */
    {
        uint32_t a, b, c, d;
        asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
        if (c & (1u << 27))      /* CPUID.01H:ECX.OSXSAVE */
            cr4 |= (1ULL << 18); /* CR4.OSXSAVE */
    }
    asm volatile("mov %0, %%cr4" ::"r"(cr4));

    /* CR0.WP is what makes a supervisor write obey the read-only bit of a
     * *user* PTE.  Without it every kernel copy straight into a fork() page
     * (a read(2) into the caller's buffer, futex's own writes to the word it
     * queues on) would sail through without a fault and quietly modify the
     * frame the parent still has.  Limine happens to leave it set, but this
     * is load-bearing enough not to be left to chance. */
    {
        uint64_t cr0;
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        if (!(cr0 & (1ULL << 16))) {
            dbg_puts("VMM: CR0.WP was clear, enabling it\r\n");
            asm volatile("mov %0, %%cr0" ::"r"(cr0 | (1ULL << 16)) : "memory");
        }
    }

    dbg_puts("VMM: kernel PML4 @");
    dbg_puts_hex(g_kernel_pml4_phys);
    dbg_puts("\r\n");
}

/*
 * Some bootloaders map only the file-backed portion of the kernel image and
 * leave the zero-initialised BSS unmapped.  The first time the kernel writes
 * a global in that region it therefore takes a page fault.  Rather than trust
 * the loader, map whatever BSS pages are not already present ourselves, reusing
 * the live kernel PML4 (g_kernel_pml4_phys) so the change is visible at once.
 *
 * The BSS bounds come straight from the linker symbols, whose addresses are
 * already relocated to their runtime (higher-half) values by the loader, so no
 * load-bias arithmetic is needed here.
 */
static uint64_t *walk(addrspace_t *as, uint64_t vaddr, int create, unsigned flags);
void             vmm_map_kernel_bss(void)
{
    extern char _bss_start[];
    extern char _kernel_end[];

    uint64_t start = (uint64_t)(uintptr_t)_bss_start;
    uint64_t end   = (uint64_t)(uintptr_t)_kernel_end;

    dbg_puts("VMM: bss map range [");
    dbg_puts_hex(start);
    dbg_puts(",");
    dbg_puts_hex(end);
    dbg_puts(")\r\n");

    /* Same physical PML4 the running kernel uses; mapping through it means the
     * new PTEs are live immediately. */
    addrspace_t k    = {.pml4_phys = g_kernel_pml4_phys};
    uint64_t    base = start & ~0xFFFULL;
    for (uint64_t va = base; va < end; va += PAGE_SIZE) {
        uint64_t *pte      = walk(&k, va, 0, 0); /* no allocate, just inspect */
        int       present  = pte && (*pte & PTE_P);
        int       writable = present && (*pte & PTE_RW);

        if (present && writable)
            continue; /* loader already gave us a good page */

        if (present) {
            /* Loader mapped it read-only; just flip the write bit. */
            *pte |= PTE_RW;
        } else {
            uint64_t frame = pmm_alloc_zeroed();
            if (!frame) {
                dbg_puts("VMM: bss OOM at ");
                dbg_puts_hex(va);
                dbg_puts("\r\n");
                panic("vmm: failed to map kernel BSS (OOM)");
            }
            if (!vmm_map(&k, va, frame, VM_WRITE)) {
                dbg_puts("VMM: bss map fail at ");
                dbg_puts_hex(va);
                dbg_puts("\r\n");
                panic("vmm: failed to map kernel BSS (map)");
            }
        }
        asm volatile("invlpg (%0)" ::"r"(va) : "memory");
    }

    /* Guard page at _kernel_end: the loop above stops at the last byte of
     * the image, so the page the end symbol lands on is not mapped.  Some
     * arenas are sized by symbols rather than by their own length, and a
     * BSS that grows (a big static pool, say) can move _kernel_end onto a
     * page boundary that then gets touched.  One extra page costs nothing
     * and removes that whole class of off-by-one. */
    {
        uint64_t  guard = end & ~0xFFFULL;
        uint64_t *gpte  = walk(&k, guard, 0, 0);
        if (!gpte || !(*gpte & PTE_P)) {
            uint64_t gframe = pmm_alloc_zeroed();
            if (gframe)
                vmm_map(&k, guard, gframe, VM_WRITE);
        }
    }

    dbg_puts("VMM: kernel BSS mapped OK\r\n");
}

addrspace_t *vmm_create(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_ADDRSPACES; i++) {
        if (!g_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return NULL;

    uint64_t pml4 = pmm_alloc_zeroed();
    if (!pml4)
        return NULL;

    /* Share the kernel half; leave the user half empty. */
    uint64_t *dst = table(pml4);
    uint64_t *src = table(g_kernel_pml4_phys);
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    g_used[slot]             = 1;
    g_spaces[slot].pml4_phys = pml4;
    g_spaces[slot].refs      = 1;
    g_spaces[slot].pages     = 0;
    g_spaces[slot].cg        = -1;
    g_spaces[slot].nmmaps    = 0;
    return &g_spaces[slot];
}

static uint64_t pte_flags(unsigned flags)
{
    uint64_t f = PTE_P;
    if (flags & VM_WRITE)
        f |= PTE_RW;
    if (flags & VM_USER)
        f |= PTE_U;
    if (!(flags & VM_EXEC))
        f |= PTE_NX;
    return f;
}

/* Walk to the PTE for `vaddr`, allocating tables when `create` is set. */
static uint64_t *walk(addrspace_t *as, uint64_t vaddr, int create, unsigned flags)
{
    uint64_t *cur = table(as->pml4_phys);

    for (int level = 4; level > 1; level--) {
        unsigned idx = (unsigned)((vaddr >> (12 + 9 * (level - 1))) & 0x1FF);

        if (!(cur[idx] & PTE_P)) {
            if (!create)
                return NULL;
            uint64_t next = pmm_alloc_zeroed();
            if (!next)
                return NULL;
            cur[idx] = next | PTE_P | PTE_RW;
        }
        /* Intermediate levels must permit whatever the leaf permits. */
        if (create) {
            cur[idx] |= PTE_RW;
            if (flags & VM_USER)
                cur[idx] |= PTE_U;
            if (flags & VM_EXEC)
                cur[idx] &= ~PTE_NX;
        }
        if (cur[idx] & PTE_PS) {
            /* A large page sits where the walk needs a table -- the KASLR
             * relocator aliases the kernel image with 2 MiB entries, and a
             * 1 GiB entry would come from whoever built the boot tables.
             * Split it one level down, preserving the mapping exactly: the
             * children cover the same physical range with the same
             * permissions, so nothing observable changes except that the
             * walk can now see individual pages. */
            uint64_t large   = cur[idx];
            uint64_t pflags  = large & (PTE_P | PTE_RW | PTE_U | PTE_PWT | PTE_PCD | PTE_NX);
            uint64_t step    = (level == 3) ? (1ULL << 21) : PAGE_SIZE;
            uint64_t leaf_ps = (level == 3) ? PTE_PS : 0;

            uint64_t next = pmm_alloc_zeroed();
            if (!next)
                return NULL;
            uint64_t *child = table(next);
            for (unsigned j = 0; j < 512; j++)
                child[j] = (large & PTE_ADDR) + j * step + pflags + leaf_ps;

            cur[idx] = next | pflags; /* PS deliberately dropped */
        }

        cur = table(cur[idx] & PTE_ADDR);
    }

    return &cur[(vaddr >> 12) & 0x1FF];
}

int vmm_map(addrspace_t *as, uint64_t vaddr, uint64_t paddr, unsigned flags)
{
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 1, flags);
    if (!pte)
        return 0;
    /* TEMPORARY Xorg debugging: a remap of an already-present page is the
     * anomaly we are hunting (normal flow never does this). */
    if ((*pte & PTE_P) && (vaddr >> 44) == 5) {
        dbg_puts("REMAP: va=");
        dbg_puts_hex(vaddr & ~0xFFFULL);
        dbg_puts(" oldpte=");
        dbg_puts_hex(*pte);
        dbg_puts(" newframe=");
        dbg_puts_hex(paddr);
        dbg_puts(" caller=");
        dbg_puts_hex((uint64_t)__builtin_return_address(0));
        dbg_puts("\r\n");
    }
    if ((vaddr >> 44) == 5 && g_watchmap_count < 24) {
        g_watchmap_count++;
        dbg_puts("VMMA: va=");
        dbg_puts_hex(vaddr);
        dbg_puts("\r\n");
    }
    uint64_t old = *pte;
    /* Replacing a *counted* mapping (COW fork page, or external shared
     * memory) has to drop the reference it was holding, or the frame leaks
     * along with its table entry.  Two things make this safe to do here and
     * nowhere else:
     *   - the new frame must differ: remapping the same frame at the same
     *     address is an update, not a replacement, and unref'ing it could
     *     free the frame the new PTE is about to install;
     *   - a private (uncounted) frame is deliberately left alone.  Nothing
     *     stops one from being mapped at several addresses, so freeing it
     *     on the strength of this one PTE would be a use-after-free; that
     *     overwriting loses a frame at all is pre-existing behaviour this
     *     change does not try to alter. */
    if ((old & PTE_P) && (old & (PTE_COW | PTE_AVL)) && (old & PTE_ADDR) != (paddr & PTE_ADDR))
        vmm_share_unref(old & PTE_ADDR);

    *pte = (paddr & PTE_ADDR) | pte_flags(flags);
    /* A SysV shared-memory page marks the PTE with the available bit so
     * unmap and address-space teardown clear it without freeing the frame
     * (the segment owns the frame; see VM_EXTSHM). */
    if (flags & VM_EXTSHM)
        *pte |= PTE_AVL;
    return 1;
}

/*
 * mprotect(2) for real: rewrite the permission bits of every present page
 * in [vaddr, vaddr+size).  PROT_NONE leaves the page mapped but strips the
 * read/write/execute bits -- the state musl relies on for a thread stack's
 * guard page, and the state its mmap(PROT_NONE)+mprotect(RW) dance needs
 * reversed for the stack itself.  Pages that are not mapped are skipped
 * (Linux tolerates holes in the range); everything mapped gets invlpg'd.
 */
int vmm_protect(addrspace_t *as, uint64_t vaddr, uint64_t size, unsigned prot)
{
    if (!as || (vaddr & 0xFFF) || (size & 0xFFF))
        return 0;

    unsigned flags = VM_USER;
    if (prot & PROT_WRITE)
        flags |= VM_WRITE;
    if (prot & PROT_EXEC)
        flags |= VM_EXEC;
    if (prot & PROT_READ)
        flags |= VM_READ;
    uint64_t bits = pte_flags(flags);

    dbg_puts("VMM: protect [");
    dbg_puts_hex(vaddr);
    dbg_puts(",");
    dbg_puts_hex(vaddr + size);
    dbg_puts(") prot=");
    dbg_puts_dec(prot);
    dbg_puts("\r\n");

    for (uint64_t va = vaddr; va < vaddr + size; va += PAGE_SIZE) {
        uint64_t *pte = walk(as, va, 0, 0);
        if (!pte || !(*pte & PTE_P))
            continue; /* hole: nothing to protect */
        if (*pte & PTE_PS)
            panic("vmm_protect: large page in a process address space");
        /* Asking for write access to a fork() page means this address space
         * wants its own copy from here on: granting it while the frame is
         * still shared would hand the parent's memory to the child's writes
         * with no fault in between to stop them. */
        if ((bits & PTE_RW) && (*pte & PTE_COW))
            vmm_cow_break(as, va);
        *pte = (*pte & ~(PTE_RW | PTE_NX)) | (bits & (PTE_RW | PTE_NX));
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 1;
}

uint64_t vmm_resolve(addrspace_t *as, uint64_t vaddr)
{
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 0, 0);
    if (!pte || !(*pte & PTE_P))
        return 0;
    return (*pte & PTE_ADDR) + (vaddr & 0xFFF);
}

int vmm_page_is_cow(addrspace_t *as, uint64_t vaddr)
{
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 0, 0);
    return (pte && (*pte & PTE_P) && (*pte & PTE_COW)) ? 1 : 0;
}

/*
 * Take a shared frame out of the refcount table because exactly one PTE is
 * left pointing at it.  It becomes an ordinary private page again: the last
 * unmap frees it directly, and the slot goes back into circulation instead
 * of pinning a table entry for the lifetime of the page.
 */
static void frame_ref_forget_if_last(uint64_t frame)
{
    spin_lock_irq(&g_shm_lock);
    frameref_t *s = shm_find_locked(frame);
    if (s && s->refs == 1) {
        s->used  = SLOT_TOMB;
        s->frame = 0;
        g_frame_slots_used--;
    }
    spin_unlock_irq(&g_shm_lock);
}

/*
 * Resolve a fork() shared page into a private, writable one.  This is what a
 * write fault lands in, and what every kernel path that writes to user
 * memory through the direct map has to do first -- those paths never touch a
 * user PTE, so nothing else would notice that they are writing into a frame
 * another address space can still see.
 *
 * Returns 1 when the caller may write at `va` afterwards, 0 on failure.
 */
int vmm_cow_break(addrspace_t *as, uint64_t va)
{
    if (!as)
        return 0;

    uint64_t *pte = walk(as, va & ~0xFFFULL, 0, 0);
    if (!pte || !(*pte & PTE_P) || !(*pte & PTE_COW))
        return 0;

    uint64_t frame = *pte & PTE_ADDR;

    /* Threads share one set of page tables between several CPUs, and this
     * kernel has no way to flush a TLB entry on another core.  Swapping the
     * frame out from under a sibling would leave it reading the old page, so
     * a page the whole thread group can see stays shared and simply becomes
     * writable: within one address space nothing needed separating anyway. */
    if (as->refs > 1) {
        *pte = (*pte & ~PTE_COW) | PTE_RW;
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
        return 1;
    }

    int refs = vmm_frame_ref_count(frame);

    /* Nobody else holds this frame any more (every other holder has already
     * taken its own copy, or unmapped it): keep the page and stop pretending
     * it is shared.  No copy, no allocation -- the common case for a page
     * the child never touches again. */
    if (refs <= 1) {
        frame_ref_forget_if_last(frame);
        *pte = (*pte & ~PTE_COW) | PTE_RW;
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
        return 1;
    }

    uint64_t nframe = pmm_alloc();
    if (!nframe)
        return 0;
    /* The permissions the page already had survive the copy: only its write
     * bit and the COW marker change.  Copying them across verbatim rather
     * than rebuilding them is also what keeps orphaned pages from silently
     * gaining execute permission along the way. */
    memcpy(pmm_virt(nframe), pmm_virt(frame), PAGE_SIZE);
    uint64_t keep = *pte & (PTE_U | PTE_PWT | PTE_PCD | PTE_NX);
    *pte          = (nframe & PTE_ADDR) | keep | PTE_P | PTE_RW;
    asm volatile("invlpg (%0)" : : "r"(va) : "memory");

    vmm_share_unref(frame); /* this page table let go of the old frame */
    return 1;
}

int vmm_page_shared(addrspace_t *as, uint64_t vaddr)
{
    if (!as)
        return 0;
    uint64_t *pte = walk(as, vaddr & ~0xFFFULL, 0, 0);
    return (pte && (*pte & PTE_P) && (*pte & PTE_AVL)) ? 1 : 0;
}

/* ---- kernel-address-space helpers (used by the module loader) ---------- */
/* The kernel runs on a page table that is not one of the proc address
 * spaces, so all three wrap the walker with a space anchored at the
 * kernel PML4.  vmm_map_kernel is what maps module pages into the kernel
 * image's own higher-half. */

int vmm_map_kernel(uint64_t vaddr, uint64_t paddr, unsigned flags)
{
    addrspace_t k = {.pml4_phys = g_kernel_pml4_phys};
    return vmm_map(&k, vaddr, paddr, flags);
}

int vmm_unmap_kernel(uint64_t vaddr, uint64_t size)
{
    addrspace_t k = {.pml4_phys = g_kernel_pml4_phys};
    return vmm_unmap(&k, vaddr, size);
}

int vmm_kernel_present(uint64_t vaddr)
{
    addrspace_t k = {.pml4_phys = g_kernel_pml4_phys};
    return vmm_resolve(&k, vaddr) != 0;
}

/* Page-fault forensics: print the four page-table entries for `va` in the
 * *current* address space, so a protection fault can be told apart from a
 * missing page without a debugger. */
void vmm_pte_dump(uint64_t va)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);

    dbg_puts("VMM: va=");
    dbg_puts_hex(va);
    uint64_t *p4 = table(cr3 & PTE_ADDR);
    dbg_puts(" p4=");
    dbg_puts_hexn(p4[i4], 16);
    if (!(p4[i4] & PTE_P))
        goto out;
    uint64_t *p3 = table(p4[i4] & PTE_ADDR);
    dbg_puts(" p3=");
    dbg_puts_hexn(p3[i3], 16);
    if (!(p3[i3] & PTE_P))
        goto out;
    uint64_t *p2 = table(p3[i3] & PTE_ADDR);
    dbg_puts(" p2=");
    dbg_puts_hexn(p2[i2], 16);
    if (!(p2[i2] & PTE_P))
        goto out;
    uint64_t *p1 = table(p2[i2] & PTE_ADDR);
    dbg_puts(" pte=");
    dbg_puts_hexn(p1[i1], 16);
out:
    dbg_puts("\r\n");
}

/*
 * Extend the stack by the one page that was touched.
 *
 * Deliberately not clever: any unmapped address inside the reserved window
 * grows the stack, with no check that it is anywhere near the current RSP.
 * Linux needs that check because a user mapping can legitimately sit just
 * below the stack; here the whole window is reserved for the stack and
 * nothing else is ever placed in it, so there is nothing to protect
 * against.  Everything below the window stays unmapped forever and is the
 * guard: a runaway recursion walks off the end and takes SIGSEGV.
 */
int vmm_grow_stack(addrspace_t *as, uint64_t addr)
{
    if (!as)
        return 0;

    if (addr >= USER_STACK_TOP || addr < USER_STACK_TOP - USER_STACK_MAX)
        return 0;

    uint64_t page = addr & ~0xFFFULL;
    if (vmm_resolve(as, page))
        return 0; /* already mapped: a protection fault, not growth */

    if (!vmm_alloc_range(as, page, PAGE_SIZE, VM_USER | VM_WRITE))
        return 0;

    /* The fault loaded a not-present entry into the TLB; drop it so the
     * retried instruction sees the mapping we just made. */
    asm volatile("invlpg (%0)" ::"r"(page) : "memory");
    return 1;
}

/*
 * Map a physical MMIO region into the kernel's own address space with the
 * cache-disable (uncacheable) attribute device registers need, and return
 * its virtual base.  Device registers must not be cached or writes can be
 * coalesced/lost and reads can return stale values -- the HHDM direct map is
 * write-back, so it is wrong for this.  We carve a private virtual arena
 * above the HHDM and page the region in one page at a time.
 */
#define MMIO_BASE 0xFFFFA00000000000ULL
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size)
{
    static uint64_t mmio_next = MMIO_BASE;

    /* A bad BAR-sizing calculation produces an enormous "size", and mapping
     * it would eat every free frame on page tables before failing.  No device
     * we drive needs more than a few hundred KiB of registers. */
    if (size == 0 || size > (16ULL << 20))
        return 0;

    uint64_t base = mmio_next;
    mmio_next += (size + 0xFFF) & ~0xFFFULL;

    addrspace_t k = {.pml4_phys = g_kernel_pml4_phys};
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *pte = walk(&k, (base + off) & ~0xFFFULL, 1, 0);
        if (!pte)
            return 0;
        *pte = ((phys + off) & PTE_ADDR) | PTE_P | PTE_RW | PTE_PCD | PTE_PWT | PTE_NX;
        asm volatile("invlpg (%0)" ::"r"(base + off) : "memory");
    }
    return base;
}

int vmm_unmap(addrspace_t *as, uint64_t vaddr, uint64_t size)
{
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = walk(as, va, 0, 0);
        if (!pte || !(*pte & PTE_P))
            continue; /* nothing mapped here */
        /* TEMPORARY Xorg debugging: who clears the libpixman text page? */
        if (va == 0x5000000047000ULL) {
            dbg_puts("WATCH-UNMAP: va=0x5000000047000 oldpte=");
            dbg_puts_hex(*pte);
            dbg_puts(" caller=");
            dbg_puts_hex((uint64_t)__builtin_return_address(0));
            dbg_puts("\r\n");
        }
        /* A frame somebody else still has a PTE pointing at is owned by the
         * refcount table, not by this address space: clear the PTE and let go
         * of one reference, and it is freed only when the last one leaves.
         * PTE_AVL covers the external owners (a SysV segment, a shared
         * anonymous mapping); PTE_COW is the fork() case. */
        if (*pte & (PTE_AVL | PTE_COW)) {
            vmm_share_unref(*pte & PTE_ADDR);
            if (as->pages > 0)
                as->pages--;
            if (as->cg >= 0)
                cg_mem_discharge(as->cg, PAGE_SIZE);
        } else {
            pmm_free(*pte & PTE_ADDR);
            if (as->pages > 0)
                as->pages--; /* resident-page accounting (cgroup memory) */
            if (as->cg >= 0)
                cg_mem_discharge(as->cg, PAGE_SIZE);
        }
        *pte = 0;
    }
    /* Drop stale TLB entries for the range, but only if this is the address
     * space we are actually running on; reloading CR3 for another process
     * would switch the page tables out from under the caller. */
    uint64_t cur;
    asm volatile("mov %%cr3, %0" : "=r"(cur));
    if ((cur & PTE_ADDR) == as->pml4_phys) {
        for (uint64_t va = start; va < end; va += PAGE_SIZE)
            asm volatile("invlpg (%0)" ::"r"(va) : "memory");
    }
    return 1;
}

int vmm_alloc_range(addrspace_t *as, uint64_t vaddr, uint64_t size, unsigned flags)
{
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        if (vmm_resolve(as, va))
            continue; /* already backed */
        /* TEMPORARY Xorg debugging: does alloc_range run at all? */
        if ((start >> 44) == 5 && g_watchmap_count < 24) {
            g_watchmap_count++;
            dbg_puts("AR: va=");
            dbg_puts_hex(va);
            dbg_puts("\r\n");
        }
        /* Charge the cgroup before allocating; reject if over memory.max. */
        if (as->cg >= 0 && cg_mem_charge(as->cg, PAGE_SIZE) < 0)
            return 0;
        uint64_t frame = pmm_alloc_zeroed();
        if (!frame) {
            if (as->cg >= 0)
                cg_mem_discharge(as->cg, PAGE_SIZE);
            return 0;
        }
        if (!vmm_map(as, va, frame, flags)) {
            pmm_free(frame);
            if (as->cg >= 0)
                cg_mem_discharge(as->cg, PAGE_SIZE);
            return 0;
        }
        as->pages++; /* resident-page accounting for the cgroup
                      * memory controller */
    }
    return 1;
}

int vmm_copy_to_user(addrspace_t *as, uint64_t dst, const void *src, uint64_t n)
{
    const uint8_t *s = (const uint8_t *)src;

    while (n) {
        uint64_t phys = vmm_resolve(as, dst);
        if (!phys)
            return 0;
        /* Writing through the direct map never consults the user PTE, so a
         * fork() page has to be made private here or the bytes would land in
         * a frame the parent can still read.  Every kernel write that reaches
         * user memory this way relies on it (clone's tid pointers, clearing a
         * dying task's clear_child_tid, loading an ELF image, ptrace). */
        if (vmm_page_is_cow(as, dst) && !vmm_cow_break(as, dst))
            return 0;
        phys = vmm_resolve(as, dst);
        if (!phys)
            return 0;

        uint64_t chunk = PAGE_SIZE - (dst & 0xFFF);
        if (chunk > n)
            chunk = n;

        memcpy(pmm_virt(phys), s, chunk);
        dst += chunk;
        s += chunk;
        n -= chunk;
    }
    return 1;
}

int user_ptr_ok(uint64_t p, uint64_t len)
{
    if (p == 0)
        return 0;
    if (p >= USER_LIMIT || len > USER_LIMIT)
        return 0;
    return p + len <= USER_LIMIT;
}

/* Recursively release the lower half of a page-table tree. */
static void free_level(uint64_t table_phys, int level)
{
    uint64_t *t     = table(table_phys);
    int       limit = (level == 4) ? 256 : 512; /* upper half is shared */

    for (int i = 0; i < limit; i++) {
        if (!(t[i] & PTE_P))
            continue;
        uint64_t child = t[i] & PTE_ADDR;
        if (level > 1) {
            free_level(child, level - 1);
        } else if (t[i] & (PTE_AVL | PTE_COW)) {
            /* Shared frame: release this mapper's reference.  Same two cases
             * vmm_unmap() sorts out above -- external owner vs. fork(). */
            vmm_share_unref(child);
        } else {
            pmm_free(child);
        }
        t[i] = 0;
    }
    if (level < 4)
        pmm_free(table_phys);
}

void vmm_destroy(addrspace_t *as)
{
    if (!as)
        return;

    free_level(as->pml4_phys, 4);
    pmm_free(as->pml4_phys);
    as->pages = 0; /* every resident page just went away */

    /* Release MAP_SHARED tmpfs file mappings after the page tables that
     * pointed at their frames are gone: each drops a mapper reference on
     * the backing node, which is what finally frees an unlinked file. */
    for (uint32_t i = 0; i < as->nshared; i++)
        tmpfs_node_shm_putmapper(as->shared[i].node);
    as->nshared = 0;

    for (int i = 0; i < MAX_ADDRSPACES; i++) {
        if (&g_spaces[i] == as) {
            g_used[i] = 0;
            break;
        }
    }
    as->pml4_phys = 0;
    as->refs      = 0;
}

addrspace_t *vmm_share(addrspace_t *as)
{
    if (as)
        as->refs++;
    return as;
}

void vmm_put(addrspace_t *as)
{
    if (!as || as->refs <= 0 || --as->refs > 0)
        return;
    /* Last reference: the caller must not still be running on it. */
    vmm_destroy(as);
}

/* Walk `src`'s lower half and reproduce every mapped page in `dst`.
 *
 * There are two ways to do it and the choice is made once per fork, in
 * vmm_clone(): a shared page can only be handed out when the parent page
 * table has exactly one user, because several CPUs may be running inside it
 * at once (see below).  Both paths otherwise agree: the child is left with
 * the same content at the same addresses.
 *
 * sharing = 0: eager copy, one new frame per page.
 * sharing = 1: parent and child point at the SAME frame and both lose write
 *              permission; whoever writes first takes a private copy
 *              (vmm_cow_break).  This is the copy-on-write path.
 */
static int clone_level(addrspace_t *dst, uint64_t src_table, int level, uint64_t vbase, int sharing)
{
    uint64_t *t     = table(src_table);
    int       limit = (level == 4) ? 256 : 512;
    unsigned  shift = (unsigned)(12 + 9 * (level - 1));

    for (int i = 0; i < limit; i++) {
        if (!(t[i] & PTE_P))
            continue;

        uint64_t va = vbase | ((uint64_t)i << shift);

        if (level > 1) {
            if (!clone_level(dst, t[i] & PTE_ADDR, level - 1, va, sharing))
                return 0;
            continue;
        }

        /* The page's own attributes, copied verbatim instead of rebuilt from
         * a canned set: it keeps the execute bit it was mapped with, so a
         * data page cannot quietly become executable across a fork. */
        uint64_t keep  = (t[i] & (PTE_PWT | PTE_PCD | PTE_NX)) | PTE_U;
        uint64_t frame = t[i] & PTE_ADDR;

        if (t[i] & PTE_AVL) {
            /* Shared-anonymous (or SysV shm) page: the child maps the very
             * same frame -- copying it would give each side private memory
             * and break every futex parked on it. */
            unsigned oflags = VM_USER | VM_EXTSHM;
            if (t[i] & PTE_RW)
                oflags |= VM_WRITE;
            if (vmm_share_ref(frame) < 0)
                return 0;
            if (!vmm_map(dst, va, frame, oflags)) {
                vmm_share_unref(frame);
                return 0;
            }
            continue;
        }

        if (!sharing) {
            unsigned flags = VM_USER | VM_EXEC;
            if (t[i] & PTE_RW)
                flags |= VM_WRITE;
            uint64_t nframe = pmm_alloc();
            if (!nframe)
                return 0;
            memcpy(pmm_virt(nframe), pmm_virt(frame), PAGE_SIZE);
            if (!vmm_map(dst, va, nframe, flags)) {
                pmm_free(nframe);
                return 0;
            }
            dst->pages++;
            if (dst->cg >= 0)
                cg_mem_charge(dst->cg, PAGE_SIZE);
            continue;
        }

        /* ---- copy-on-write: hand the child the parent's own frame ------ */
        /* One count for the parent's entry (until now a private, uncounted
         * page), one for the child's.  Counting per entry rather than per
         * address space is what keeps a frame that the parent maps twice
         * from being freed while one of those mappings still uses it. */
        if (vmm_frame_ref_inc(frame) < 0)
            return 0;
        if (vmm_frame_ref_inc(frame) < 0) {
            vmm_share_unref(frame);
            return 0;
        }

        t[i] = frame | keep | PTE_P | PTE_COW;
        /* The parent may carry straight on in this very context, keeping its
         * no-longer-valid writable TLB entry for this page.  Other CPUs are
         * safe without an IPI: this address space can only be reloaded there
         * through vmm_switch(), which writes CR3 and drops every entry. */
        asm volatile("invlpg (%0)" ::"r"(va) : "memory");

        /* walk() only adds PTE_U to the newly or previously non-user
         * directory entries when asked: passing 0 here produced a table
         * whose p3/p2/p1 entries were supervisor-only, making every user
         * fetch land on err=0x15. */
        uint64_t *cpte = walk(dst, va, 1, VM_USER);
        if (!cpte) {
            vmm_share_unref(frame); /* the child's entry never happened */
            return 0;
        }
        *cpte = frame | keep | PTE_P | PTE_COW;
        dst->pages++; /* the child has the page resident too */
        if (dst->cg >= 0)
            cg_mem_charge(dst->cg, PAGE_SIZE);
    }
    return 1;
}

/* Every private page of a shared clone costs one refcount-table slot, so a
 * clone may only share once it knows the whole table load fits.  Counting
 * first and asking whether there is room keeps a clone from being half
 * copy-on-write and half eager copy, which would leave the counts wrong. */
static void count_level(uint64_t src_table, int level, uint32_t *n)
{
    uint64_t *t     = table(src_table);
    int       limit = (level == 4) ? 256 : 512;

    for (int i = 0; i < limit; i++) {
        if (!(t[i] & PTE_P))
            continue;
        if (level > 1) {
            count_level(t[i] & PTE_ADDR, level - 1, n);
            continue;
        }
        if (t[i] & PTE_AVL)
            continue; /* already counted elsewhere */
        (*n)++;
    }
}

addrspace_t *vmm_clone(addrspace_t *src)
{
    addrspace_t *dst = vmm_create();
    if (!dst)
        return NULL;

    /* Sharing is off for a page table more than one task is running in: this
     * kernel has no way to flush another CPU's TLB, so a sibling thread that
     * still believed the page writable would write straight through into the
     * child's view of it.  An eager copy is simply always safe. */
    int sharing = (src->refs == 1);
    if (sharing) {
        uint32_t need = 0;
        count_level(src->pml4_phys, 4, &need);
        if (need > FRAME_SLOTS - vmm_frame_ref_slots())
            sharing = 0; /* would not fit: copy instead */
    }

    if (!clone_level(dst, src->pml4_phys, 4, 0, sharing)) {
        vmm_destroy(dst);
        return NULL;
    }
    /* Carry the mmap records across a fork so the child's fault handler and
     * munmap see the same mappings until it execve's a fresh image. */
    dst->nmmaps = src->nmmaps;
    for (int i = 0; i < src->nmmaps; i++)
        dst->mmaps[i] = src->mmaps[i];
    dst->cg = src->cg; /* inherit the cgroup membership */
    return dst;
}

/* TEMPORARY Xorg debugging: on an unbacked fault, dump every address space's
 * record head plus the raw words just below g_spaces, so whoever corrupts the
 * table can be recognised by what it writes. */
void vmm_as_debug_dump(void)
{
    dbg_puts("ASDUMP:\r\n");
    for (int i = 0; i < MAX_ADDRSPACES; i++) {
        if (!g_used[i])
            continue;
        dbg_puts("  AS");
        dbg_puts_dec(i);
        dbg_puts(" nmmaps=");
        dbg_puts_dec(g_spaces[i].nmmaps);
        dbg_puts(" rec0=(");
        dbg_puts_hex(g_spaces[i].mmaps[0].base);
        dbg_puts(",");
        dbg_puts_hex(g_spaces[i].mmaps[0].size);
        dbg_puts(",f");
        dbg_puts_hex(g_spaces[i].mmaps[0].flags);
        dbg_puts(")\r\n");
    }
    dbg_puts("  pre: ");
    uint64_t *pre = (uint64_t *)((uint8_t *)g_spaces - 64);
    for (int i = 0; i < 8; i++) {
        dbg_puts_hex(pre[i]);
        dbg_puts(" ");
    }
    dbg_puts("\r\n");
}

/* TEMPORARY Xorg debugging: byte-sum a mapped region (0 for pages that are
 * not present).  Used to detect whether a mapping's content changed after
 * the copy-in. */
uint64_t vmm_region_checksum(addrspace_t *as, uint64_t base, uint64_t size)
{
    uint64_t sum = 0;
    uint64_t end = base + size;

    for (uint64_t va = base & ~0xFFFULL; va < end; va += PAGE_SIZE) {
        uint64_t phys = vmm_resolve(as, va);
        if (!phys)
            continue;
        uint8_t *p = (uint8_t *)pmm_virt(phys);
        for (uint64_t o = 0; o < PAGE_SIZE && va + o < end; o++)
            sum += p[o];
    }
    return sum;
}

/* TEMPORARY Xorg debugging: walk every address space's page tables and print
 * each VA whose PTE maps @frame -- the frame-alias detector.  The kernel half
 * (level-4 entries 256..511) is shared by all address spaces, so it is scanned
 * once. */
void vmm_alias_scan(uint64_t frame)
{
    int kernel_half_done = 0;

    for (int s = 0; s < MAX_ADDRSPACES; s++) {
        if (!g_used[s])
            continue;
        addrspace_t *as = &g_spaces[s];
        if (!as->pml4_phys)
            continue;
        uint64_t *p4 = table(as->pml4_phys);
        for (int a = 0; a < 512; a++) {
            int kern = a >= 256;
            if (kern && kernel_half_done)
                continue;
            if (!(p4[a] & PTE_P))
                continue;
            uint64_t *p3 = table(p4[a] & PTE_ADDR);
            for (int b = 0; b < 512; b++) {
                if (!(p3[b] & PTE_P))
                    continue;
                uint64_t *p2 = table(p3[b] & PTE_ADDR);
                for (int c = 0; c < 512; c++) {
                    if (!(p2[c] & PTE_P))
                        continue;
                    uint64_t *p1 = table(p2[c] & PTE_ADDR);
                    for (int d = 0; d < 512; d++) {
                        if (!(p1[d] & PTE_P))
                            continue;
                        if ((p1[d] & PTE_ADDR) == frame) {
                            uint64_t va = ((uint64_t)a << 39) | ((uint64_t)b << 30) |
                                          ((uint64_t)c << 21) | ((uint64_t)d << 12);
                            dbg_puts("  ALIAS: ");
                            dbg_puts(kern ? "KERN" : "as");
                            if (!kern)
                                dbg_puts_dec(s);
                            dbg_puts(" va=");
                            dbg_puts_hex(va);
                            dbg_puts(" pte=");
                            dbg_puts_hex(p1[d]);
                            dbg_puts("\r\n");
                        }
                    }
                }
            }
            if (kern)
                kernel_half_done = 1;
        }
    }
}

void vmm_switch(addrspace_t *as)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_ADDR) == as->pml4_phys)
        return; /* already there: skip the TLB flush */
    asm volatile("mov %0, %%cr3" ::"r"(as->pml4_phys) : "memory");
}

void vmm_switch_kernel(void)
{
    asm volatile("mov %0, %%cr3" ::"r"(g_kernel_pml4_phys) : "memory");
}

addrspace_t *vmm_kernel_as(void)
{
    static addrspace_t k = {.pml4_phys = 0, .refs = 1};
    k.pml4_phys          = g_kernel_pml4_phys;
    return &k;
}

/* ---- self test ----------------------------------------------------------
 * Copy-on-write is the kind of bug that leaves no trace until it corrupts
 * something: a frame freed too early, or a page the other side can still
 * write to.  This exercises the whole sharing lifetime in one go -- share,
 * separate, tear down -- and checks afterwards that neither a frame nor a
 * refcount-table entry was left behind.  It runs on two scratch address
 * spaces during boot, before any process exists. */
void vmm_cow_self_test(void)
{
    static const char ok_msg[]   = "VMM COW self test: PASS\r\n";
    static const char fail_msg[] = "VMM COW self test: FAIL\r\n";

    uint64_t free0  = pmm_free_frames();
    uint32_t slots0 = vmm_frame_ref_slots();

    /* The 4 MiB of anonymous space an ordinary user program would be given;
     * nothing here ever runs, so the address only has to be user-range. */
    uint64_t va = 0x0000400000000000ULL;

    addrspace_t *a = vmm_create();
    if (!a) {
        dbg_puts("VMM COW: no address space slot\r\n");
        return;
    }
    uint64_t frame = pmm_alloc_zeroed();
    if (!frame || !vmm_map(a, va, frame, VM_USER | VM_WRITE)) {
        dbg_puts(fail_msg);
        return;
    }
    volatile uint32_t *p = (volatile uint32_t *)pmm_virt(frame);
    p[0]                 = 0xC0FFEEu;

    addrspace_t *b = vmm_clone(a);
    if (!b) {
        dbg_puts(fail_msg);
        return;
    }

    int fail = 0;
    /* Sharing: both sides see one frame, and neither may write to it. */
    if (vmm_resolve(a, va) != frame || vmm_resolve(b, va) != frame)
        fail = 1;
    if (!vmm_page_is_cow(a, va) || !vmm_page_is_cow(b, va))
        fail = 2;
    if (vmm_frame_ref_count(frame) != 2)
        fail = 3;

    /* Separating: the child gets its own copy and the parent keeps its word. */
    if (!vmm_cow_break(b, va))
        fail = 4;
    uint64_t bframe = vmm_resolve(b, va);
    if (bframe == 0 || bframe == frame)
        fail = 5;
    if (vmm_frame_ref_count(frame) != 1)
        fail = 6;
    if (*(volatile uint32_t *)pmm_virt(bframe) != 0xC0FFEEu)
        fail = 7;
    /* A write through the child's own frame must not touch the parent's. */
    *(volatile uint32_t *)pmm_virt(bframe) = 0xDEADu;
    if (p[0] != 0xC0FFEEu)
        fail = 8;

    vmm_destroy(b);
    vmm_destroy(a);

    /* Nothing may be left outstanding: frames freed, slots returned. */
    if (pmm_free_frames() != free0)
        fail = 9;
    if (vmm_frame_ref_slots() != slots0)
        fail = 10;

    if (fail) {
        dbg_puts("VMM COW self test: FAIL step ");
        dbg_puts_dec((uint32_t)fail);
        dbg_puts("\r\n");
        return;
    }
    dbg_puts(ok_msg);
}
