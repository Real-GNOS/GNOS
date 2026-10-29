/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smp.c — multi-core bring-up for GNOS. (GPLv2)
 *
 * Brings the APs reported by Limine online and runs them into the shared
 * EEVDF scheduler.  Every core owns its cpu_t slot (found through GS), its
 * GDT/TSS, its stack and -- once lapic_timer_start() has run -- its own
 * 100 Hz clock; the run queue and the process table are shared under
 * g_proc_lock, everything else under the big kernel lock defined here.
 */
#include <stdint.h>
#include <limine.h>

#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "debugcon.h"
#include "lapic.h"
#include "panic.h"
#include "proc.h"
#include "syscall.h"

cpu_t g_cpu[MAX_CPUS];
int   g_ncpus;
int   g_bsp_id;

/* The big kernel lock.  Taken by isr_dispatch on every trap from ring 3
 * and dropped before a process parks; the rest of the kernel stays
 * lock-free exactly as it was on one core. */
spinlock_t g_bkl;

void bkl_acquire(void)
{
    spin_lock(&g_bkl);
}

void bkl_release(void)
{
    spin_unlock(&g_bkl);
}

static spinlock_t        g_tlb_lock;
static uint32_t          g_tlb_epoch;
static volatile int      g_tlb_active;
static volatile uint32_t g_tlb_ack[MAX_CPUS];

static inline void cpu_relax(void)
{
    asm volatile("pause");
}

void smp_tlb_shootdown_all(void)
{
    int online = smp_online_count();
    if (online <= 1)
        return;
    if (!lapic_ready())
        panic("smp_tlb: LAPIC unavailable");

    spin_lock_irq(&g_tlb_lock);

    uint32_t epoch;
    do {
        epoch = ++g_tlb_epoch;
    } while (epoch == 0);

    __atomic_store_n(&g_tlb_active, 1, __ATOMIC_RELEASE);
    if (lapic_send_nmi_others() < 0) {
        __atomic_store_n(&g_tlb_active, 0, __ATOMIC_RELEASE);
        spin_unlock_irq(&g_tlb_lock);
        panic("smp_tlb: NMI delivery failed");
    }

    for (int i = 0; i < g_ncpus; i++) {
        if (!g_cpu[i].online || i == (int)cpu_self()->id)
            continue;
        uint64_t spin = 0;
        while (__atomic_load_n(&g_tlb_ack[i], __ATOMIC_ACQUIRE) != epoch) {
            if (++spin > 2000000000ULL) {
                __atomic_store_n(&g_tlb_active, 0, __ATOMIC_RELEASE);
                spin_unlock_irq(&g_tlb_lock);
                panic("smp_tlb: no ack from CPU");
            }
            cpu_relax();
        }
    }

    __atomic_store_n(&g_tlb_active, 0, __ATOMIC_RELEASE);
    spin_unlock_irq(&g_tlb_lock);
}

int smp_tlb_handle_nmi(void)
{
    if (!__atomic_load_n(&g_tlb_active, __ATOMIC_ACQUIRE))
        return 0;

    uint32_t epoch = __atomic_load_n(&g_tlb_epoch, __ATOMIC_RELAXED);
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3) : : "memory");
    asm volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
    __atomic_store_n(&g_tlb_ack[cpu_self()->id], epoch, __ATOMIC_RELEASE);
    return 1;
}

/* Static per-CPU stacks, ready before smp_init() runs (no heap needed). */
uint8_t g_cpu_stack[MAX_CPUS][PERCPU_KSTACK] __attribute__((aligned(16)));

/* A flat array of stack tops, indexed by cpu, so the AP trampoline can load
 * RSP without knowing the layout of struct cpu. */
void *g_ap_stack_top[MAX_CPUS];

/* Defined in limine_requests.c; NULL if Limine did not honour the request
 * (old firmware / single-core). */
extern volatile struct limine_smp_request smp_request;

