#include "kernel/init/apic/apic.h"

#include <stdint.h>

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/init/acpi/acpi.h"
#include "kernel/init/pit/pit.h"

#define MSR_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1u << 8)

#define LAPIC_SVR 0x0F0
#define LAPIC_EOI 0x0B0
#define LAPIC_LVT_T 0x320
#define LAPIC_LVT_PMC 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360

#define LVIT_MASK (1u << 16)

#define IOAPIC_VER 0x01
#define IOREG_TABLE 0x10
#define IOAPIC_MAX_PINS 23

#define APIC_VADDR_IOAPIC 0x40000000u
#define APIC_VADDR_LAPIC 0x40200000u

#define IO_IR_MASK (1u << 16)
#define IO_IR_TRIGGER (1u << 15)
#define IO_IR_POLARITY (1u << 13)

#define IRQ_TIMER 0
#define IRQ_KEYBOARD 1
#define IRQ_MOUSE 12
#define IRQ_IDE 14

#define VECTOR_BASE 0x20

static volatile uint32_t *lapic;
static volatile uint32_t *ioapic;
static int s_apic_active;

int apic_active(void) {
    return s_apic_active;
}

static uint32_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return lo;
}

static void wrmsr(uint32_t msr, uint32_t lo) {
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(0), "c"(msr) : "memory");
}

static void lapic_write(uint32_t off, uint32_t v) {
    lapic[off / 4] = v;
}

static uint32_t lapic_read(uint32_t off) {
    return lapic[off / 4];
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

#define LAPIC_ID 0x020
#define LAPIC_ICR 0x300
#define LAPIC_ICR_HIGH 0x310
#define ICR_IRR_MASK 0x00001000

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

static void lapic_send_icr(uint32_t apic_id, uint32_t low) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR, low);

    uint32_t spins = 0;
    while (lapic_read(LAPIC_ICR) & ICR_IRR_MASK) {
        asm_pause();
        if (++spins > 1000000u)
            break;
    }
}

void lapic_send_ipi_init(uint32_t apic_id) {
    lapic_send_icr(apic_id, 0x00000500u);
}

void lapic_send_ipi_sipi(uint32_t apic_id, uint32_t vector) {
    lapic_send_icr(apic_id, 0x00000600u | (vector & 0xFF));
}

void lapic_ap_enable(void) {
    uint32_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    }

    lapic_write(LAPIC_LVT_LINT0, LVIT_MASK);
    lapic_write(LAPIC_LVT_LINT1, LVIT_MASK);
    lapic_write(LAPIC_LVT_PMC, LVIT_MASK);
    lapic_write(LAPIC_LVT_T, VECTOR_BASE | LVIT_MASK);
    lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFu) | 0x100u | 0x2F);
}

void lapic_send_ipi_all_but_self(uint32_t vector) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR, 0x000C4000u | (vector & 0xFFu));
}

static uint32_t ioapic_read(uint32_t reg) {
    ioapic[0] = reg;
    return ioapic[4];
}

static void ioapic_write(uint32_t reg, uint32_t v) {
    ioapic[0] = reg;
    ioapic[4] = v;
}

/**
 * 取 ISA IRQ 在 IOAPIC 上的引脚号。
 *
 * @param irq 逻辑 IRQ 号，同时决定中断向量（VECTOR_BASE + irq）
 * @returns 引脚号
 */
static uint32_t irq_pin(uint32_t irq) {
    const struct ACPI_MADT_INFO *madt = acpi_madt();

    if (madt && irq < ACPI_LEGACY_IRQ_NR &&
        madt->irq_gsi[irq] >= madt->ioapic_gsi_base) {
        return madt->irq_gsi[irq] - madt->ioapic_gsi_base;
    }
    return irq;
}

/**
 * 取 ISA IRQ 对应的重定向表极性/触发位。
 *
 * @param irq 逻辑 IRQ 号
 * @returns 可直接或进向量的低位掩码
 */
