#include "kernel/init/gdt/gdt.h"
#include "kernel/asm_func.h"
#include "kernel/sched/percpu.h"

struct GDT_DESC gdt[GDT_ENTRIES];
struct GDT_REG {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr0;

static void desc_init(struct GDT_DESC *d, uint64_t base, uint32_t limit,
                      uint8_t attr_low, uint8_t attr_high) {
    d->limit_low = limit & 0xFFFF;
    d->base_low = base & 0xFFFF;
    d->base_mid = (base >> 16) & 0xFF;
    d->attr_low = attr_low;
    d->limit_high_attr_high = ((limit >> 16) & 0x0F) | attr_high;
    d->base_high = (base >> 24) & 0xFF;
}

void tss_desc_init(struct GDT_DESC *d, uint64_t base, uint32_t limit) {
    uint8_t *p = (uint8_t *)d;
    p[0] = limit & 0xFF;
    p[1] = (limit >> 8) & 0xFF;
    p[2] = base & 0xFF;
    p[3] = (base >> 8) & 0xFF;
    p[4] = (base >> 16) & 0xFF;
    p[5] = 0x89;
    p[6] = (limit >> 16) & 0x0F;
    p[7] = (base >> 24) & 0xFF;
    *(uint64_t *)(p + 8) = base >> 32;
}

void set_tss_desc(uint64_t tss_base, uint32_t tss_limit) {
    tss_desc_init(&gdt[GDT_TSS_INDEX], tss_base, tss_limit);
}

void tls_desc_set_base(uint32_t base) {
    desc_init(&gdt[GDT_TLS_INDEX], base, 0xFFFFF, 0xF2, 0xCF);
}

void gdt_init(void) {
    desc_init(&gdt[0], 0, 0, 0, 0);
    desc_init(&gdt[1], 0, 0, 0x9A, 0x20);
    desc_init(&gdt[2], 0, 0xFFFFF, 0x92, 0xCF);
    desc_init(&gdt[3], 0, 0xFFFFF, 0x9A, 0xCF);
    desc_init(&gdt[4], 0, 0xFFFFF, 0xF2, 0xCF);
    desc_init(&gdt[5], 0, 0xFFFFF, 0xFA, 0xCF);
    desc_init(&gdt[6], 0, 0, 0xFA, 0x20);
    desc_init(&gdt[GDT_TLS_INDEX], 0, 0xFFFFF, 0xF2, 0xCF);
    desc_init(&gdt[GDT_PER_CPU_INDEX], PER_CPU_BASE, 0xFFF, 0x92, 0x40);
    tls_desc_set_base(0);

    gdtr0.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr0.base = (uint64_t)gdt;
    asm_lgdt((uint64_t)&gdtr0);
    asm_reload_segments();
}