int smp_online_count(void)
{
    int n = 0;
    for (int i = 0; i < g_ncpus; i++)
        if (g_cpu[i].online)
            n++;
    return n;
}

void smp_init(void)
{
    if (!smp_request.response) {
        g_ncpus            = 1;
        g_cpu[0].id        = 0;
        g_cpu[0].online    = 1;
        g_cpu[0].stack_top = (void *)((uintptr_t)g_cpu_stack[0] + PERCPU_KSTACK);
        dbg_puts("GNOS: SMP disabled (no response from Limine)\r\n");
        return;
    }

    struct limine_smp_response *resp = smp_request.response;
    g_ncpus                          = (int)resp->cpu_count;
    g_bsp_id                         = (int)resp->bsp_lapic_id;
    if (g_ncpus > MAX_CPUS)
        g_ncpus = MAX_CPUS;

    for (int i = 0; i < g_ncpus; i++) {
        g_cpu[i].id        = i;
        g_cpu[i].lapic_id  = resp->cpus[i]->lapic_id;
        g_cpu[i].stack_top = (void *)((uintptr_t)g_cpu_stack[i] + PERCPU_KSTACK);
        g_ap_stack_top[i]  = g_cpu[i].stack_top;
        /* The BSP (cpus[0]) is already running; everyone else is not online
         * until its trampoline reaches ap_main(). */
        g_cpu[i].online = (i == 0);
    }

    /* Hand each AP its entry point and the cpu index it should use.  Writing
     * goto_address is what releases the AP: Limine starts it the moment the
     * field is non-NULL. */
    for (int i = 1; i < g_ncpus; i++) {
        resp->cpus[i]->extra_argument = (uint64_t)i;
        resp->cpus[i]->goto_address   = (limine_goto_address)ap_entry;
    }

    /* Wait (bounded) for every AP to report in.  APs are parked afterwards,
     * so a slow or dead core just stays offline rather than hanging boot. */
    for (volatile unsigned t = 0; t < 200000000U; t++) {
        int all = 1;
        for (int i = 1; i < g_ncpus; i++)
            if (!g_cpu[i].online) {
                all = 0;
                break;
            }
        if (all)
            break;
    }

    dbg_puts("GNOS: SMP: ");
    dbg_puts_dec((uint32_t)smp_online_count());
    dbg_puts(" CPU(s) online (BSP lapic ");
    dbg_puts_dec((uint32_t)g_bsp_id);
    dbg_puts(")\r\n");
}

/* C entry for an AP.  RSP already points at this CPU's stack (the trampoline
 * set it before calling), so it is safe to use C here. */
