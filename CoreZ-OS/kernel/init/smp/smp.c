#include "kernel/init/smp/smp.h"

#include <stdint.h>

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/percpu.h"
#include "kernel/sched/thread.h"
#include "kernel/init/acpi/acpi.h"
#include "kernel/init/apic/apic.h"
#include "arch/interrupt/idt.h"
#include "kernel/init/gdt/gdt.h"
#include "kernel/init/tss/tss.h"
#include "kernel/init/pit/pit.h"

struct SMP_BOOT_INFO {
    uint32_t gdtr;
    uint32_t stack_top;
    uint32_t ap_main;
    uint32_t index;
};

static struct GDT_DESC ap_gdt[NR_CPU][GDT_ENTRIES];
static uint8_t ap_boot_fpu[NR_CPU][FPU_SAVE_SIZE] __attribute__((aligned(64)));

struct GDT_REG {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
static struct GDT_REG ap_gdtr[NR_CPU];

static uint32_t ap_ready[NR_CPU];

static uint8_t ap_boot_stack[NR_CPU][PAGE_SIZE] __attribute__((aligned(16)));

static uint32_t cpu_apic_id[NR_CPU];
static uint32_t cpu_nr;

static void cpu_table_init(void) {
    const struct ACPI_MADT_INFO *madt = acpi_madt();
    uint32_t bsp = lapic_get_id();
    uint32_t total = madt ? madt->lapic_nr : 0;

    cpu_nr = 0;
    if (total == 0) {
        for (uint32_t i = 0; i < NR_CPU; i++) {
            cpu_apic_id[i] = i;
        }
        cpu_nr = NR_CPU;
        kprintf("[SMP] no MADT, assume %u CPUs with identity APIC ids\n",
                (unsigned)NR_CPU);
        return;
    }

    cpu_apic_id[cpu_nr++] = bsp;
    for (uint32_t i = 0; i < total && cpu_nr < NR_CPU; i++) {
        if (madt->lapic_apic_id[i] != bsp) {
            cpu_apic_id[cpu_nr++] = madt->lapic_apic_id[i];
        }
    }
    if (total > NR_CPU) {
        kprintf("[SMP] firmware reports %u CPUs, capped at %u\n",
                (unsigned)total, (unsigned)NR_CPU);
    }
}

static void ap_desc_init(struct GDT_DESC *d, uint32_t base, uint32_t limit,
                         uint8_t attr_low, uint8_t attr_high) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->attr_low = attr_low;
    d->limit_high_attr_high = ((limit >> 16) & 0x0F) | attr_high;
    d->base_high = (base >> 24) & 0xFF;
}

static void ap_build_gdt(uint32_t idx, uint32_t percpu_base) {
    struct GDT_DESC *g = ap_gdt[idx];
    memset(g, 0, sizeof(ap_gdt[idx]));

    ap_desc_init(&g[1], 0, 0, 0x9A, 0x20);
    ap_desc_init(&g[2], 0, 0xFFFFF, 0x92, 0xCF);
    ap_desc_init(&g[GDT_PER_CPU_INDEX], percpu_base, 0xFFF, 0x92, 0x40);
    tss_desc_init(&g[GDT_TSS_INDEX], (uint64_t)tss_cpu(idx),
                  sizeof(struct X86_TSS) - 1);

    ap_gdtr[idx].limit = (uint16_t)(sizeof(ap_gdt[idx]) - 1);
    ap_gdtr[idx].base = (uint64_t)g;
}

static void ap_main(uint32_t idx) {
    set_cpu_id(idx);
    set_current((struct TASK *)0);
    idt_load_idtr();
    idt_syscall_init();
    lapic_ap_enable();

    struct TASK *me = idle_threads[idx];
    if (me == NULL) {
        __atomic_store_n(&ap_ready[idx], 1u, __ATOMIC_RELEASE);
        asm_cli();
        for (;;) {
            asm_hlt();
        }
    }

    set_current(me);
    me->status = TASK_RUNNING;
    me->on_cpu = idx;

    __atomic_store_n(&ap_ready[idx], 1u, __ATOMIC_RELEASE);

    asm_cli();
    uint64_t *boot_kstack = 0;
    switch_to(&boot_kstack, &me->self_kstack, ap_boot_fpu[idx],
              me->fpu_storage);

    for (;;) {
        asm_hlt();
    }
}

static void wakeup_ap(uint32_t idx, uint32_t apic_id) {
    volatile struct SMP_BOOT_INFO *info =
        (volatile struct SMP_BOOT_INFO *)AP_BOOT_INFO_ADDR;

    uint32_t percpu_base = PER_CPU_BASE + idx * PAGE_SIZE;
    uint32_t stack_top = (uint32_t)(uintptr_t)&ap_boot_stack[idx][PAGE_SIZE];

    ap_build_gdt(idx, percpu_base);
    tss_ap_init(idx, stack_top);
    __atomic_store_n(&ap_ready[idx], 0u, __ATOMIC_RELAXED);

    info->gdtr = (uint32_t)(uintptr_t)&ap_gdtr[idx];
    info->stack_top = stack_top;
    info->ap_main = (uint32_t)(uintptr_t)ap_main;
    info->index = idx;
    __asm__ volatile("mfence" ::: "memory");

    lapic_send_ipi_init(apic_id);
    mtime_sleep(10);
    lapic_send_ipi_sipi(apic_id, AP_TRAMPOLINE_VECTOR);
    mtime_sleep(10);
    lapic_send_ipi_sipi(apic_id, AP_TRAMPOLINE_VECTOR);

    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if (__atomic_load_n(&ap_ready[idx], __ATOMIC_ACQUIRE)) {
            return;
        }
        asm_pause();
    }
}

void smp_init(void) {
    uint32_t bsp_id = lapic_get_id();
    kprintf("[SMP] BSP LAPIC id=0x%x\n", (unsigned)bsp_id);

    cpu_table_init();

    uint8_t *src = _binary_ap_trampoline_bin_start;
    uint32_t size = (uint32_t)(_binary_ap_trampoline_bin_end -
                               _binary_ap_trampoline_bin_start);
    if (size == 0 || size > 0x700) {
        kprintf("[SMP] no valid trampoline(%u), APs disabled\n",
                (unsigned)size);
        return;
    }
    memcpy((void *)AP_TRAMPOLINE_ADDR, src, size);

    uint32_t online = 1;
    for (uint32_t i = 1; i < cpu_nr; i++) {
        wakeup_ap(i, cpu_apic_id[i]);
        if (__atomic_load_n(&ap_ready[i], __ATOMIC_ACQUIRE)) {
            online++;
            struct TASK *idle = idle_threads[i];
            kprintf("[SMP] cpu%u online (apic id=0x%x) idle pid=%d aff=%x\n", i,
                    (unsigned)cpu_apic_id[i], idle ? idle->pid : -1,
                    idle ? (unsigned)idle->cpu_aff : 0u);
        } else {
            kprintf("[SMP] cpu%u no response, skip\n", i);
        }
    }

    kprintf("[SMP] %u/%u CPU online\n", (unsigned)online, (unsigned)cpu_nr);
}