static uint32_t irq_route_flags(uint32_t irq) {
    const struct ACPI_MADT_INFO *madt = acpi_madt();
    uint16_t iso;
    uint32_t flags = 0;

    if (!madt || irq >= ACPI_LEGACY_IRQ_NR) {
        return 0;
    }
    iso = madt->irq_flags[irq];
    if ((iso & 3u) == 3u) {
        flags |= IO_IR_POLARITY;
    }
    if (((iso >> 2) & 3u) == 3u) {
        flags |= IO_IR_TRIGGER;
    }
    return flags;
}

static void ioapic_route(uint32_t irq, uint32_t vector) {
    uint32_t pin = irq_pin(irq);
    uint32_t reg;

    if (pin > IOAPIC_MAX_PINS) {
        kprintf("[APIC] irq%u -> pin%u exceeds ioapic pins, not routed\n",
                (unsigned)irq, (unsigned)pin);
        return;
    }
    reg = IOREG_TABLE + 2 * pin;
    ioapic_write(reg, vector | irq_route_flags(irq));
    ioapic_write(reg + 1, 0);
}

static void ioapic_init(void) {
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint32_t maxpin = (ver >> 16) & 0xFF;
    if (maxpin > IOAPIC_MAX_PINS) {
        maxpin = IOAPIC_MAX_PINS;
    }

    for (uint32_t pin = 0; pin <= maxpin; pin++) {
        uint32_t reg = IOREG_TABLE + 2 * pin;
        ioapic_write(reg, VECTOR_BASE | IO_IR_TRIGGER | IO_IR_MASK);
        ioapic_write(reg + 1, 0);
    }

    ioapic_route(IRQ_TIMER, VECTOR_BASE + IRQ_TIMER);
    ioapic_route(IRQ_KEYBOARD, VECTOR_BASE + IRQ_KEYBOARD);
    ioapic_route(IRQ_MOUSE, VECTOR_BASE + IRQ_MOUSE);
    ioapic_route(IRQ_IDE, VECTOR_BASE + IRQ_IDE);
}

static void disable_pic(void) {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    outb(0x20, 0x0A);
    outb(0xA0, 0x0A);
}

int apic_init(void) {
    uint32_t base = rdmsr(MSR_APIC_BASE);
    const struct ACPI_MADT_INFO *madt = acpi_madt();

    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_APIC_BASE, base);
    }

    lapic = (volatile uint32_t *)APIC_VADDR_LAPIC;
    ioapic = (volatile uint32_t *)APIC_VADDR_IOAPIC;
    if (lapic == 0 || ioapic == 0) {
        return -1;
    }

    lapic_write(LAPIC_LVT_LINT0, LVIT_MASK);
    lapic_write(LAPIC_LVT_LINT1, LVIT_MASK);
    lapic_write(LAPIC_LVT_PMC, LVIT_MASK);

    lapic_write(LAPIC_SVR, (lapic_read(LAPIC_SVR) & ~0xFFu) | 0x100u | 0x2F);

    /* LAPIC 自带定时器不再作为系统 tick 源：其频率需要用 PIT 校准，而校准依赖
     * 逐口读 0x40 的轮询，在虚拟化下每次读都是 VM exit，导致测得的窗口远长于
     * 10ms，tick 频率会低几个数量级且每次启动都不同。系统 tick 统一由 PIT
     * （1.193182MHz 固定频率，分频 PIT_HZ）提供，见 main.c 的 pit_init。 */
    lapic_write(LAPIC_LVT_T, VECTOR_BASE | LVIT_MASK);

    ioapic_init();
    disable_pic();

    s_apic_active = 1;
    kprintf("[APIC] id=%u lapic=0x%x ioapic=0x%x\n", (unsigned)lapic_get_id(),
            madt ? madt->lapic_addr : ACPI_APIC_DEFAULT_LAPIC,
            madt ? madt->ioapic_addr : ACPI_APIC_DEFAULT_IOAPIC);
    kprintf("[APIC] timer irq%u -> pin%u vector%u (PIT)\n",
            (unsigned)IRQ_TIMER, (unsigned)irq_pin(IRQ_TIMER),
            (unsigned)(VECTOR_BASE + IRQ_TIMER));
    return 0;
}