void ap_main(int cpu)
{
    /* Publish the self pointer and point GS at this core's slot *before*
     * anything touches cpu_self() (gdt_build -> tss_set_rsp0 does). */
    g_cpu[cpu].self = &g_cpu[cpu];
    uint64_t base   = (uint64_t)(uintptr_t)&g_cpu[cpu];
    asm volatile("wrmsr" ::"c"((uint32_t)IA32_GS_BASE), "a"((uint32_t)(base & 0xFFFFFFFFu)),
                 "d"((uint32_t)(base >> 32))
                 : "memory");

    /* Build and load this core's own GDT/TSS. */
    gdt_build(&g_cpu[cpu]);

    struct gdtr gdtr = {.limit = sizeof(g_cpu[cpu].gdt) - 1,
                        .base  = (uint64_t)(uintptr_t)g_cpu[cpu].gdt};
    asm volatile("lgdt %0" ::"m"(gdtr) : "memory");

    /* Reload every data selector and CS onto the new GDT.  CS is what the
     * CPU will push into every interrupt frame; leaving it on Limine's
     * selector (0x28 here, our TSS) makes the first iretq #GP(0x28). */
    asm volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq %1\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "i"(SEL_KDATA), "i"(SEL_KCODE), "i"(SEL_TSS)
        : "rax", "memory");

    /* `mov %ax,%gs` zeroed IA32_GS_BASE (long-mode selector reload); restore
     * the per-CPU pointer set above or cpu_self() faults at address 0. */
    g_cpu[cpu].self = &g_cpu[cpu];
    asm volatile("wrmsr" ::"c"((uint32_t)IA32_GS_BASE), "a"((uint32_t)(base & 0xFFFFFFFFu)),
                 "d"((uint32_t)(base >> 32))
                 : "memory");

    /* The shared IDT is a safety net until interrupts come on. */
    idt_load();

    /* FPU/SSE control bits are per-core too.  Limine leaves the AP with
     * CR4.OSFXSR/OSXMMEXCPT clear, so the first user SSE instruction (musl
     * emits `pxor %xmm0,%xmm0`) takes #UD on this core. */
    {
        uint64_t cr0, cr4;
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((1ULL << 2) | (1ULL << 3)); /* clear EM and TS */
        cr0 |= (1ULL << 1);                  /* MP */
        asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT */
        asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
    }

    /* SYSCALL MSRs are per-core; without this the first user `syscall` on
     * this AP takes #UD (vector 6) at the instruction.  Do it before the
     * core is published online so nobody schedules user code here yet. */
    syscall_init_cpu();

    g_cpu[cpu].online = 1;

    /* This core's own clock, then join the scheduler.  With an empty run
     * queue schedule() falls back to hlt until the first process is ready;
     * the LAPIC timer makes sure a wake-up tick eventually lands. */
    lapic_init();
    lapic_timer_start();
    sched_start();
    __builtin_unreachable();
}

/* ---- user-RSP stash watchpoint (temporary exec debugging) --------------
 *
 * syscall_entry stashes the user RSP in cpu_t.user_rsp (GS+0x28) on every
 * ring-3 entry and the iretq tail restores it from there.  The openrc #GP
 * crashes showed that stash holding path strings and kernel pointers --
 * someone overwrites it mid-syscall.  Arm DR0 on the BSP's stash slot and
 * record the RIP the entry-time write returns to, so the #DB handler can
 * wave through the one benign writer and dump everyone else in the act. */
uint64_t g_user_rsp_watch_rip;
uint64_t g_wp_benign_hits;

void smp_arm_user_rsp_watch(void)
{
    extern char syscall_entry[];
    /* entry+0x14: the instruction after `mov %rsp,%gs:0x28` (cli 1b,
     * pushf/popf 10b, the mov 9b => 0xb + 9). */
    g_user_rsp_watch_rip = (uint64_t)(uintptr_t)syscall_entry + 0x14;

    /* Watch BOTH cores' stash, LOW and HIGH halves.  The corruption seen
     * so far only touches the high dword (0x00007FFF -> 0x00006FFF), a
     * 4-byte write at user_rsp+4 that a low-half-only watch never sees.
     * DR0/DR1 = BSP low/high, DR2/DR3 = AP low/high. */
    uint64_t slot0 = (uint64_t)(uintptr_t)&g_cpu[0].user_rsp;
    uint64_t slot1 = (uint64_t)(uintptr_t)&g_cpu[1].user_rsp;
    asm volatile("mov %0, %%dr0" ::"r"(slot0));
    asm volatile("mov %0, %%dr1" ::"r"(slot0 + 4));
    asm volatile("mov %0, %%dr2" ::"r"(slot1));
    asm volatile("mov %0, %%dr3" ::"r"(slot1 + 4));
    /* L0..L3 enabled; each watch: R/W=01 (write), LEN=11 (4 bytes). */
    asm volatile("mov %0, %%dr7" ::"r"((uint64_t)0x7777000F));
    dbg_puts("WP: watching user_rsp stash @");
    dbg_puts_hex(slot0);
    dbg_puts(" + AP ");
    dbg_puts_hex(slot1);
    dbg_puts(" (benign rip ");
    dbg_puts_hex(g_user_rsp_watch_rip);
    dbg_puts(")\r\n");
}
