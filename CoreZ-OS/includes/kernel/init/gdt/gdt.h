#ifndef GDT_H
#define GDT_H
#include <stdint.h>

#define SELECTOR_KERNEL_CODE 0x08
#define SELECTOR_KERNEL_DATA 0x10
#define SELECTOR_KCOMPAT_CODE 0x18
#define SELECTOR_U_DATA 0x23
#define SELECTOR_U_CODE 0x2B
#define SELECTOR_USER64_CODE 0x33
#define SELECTOR_TLS 0x38
#define SELECTOR_PER_CPU 0x40
#define SELECTOR_TSS 0x48

#define GDT_TLS_INDEX 7
#define GDT_PER_CPU_INDEX 8
#define GDT_TSS_INDEX 9
#define GDT_ENTRIES 12

struct GDT_DESC {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t attr_low;
    uint8_t limit_high_attr_high;
    uint8_t base_high;
} __attribute__((packed));

extern struct GDT_DESC gdt[GDT_ENTRIES];

void gdt_init(void);
void set_tss_desc(uint64_t tss_base, uint32_t tss_limit);
void tss_desc_init(struct GDT_DESC *d, uint64_t base, uint32_t limit);
void tls_desc_set_base(uint32_t base);
#endif
