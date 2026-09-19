/*
 * idt.c — interrupt descriptor table, PIC remap and trap dispatch. (GPLv2)
 *
 * Every vector points at the matching stub in isr.asm; the stubs funnel into
 * isr_dispatch() below with a uniform register frame.  CPU exceptions that we
 * cannot recover from end up in panic_from_frame(), which is the whole reason
 * an IDT is worth installing this early: without one, a stray #UD or #PF just
 * escalates to a triple fault and the machine silently resets.
 */
#include <stdint.h>

#include "idt.h"
#include "gdt.h"
#include "panic.h"
#include "proc.h"
#include "vmm.h"
#include "sysnum.h"              /* PROT_READ/WRITE/EXEC */
#include "pmm.h"
#include "debugcon.h"
#include "smp.h"
#include "lapic.h"
#include "cgroup.h"

/* Forward declarations for functions defined in vmm.c and lapic.c but not
 * exported through headers yet. */
extern void vmm_pte_dump(uint64_t va);
extern irq_handler_t lapic_timer_handler(void);

struct idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint8_t isr_stub_base[];   /* isr.asm, 16 bytes per vector */

static struct idt_entry g_idt[256];
static irq_handler_t    g_irq[16];
static irq_handler_t    g_syscall;

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- 8259A PIC ------------------------------------------------------- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void pic_remap(void)
{
    uint8_t m1 = inb(PIC1_DATA), m2 = inb(PIC2_DATA);
    (void)m1; (void)m2;

    outb(PIC1_CMD, 0x11);  outb(PIC2_CMD, 0x11);   /* ICW1: init + ICW4  */
    outb(PIC1_DATA, IRQ_BASE);                     /* ICW2: vector bases */
    outb(PIC2_DATA, IRQ_BASE + 8);
    outb(PIC1_DATA, 0x04);                         /* ICW3: slave on IR2 */
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);  outb(PIC2_DATA, 0x01); /* ICW4: 8086 mode    */

    outb(PIC1_DATA, 0xFF);                         /* mask everything    */
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask(unsigned irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) & ~bit));
    if (irq >= 8)                                  /* also open the cascade */
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
}

static void pic_eoi(unsigned irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ---- gates ----------------------------------------------------------- */
static void idt_set(unsigned vec, uint64_t handler, uint8_t dpl)
{
    g_idt[vec].off_lo    = (uint16_t)(handler & 0xFFFF);
    g_idt[vec].selector  = SEL_KCODE;
    g_idt[vec].ist       = 0;
    /* 0x8E = present, 64-bit interrupt gate (IF cleared on entry). */
    g_idt[vec].type_attr = (uint8_t)(0x8E | ((dpl & 3) << 5));
    g_idt[vec].off_mid   = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vec].off_hi    = (uint32_t)(handler >> 32);
    g_idt[vec].zero      = 0;
}

void irq_install(unsigned irq, irq_handler_t fn)
{
    if (irq >= 16)
        return;
    g_irq[irq] = fn;
    pic_unmask(irq);
}

void syscall_install(irq_handler_t fn)
{
    g_syscall = fn;
}

/* Minimal, framebuffer-free fatal-fault reporter.  Used for ring-0 faults
 * and double faults so that the diagnostic reaches the debug console even
 * when the full panic path would itself fault and escalate to a triple
 * fault (which would silently reset the CPU before anything was printed). */
