/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kaslr.c — moving the kernel to a random virtual base, once, very early.
 *
 * Limine loads the kernel at a fixed default virtual base and offers no
 * request to change that, so per-boot randomization has to happen after the
 * handoff, here.  The shape of the trick:
 *
 *   - The image is PIE, linked at virtual address 0, so "where it lives" is
 *     entirely in the page tables and the RELATIVE relocations.  Nothing in
 *     the image itself needs to move.
 *   - A random 2 MB slot of the *same* 1 GiB window is aliased onto the same
 *     physical pages (2 MB pages, PS bit, written through the hhdm).  The old
 *     and the new window are two views of one memory.
 *   - Because the views are one memory, the RELATIVE relocations can be
 *     re-applied for the new base by writing through the old view -- each
 *     entry's target just needs the new base added where Limine put the old
 *     one.
 *   - Then execution jumps to the new view and never comes back: coming back
 *     would make the randomization cosmetic, since the old base is the one
 *     address an attacker can guess.
 *
 * Every step checks before it writes, and every failure falls back to the
 * default base with a line on the debug console: a broken randomization must
 * not cost the boot.
 */

#include <stdint.h>
#include "kaslr.h"
#include "debugcon.h"

/* Set by kernel_entry; kaslr_landed overwrites it with the new base. */
extern uint64_t g_kernel_virt;

/* The window Limine maps the kernel into: 0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF
 * (PML4[511], PML4-directory-pointer-table[480]).  Staying inside it means
 * the PML4 and the PDPT already exist, and stopping at slot 511 keeps the
 * kernel below the module window at 0xFFFFFFFFC0000000. */
#define KASLR_WINDOW_BASE 0xFFFFFFFF80000000ULL
#define KASLR_PML4_INDEX  511u
#define KASLR_PDPT_INDEX  510u
#define KASLR_SLOT_BYTES  0x200000ULL /* 2 MB, one PD entry */
#define KASLR_PD_ENTRIES  512u

/* The RELATIVE relocation type in the x86-64 psABI.  It is 8: type 1 is
 * R_X86_64_64, and filtering on 1 silently skips the entire table -- which
 * is exactly what the first version did. */
#define R_X86_64_RELATIVE 8u

/* linker.ld brackets the dynamic relocation table. */
extern const uint8_t __rela_start[];
extern const uint8_t __rela_end[];

/* The end of the image.  The kernel is linked at virtual address 0, so the
 * linker's value for this symbol *is* the image span in bytes. */
extern uint8_t _kernel_end[];

/* kernel_main is the continuation: it never returns, which is what lets the
 * old virtual address be abandoned outright. */
void kernel_main(void);

/* Written before the jump, read after it -- one memory, two views.  A
 * register argument would need ABI surgery on the jmp; a static cell just
 * works, because the alias means the store is already visible at the new
 * base. */
static uint64_t kaslr_new_base;

/* ---- early entropy ------------------------------------------------------ */

static uint64_t kaslr_entropy(void)
{
    uint64_t e;
    uint32_t a, b, c, d;

    __asm__ volatile("rdtsc" : "=A"(e));

    /* RDRAND when the CPU advertises it; TSC alone is decent but resets with
     * the machine, so a hardware generator is worth the probe. */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (c & (1u << 30)) {
        uint64_t r;
        uint8_t  ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=q"(ok)::"cc");
        if (ok)
            e ^= r;
    }
    return e;
}

/* ---- the relocations ----------------------------------------------------- */

struct elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t  addend;
};

/*
 * Re-apply the RELATIVE relocations for @new_base.  The old view and the new
 * view are the same physical memory, so writing through the old addresses
 * fixes the new ones; the loop runs while the code is still executing at the
 * old base, which is safe because nothing here dereferences the pointers it
 * is rewriting.
 */
