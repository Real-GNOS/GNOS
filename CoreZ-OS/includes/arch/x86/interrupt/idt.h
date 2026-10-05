#ifndef ARCH_X86_INTERRUPT_IDT_H
#define ARCH_X86_INTERRUPT_IDT_H

#include <stdint.h>

struct IDT_ENTRY {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

#define IDT_TYPE_INT_GATE64 0x8E
#define IDT_TYPE_TRAP_GATE64 0x8F
#define IDT_TYPE_TRAP_GATE3 0xEF

extern struct IDT_ENTRY idt[256];

void idt_init(void);
void idt_load_idtr(void);
void idt_syscall_init(void);

#endif