static void __attribute__((noreturn)) fault_halt(regs_t *r, const char *tag)
{
    uint64_t cr2 = 0;
    if (r && r->vector == 14)
        asm volatile("mov %%cr2, %0" : "=r"(cr2));

    dbg_puts(tag);
    dbg_puts(" vector=");
    dbg_puts_dec((uint32_t)(r ? r->vector : 0));
    dbg_puts(" err=");
    dbg_puts_hex(r ? r->errcode : 0);
    dbg_puts(" rip=");
    dbg_puts_hex(r ? r->rip : 0);
    dbg_puts(" cr2=");
    dbg_puts_hex(cr2);
    dbg_puts(" cs=");
    dbg_puts_hex(r ? r->cs : 0);
    dbg_puts("\r\n");

    uint32_t gslo, gshi;
    uint64_t cr3;
    asm volatile("rdmsr" : "=a"(gslo), "=d"(gshi) : "c"((uint32_t)0xC0000101));
    dbg_puts(" gsbase=");
    dbg_puts_hex(((uint64_t)gshi << 32) | gslo);
    dbg_puts(" cr3=");
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    dbg_puts_hex(cr3);
    dbg_puts("\r\n");

    for (;;)
        asm volatile("cli; hlt");
}

/* ---- dispatch -------------------------------------------------------- */
/*
 * Back a not-yet-present page that falls inside one of the address space's
 * recorded mmap regions.  Returns 1 when the page was backed and the
 * faulting instruction may be retried, 0 when the address is nobody's
 * business or the access violates the mapping's protection.
 *
 * Used from both fault paths: ring 3 (the normal lazy-mmap case) and,
 * just as important, ring 0 -- syscalls that memcpy straight into user
 * buffers (read, getcwd, ...) now regularly land on anonymous pages no
 * user code has touched yet, and a kernel-mode fault there used to be
 * instantly fatal.
 */
static int fault_back_lazy(addrspace_t *as, uint64_t cr2, int errcode)
{
    if (!as)
        return 0;
    int is_write = (int)(errcode & 2);
    int is_fetch = (int)(errcode & 16);
    /* Records can overlap (a MAP_FIXED mapping replaces an earlier one), so
     * the LAST matching record -- the one a fixed mapping most recently
     * claimed -- wins, exactly as a page-table walk would resolve it. */
    int hit = -1;
    for (int i = 0; i < as->nmmaps; i++) {
        uint64_t mb = as->mmaps[i].base;
        uint64_t ms = as->mmaps[i].size;
        if (cr2 >= mb && cr2 < mb + ms)
            hit = i;
    }
    if (hit < 0) {
        /* TEMPORARY Xorg debugging: the address is in nobody's business. */
        dbg_puts("LAZY: no record covers cr2=");
        dbg_puts_hex(cr2);
        dbg_puts("\r\n");
        vmm_as_debug_dump();
        return 0;
    }
    {
        uint64_t mb = as->mmaps[hit].base;
        uint64_t ms = as->mmaps[hit].size;
        unsigned vf = as->mmaps[hit].flags;
        /* TEMPORARY Xorg debugging: say why a fault is not being backed. */
        if (is_write && !(vf & VM_WRITE)) {
            dbg_puts("LAZY: write into R-only record base=");
            dbg_puts_hex(mb);
            dbg_puts(" flags=");
            dbg_puts_hex(vf);
            dbg_puts("\r\n");
        }
        /* A write into a mapping that is not writable is a REAL fault --
         * thread-stack guard pages live on this path.  Backing them
         * silently would turn a runaway recursion into a machine that
         * eats one zeroed frame per iteration instead of just killing
         * the offender.  Same for an instruction fetch into a mapping
         * without exec permission. */
        if (is_write && !(vf & VM_WRITE))
            return 0;
        if (is_fetch && !(vf & VM_EXEC))
            return 0;
        /* A fault on a page that is ALREADY present is a protection
         * fault, not demand paging: the PTE's permissions went stale
         * (a MAP_FIXED remap once skipped present pages, leaving the
         * whole-file temp map's R-only+NX bits behind).  Upgrade the
         * existing PTE to the record's permissions instead of replacing
         * the frame -- replacing it here used to silently ZERO live
         * pages: initialized .data was wiped on its first write and
         * executable text came back as all-zero pages that the CPU
         * happily executed. */
        if (vmm_resolve(as, cr2 & ~0xFFFULL)) {
            unsigned prot = ((vf & VM_READ) ? PROT_READ : 0) |
                            ((vf & VM_WRITE) ? PROT_WRITE : 0) |
                            ((vf & VM_EXEC) ? PROT_EXEC : 0);
            return vmm_protect(as, cr2 & ~0xFFFULL, PAGE_SIZE, prot);
        }
        uint64_t frame = pmm_alloc_zeroed();
        if (!frame)
            return 0;
        int ok = vmm_map(as, cr2 & ~0xFFFULL, frame, vf);
        if (ok) {
            as->pages++;    /* lazy fault backed a resident page */
            /* TEMPORARY Xorg debugging: lazy mappings are silent -- expose
             * them, they are exactly how a text page becomes zeros. */
            dbg_puts("LAZY: backed va=");
            dbg_puts_hex(cr2 & ~0xFFFULL);
            dbg_puts(" frame=");
            dbg_puts_hex(frame);
            dbg_puts(" vf=");
            dbg_puts_hex(vf);
            dbg_puts("\r\n");
            /* Charge the page to the cgroup hierarchy; if the cgroup has
             * exceeded memory.max the allocation is rejected and the page
             * is immediately freed.  The BKL is already held by isr_dispatch
             * so cg_mem_charge sees a consistent mem_bytes snapshot. */
            if (as->cg >= 0 && cg_mem_charge(as->cg, PAGE_SIZE) < 0) {
                /* vmm_unmap frees the frame AND decrements as->pages */
                vmm_unmap(as, cr2 & ~0xFFFULL, PAGE_SIZE);
                return 0;
            }
        }
        return ok;
    }
}