static void kaslr_reapply_relocations(uint64_t old_base, uint64_t new_base, uint64_t phys_base,
                                      uint64_t hhdm)
{
    /* __rela_start decays to the old-base address; subtract it back out to
     * get the image offset, then rebuild through the hhdm. */
    uint64_t                 rela_off = (uint64_t)(uintptr_t)__rela_start - old_base;
    const struct elf64_rela *rela     = (const struct elf64_rela *)(hhdm + phys_base + rela_off);
    uint64_t                 count    = (uint64_t)(__rela_end - __rela_start) / sizeof(*rela);
    uint64_t                 relative = 0;

    for (uint64_t i = 0; i < count; i++) {
        if ((uint32_t)(rela[i].info) != R_X86_64_RELATIVE)
            continue;
        /* Through the hhdm, not through either kernel view: some targets sit
         * in the read-only .limine_requests segment (the module request's
         * internal pointers), and the old view enforces its PHDR flags.  The
         * hhdm maps all of physical memory read-write. */
        *(uint64_t *)(hhdm + phys_base + rela[i].offset) = new_base + (uint64_t)rela[i].addend;
        relative++;
    }

    dbg_puts("GNOS: kaslr   = ");
    dbg_puts_dec((uint32_t)relative);
    dbg_puts(" relative relocs rewritten\r\n");
}

/* ---- landing pad -------------------------------------------------------- */

/*
 * Runs at the new base.  Must never return: its caller jumped here, so the
 * return address on the stack points back into the old-base instruction
 * stream, and continuing there would undo the randomization.  It hands the
 * boot to kernel_main(), which does not return either.
 */
static __attribute__((noinline)) void kaslr_landed(void)
{
    g_kernel_virt = kaslr_new_base;
    dbg_puts("GNOS: kvirt  = ");
    dbg_puts_hex(g_kernel_virt);
    dbg_puts(" (randomized)\r\n");
    kernel_main();
}

/* ---- the move ------------------------------------------------------------ */