void isr_dispatch(regs_t *r)
{
    /* A trap taken from ring 3 runs in the interrupted process's kernel
     * execution, so it takes the big kernel lock: the kernel is otherwise
     * lock-free, and this is the one point every entry into it passes.
     * Kernel-mode interrupts (nested inside a BKL holder) never take it.
     * The release happens on the single `out` path below, and a process
     * that blocks or is pre-empted hands the lock to whoever resumes it
     * (see bkl_leave_for_switch in proc.c). */
    if ((r->cs & 3) == 3) {
        bkl_acquire();
        if (proc_current())
            proc_current()->bkl_held = 1;
    }

    /* A double fault means the normal fault handler faulted again; report it
     * and park the CPU instead of letting it escalate to a triple fault. */
    if (r->vector == 8) {
        fault_halt(r, "GNOS: DOUBLE FAULT");
    }

    if (r->vector < 32) {
        /* #BP is the only fault we let through: it is how a debugger stops. */
        if (r->vector == 3) {
            dbg_puts("GNOS: breakpoint at ");
            dbg_puts_hex(r->rip);
            dbg_puts("\r\n");
            goto out;
        }

        /*
         * A fault in ring 3 is the user program's problem, not the kernel's:
         * turn it into SIGSEGV and let the default action kill just that
         * process.  Only a fault taken in ring 0 is fatal to the system.
         */
        if ((r->cs & 3) == 3 && proc_current()) {
            /*
             * A page fault just below the stack is not an error, it is the
             * stack growing.  Map the page and retry the instruction; only
             * if the address is outside the stack window does this fall
             * through to the SIGSEGV path below.
             */
            if (r->vector == 14) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                if (vmm_grow_stack(proc_current()->as, cr2))
                    goto out;   /* retry the faulting instruction */
                /* Demand-paging for recorded anonymous mmap regions: musl and
                 * labwc expect a freshly-mmap'd region to be lazily backed, so
                 * the first touch of a mapping that has no PTE yet just gets a
                 * zeroed page instead of a SIGSEGV.  The records live in the
                 * shared address space, so a fault in a region any thread of
                 * the process mapped is served -- but a region that was
                 * munmap'd (record dropped) is NOT resurrected, which keeps
                 * freed stacks/heaps from being silently zeroed. */
                if (fault_back_lazy(proc_current()->as, cr2,
                                    (int)(r->errcode & 2)))
                    goto out;   /* retry the faulting instruction */
            }

            dbg_puts("GNOS: fault in user pid ");
            dbg_puts_dec((uint32_t)proc_current()->pid);
            dbg_puts(" vector=");
            dbg_puts_dec((uint32_t)r->vector);
            dbg_puts(" rip=");
            dbg_puts_hex(r->rip);
            /* CR2 and the stack say far more than RIP alone: a jump through a
             * null function pointer and a wild data write both arrive here as
             * "vector=14", and only the faulting address tells them apart. */
            if (r->vector == 14) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                dbg_puts(" cr2=");
                dbg_puts_hex(cr2);
                dbg_puts(" err=");
                dbg_puts_hex(r->errcode);
                vmm_pte_dump(cr2);
                {
                    extern void dbg_puts(const char *);
                    addrspace_t *as = proc_current()->as;
                    dbg_puts("\nMMAP-RECORDS n=");
                    dbg_puts_dec((uint32_t)as->nmmaps);
                    for (unsigned i = 0; i < as->nmmaps; i++) {
                        dbg_puts(" [");
                        dbg_puts_hex(as->mmaps[i].base);
                        dbg_puts("+");
                        dbg_puts_hex(as->mmaps[i].size);
                        dbg_puts(" f=");
                        dbg_puts_hex((uint64_t)as->mmaps[i].flags);
                        dbg_puts("]");
                    }
                    dbg_puts("\n");
                }
            }
            dbg_puts(" rsp=");
            dbg_puts_hex(r->rsp);
            dbg_puts(" rdi=");
            dbg_puts_hex(r->rdi);
            dbg_puts(" rsi=");
            dbg_puts_hex(r->rsi);
            dbg_puts(" rax=");
            dbg_puts_hex(r->rax);
            dbg_puts(" r12=");
            dbg_puts_hex(r->r12);
            dbg_puts(" r13=");
            dbg_puts_hex(r->r13);
            dbg_puts(" r15=");
            dbg_puts_hex(r->r15);
            dbg_puts("\r\n");
            {
                uint32_t flo, fhi;
                asm volatile("rdmsr" : "=a"(flo), "=d"(fhi) : "c"((uint32_t)0xC0000100));
                dbg_puts("  fsbase=");
                dbg_puts_hex(((uint64_t)fhi << 32) | flo);
                dbg_puts("\r\n");
            }
            /* Dump the last few syscalls so a NULL-deref shows what led to
             * it: nr=return pairs, oldest-first. */
            proc_t *hp = proc_current();
            if (hp && hp->sys_hist_idx) {
                dbg_puts("  last syscalls: ");
                uint8_t n = hp->sys_hist_idx > 8 ? 8 : hp->sys_hist_idx;
                for (uint8_t k = 0; k < n; k++) {
                    uint8_t i = (hp->sys_hist_idx - n + k) & 7;
                    dbg_puts_dec((uint32_t)hp->sys_hist_nr[i]);
                    dbg_puts("=");
                    dbg_puts_dec((uint32_t)(int32_t)hp->sys_hist_ret[i]);
                    dbg_puts(" ");
                }
                dbg_puts("\r\n");
            }
            /* Dump return addresses from the user stack so the call chain
             * into the fault is recoverable.  Scan a window for values that
             * fall inside the text segment of a static no-PIE binary (the
             * only kind GNOS runs): those are call-chain return addresses. */
            if (proc_current()) {
                dbg_puts("  rbp=");
                dbg_puts_hex(r->rbp);
                dbg_puts("\r\n");
                dbg_puts("  bt: ");
                for (int64_t s = -16; s < 512; s++) {
                    uint64_t ua = r->rsp + (uint64_t)s * 8;
                    if (ua < 0x400000)
                        continue;
                    uint64_t phys = vmm_resolve(proc_current()->as, ua);
                    if (!phys)
                        continue;
                    uint64_t *wp = (uint64_t *)pmm_virt(phys);
                    uint64_t w = wp[(ua & 0xFFF) / 8];
                    if (w >= 0x400000 && w < 0x1000000) {
                        dbg_puts_hex(w);
                        dbg_puts(" ");
                    }
                }
                dbg_puts("\r\n");
                dbg_puts("  ustack: ");
                for (int64_t s = -8; s < 40; s++) {
                    uint64_t ua = r->rsp + (uint64_t)s * 8;
                    uint64_t phys = vmm_resolve(proc_current()->as, ua);
                    if (!phys) { dbg_puts("?? "); continue; }
                    uint64_t *wp = (uint64_t *)pmm_virt(phys);
                    uint64_t w = wp[(ua & 0xFFF) / 8];
                    dbg_puts_hex(w);
                    dbg_puts(" ");
                }
                dbg_puts("\r\n");
            }
            /* TEMPORARY Xorg debugging: identify where user programs die. */
            {
                uint64_t ucr2;
                asm volatile("mov %%cr2, %0" : "=r"(ucr2));
                dbg_puts("USERFAULT: vec=");
                dbg_puts_dec(r->vector);
                dbg_puts(" rip=");
                dbg_puts_hex(r->rip);
                dbg_puts(" cr2=");
                dbg_puts_hex(ucr2);
                dbg_puts(" rsp=");
                dbg_puts_hex(r->rsp);
                dbg_puts("\r\n");
                /* TEMPORARY Xorg debugging: re-verify the checksum of every
                 * record covering rip or cr2, and scan for frame aliases of
                 * cr2. */
                {
                    addrspace_t *uas = proc_current()->as;
                    for (int i = 0; i < uas->nmmaps; i++) {
                        uint64_t mb = uas->mmaps[i].base;
                        uint64_t ms = uas->mmaps[i].size;
                        if ((r->rip < mb || r->rip >= mb + ms) &&
                            (ucr2 < mb || ucr2 >= mb + ms))
                            continue;
                        uint64_t now = vmm_region_checksum(uas, mb, ms);
                        dbg_puts("  REC cksum base=");
                        dbg_puts_hex(mb);
                        dbg_puts(" stored=");
                        dbg_puts_hex(uas->mmaps[i].cksum);
                        dbg_puts(" now=");
                        dbg_puts_hex(now);
                        dbg_puts(uas->mmaps[i].cksum == now ? " OK" : " CHANGED");
                        dbg_puts("\r\n");
                        /* TEMPORARY: per-page zero/nonzero map. */
                        dbg_puts("  pagemap: ");
                        for (uint64_t va = mb; va < mb + ms; va += PAGE_SIZE) {
                            uint64_t ph = vmm_resolve(uas, va);
                            if (!ph) { dbg_puts("?"); continue; }
                            uint8_t *p = (uint8_t *)pmm_virt(ph);
                            int z = 1;
                            for (uint64_t o = 0; o < PAGE_SIZE; o++)
                                if (p[o]) { z = 0; break; }
                            dbg_puts(z ? "0" : "X");
                        }
                        dbg_puts("\r\n");
                    }
                    uint64_t cr2phys = vmm_resolve(uas, ucr2 & ~0xFFFULL);
                    if (cr2phys) {
                        dbg_puts("  ALIAS SCAN for frame ");
                        dbg_puts_hex(cr2phys);
                        dbg_puts("\r\n");
                        vmm_alias_scan(cr2phys);
                    }
                    /* TEMPORARY Xorg debugging: who else maps the faulting
                     * code page? */
                    uint64_t ripphys = vmm_resolve(uas, r->rip & ~0xFFFULL);
                    dbg_puts("  rip page frame=");
                    dbg_puts_hex(ripphys);
                    dbg_puts("\r\n");
                    if (ripphys) {
                        dbg_puts("  ALIAS SCAN for rip frame\r\n");
                        vmm_alias_scan(ripphys);
                        pmm_dump_free_log(ripphys, ripphys + PAGE_SIZE);
                    }
                }
                /* TEMPORARY: dump the faulting instruction bytes so the
                 * crashing operation can be disassembled offline. */
                {
                    dbg_puts("  code: ");
                    for (int b = -8; b < 16; b++) {
                        uint64_t ua = r->rip + (uint64_t)b;
                        uint64_t phys = vmm_resolve(proc_current()->as, ua & ~0xFFFULL);
                        if (!phys) { dbg_puts("?? "); continue; }
                        uint8_t *kp = (uint8_t *)pmm_virt(phys);
                        dbg_puts_hex(kp[ua & 0xFFF]);
                        dbg_puts(" ");
                    }
                    dbg_puts("\r\n");
                }
            }
            proc_signal(proc_current(), SIGSEGV);
            proc_check_signals(r);
            goto out;
        }

        /*
         * A kernel-mode fault on a lazily-backed user page: we are inside a
         * syscall memcpy'ing into a buffer no user code has touched yet.
         * Back it and retry -- same context, the BKL is already held by us,
         * so no extra locking.  Anything else stays fatal.
         */
        if (r->vector == 14 && proc_current()) {
            uint64_t kcr2;
            asm volatile("mov %%cr2, %0" : "=r"(kcr2));
            if (fault_back_lazy(proc_current()->as, kcr2,
                                (int)(r->errcode & 2)))
                goto out;
        }

        fault_halt(r, "GNOS: RING0 FAULT");
    }

    if (r->vector == SYSCALL_VECTOR) {
        if (g_syscall)
            g_syscall(r);
        else
            r->rax = (uint64_t)-38;      /* -ENOSYS */
        proc_check_signals(r);
        goto out;
    }

    /* The per-CPU LAPIC timer: every AP's clock.  Acknowledge first, for
     * the same reason the PIC path does -- the handler may switch tasks. */
    if (r->vector == LAPIC_TIMER_VECTOR) {
        lapic_eoi();
#ifdef SYSTRACE
        {
            static unsigned lt;
            if ((++lt & 4095) == 0) {
                extern void dbg_puts(const char *);
                dbg_puts("LAPIC-TICK\n");
            }
        }
#endif
        irq_handler_t h = lapic_timer_handler();
        if (h)
            h(r);
        proc_check_signals(r);
        goto out;
    }

    if (r->vector >= IRQ_BASE && r->vector < IRQ_BASE + 16) {
        unsigned irq = (unsigned)(r->vector - IRQ_BASE);

        /* Acknowledge first.  A handler may switch tasks and not come back
         * for a long time; leaving the PIC un-acked would stop every further
         * interrupt on that line, the timer included. */
        pic_eoi(irq);
        if (g_irq[irq])
            g_irq[irq](r);
        proc_check_signals(r);
        goto out;
    }

out:
    /* Release the big kernel lock on the way back to ring 3.  The check is
     * on the flag rather than the CS: an interrupt that pre-empted a ring-3
     * task and switched to another one leaves `r` describing the old frame,
     * while bkl_held belongs to whoever is current now. */
    proc_t *cur = proc_current();
    if (cur && cur->bkl_held) {
        cur->bkl_held = 0;
        bkl_release();
    }
}

void idt_init(void)
{
    pic_remap();

    for (unsigned v = 0; v < 256; v++) {
        uint64_t stub = (uint64_t)(uintptr_t)isr_stub_base + (uint64_t)v * 16;
        /* int 0x80 must be reachable from ring 3, everything else must not. */
        idt_set(v, stub, (v == SYSCALL_VECTOR) ? 3 : 0);
    }

    struct idtr idtr = { .limit = sizeof(g_idt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_idt };
    asm volatile("lidt %0" :: "m"(idtr) : "memory");

    dbg_puts("GNOS: IDT installed, stubs@");
    dbg_puts_hex((uint64_t)(uintptr_t)isr_stub_base);
    dbg_puts("\r\n");
}

/* Reload the shared IDT on the current CPU (used by APs after they have
 * their own GDT).  The table itself is built once in idt_init(). */
void idt_load(void)
{
    struct idtr idtr = { .limit = sizeof(g_idt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_idt };
    asm volatile("lidt %0" :: "m"(idtr) : "memory");
}