uint64_t kaslr_maybe_relocate(uint64_t old_base, uint64_t phys_base, uint64_t hhdm)
{
    if (old_base != KASLR_WINDOW_BASE) {
        dbg_puts("GNOS: kaslr   = unexpected base, keeping it\r\n");
        return old_base;
    }
    if (!hhdm) {
        dbg_puts("GNOS: kaslr   = no hhdm, keeping default base\r\n");
        return old_base;
    }

    /* Image span in bytes.  _kernel_end decays to the symbol's *running*
     * address (old base + linked offset), so the base must come off -- the
     * first version forgot, and the allocator politely refused to find a
     * slot for a 4 EiB image. */
    uint64_t span  = (uint64_t)(uintptr_t)_kernel_end - old_base;
    uint64_t slots = (span + KASLR_SLOT_BYTES - 1) / KASLR_SLOT_BYTES;

    /* One 2 MB page above the image's own slot, and enough room below the
     * top of the window for the whole image. */
    if (slots + 2 > KASLR_PD_ENTRIES) {
        dbg_puts("GNOS: kaslr   = image too large for the window\r\n");
        return old_base;
    }

    /* Find the page tables Limine built, through the hhdm. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    const uint64_t *pml4 = (const uint64_t *)(hhdm + (cr3 & ~0xFFFULL));
    if (!(pml4[KASLR_PML4_INDEX] & 1)) {
        dbg_puts("GNOS: kaslr   = pml4 entry missing\r\n");
        return old_base;
    }
    const uint64_t *pdpt = (const uint64_t *)(hhdm + (pml4[KASLR_PML4_INDEX] & ~0xFFFULL));
    if (!(pdpt[KASLR_PDPT_INDEX] & 1)) {
        dbg_puts("GNOS: kaslr   = pdpt entry missing\r\n");
        return old_base;
    }
    if (pdpt[KASLR_PDPT_INDEX] & 0x080) {
        dbg_puts("GNOS: kaslr   = 1 GiB page, table layout unknown\r\n");
        return old_base;
    }
    uint64_t *pd = (uint64_t *)(hhdm + (pdpt[KASLR_PDPT_INDEX] & ~0xFFFULL));

    /* Pick a slot of consecutive free entries.  Eight draws from ~460 make
     * a collision about as likely as a boot-time coincidence can be, and the
     * fallback still boots.
     *
     * The physical base is NOT 2 MiB aligned (Limine hands it out page
     * aligned), and a PS entry demands zero in bits 20:13 -- a raw phys_base
     * in the entry sets reserved bits and every access through it faults.
     * So the mapping starts at the 2 MiB-aligned address below the image and
     * the new base picks up the remainder as an offset. */
    uint64_t d  = phys_base & (KASLR_SLOT_BYTES - 1);
    uint64_t p2 = phys_base - d; /* 2 MiB aligned */
    uint64_t n  = (d + span + KASLR_SLOT_BYTES - 1) / KASLR_SLOT_BYTES;

    /* The image's own mapping occupies the first ceil(span / 2 MB) entries;
     * clobbering any of them pulls the rug out from under the running code. */
    uint64_t kmin = (span + KASLR_SLOT_BYTES - 1) / KASLR_SLOT_BYTES;
    uint64_t kmax = KASLR_PD_ENTRIES - n; /* k + n <= 512 */
    if (kmin > kmax) {
        dbg_puts("GNOS: kaslr   = no room in the window, keeping default\r\n");
        return old_base;
    }

    uint64_t slot = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t k    = kmin + kaslr_entropy() % (kmax - kmin + 1);
        int      free = 1;
        for (uint64_t i = 0; i < n; i++) {
            if (pd[k + i] & 1) {
                free = 0;
                break;
            }
        }
        if (free) {
            slot = k;
            break;
        }
    }
    if (!slot) {
        dbg_puts("GNOS: kaslr   = no free slot found, keeping default\r\n");
        return old_base;
    }

    /* Alias the image: entry k+i maps the 2 MiB at p2 + i * 2 MB, so the
     * image -- which starts d bytes into the first page -- lands at
     * window_base + k * 2 MB + d with its linear offset preserved:
     * new_base + o == physical_base + o for every image offset o. */
    for (uint64_t i = 0; i < n; i++) {
        pd[slot + i] = (p2 + i * KASLR_SLOT_BYTES) | 0x003u /* present, read/write */
                       | 0x080u;                            /* PS: 2 MiB page */
    }

    uint64_t new_base = KASLR_WINDOW_BASE + slot * KASLR_SLOT_BYTES + d;

    /* Prove the alias before trusting it with the machine: read back
     * through the new window and compare against the old view. */
    {
        static const uint64_t probes[] = {0, 0x1000, 0x400000, 0x1000000};
        for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
            uint64_t o = probes[i];
            if (*(volatile uint64_t *)(new_base + o) != *(volatile uint64_t *)(old_base + o)) {
                dbg_puts("GNOS: kaslr   = alias readback mismatch at +0x");
                dbg_puts_hex(o);
                dbg_puts(", rolling back\r\n");
                for (uint64_t j = 0; j < n; j++)
                    pd[slot + j] = 0;
                return old_base;
            }
        }
    }

    dbg_puts("GNOS: kaslr   = slot ");
    dbg_puts_dec((uint32_t)slot);
    dbg_puts(", base ");
    dbg_puts_hex(new_base);
    dbg_puts("\r\n");

    kaslr_reapply_relocations(old_base, new_base, phys_base, hhdm);

    /* Go.  The address arithmetic is done against the symbol's old-base
     * value, and the jump is indirect so nothing PC-relative is involved.
     * The stack stays where Limine put it -- still mapped, and holding
     * nothing the new view disagrees with. */
    kaslr_new_base  = new_base;
    uint64_t target = (uint64_t)(uintptr_t)&kaslr_landed - old_base + new_base;
    __asm__ volatile("jmp *%0" ::"r"(target) : "memory");
    __builtin_unreachable();
}
