#ifndef ARCH_X86_MMU_H
#define ARCH_X86_MMU_H

#include <stdint.h>

extern uint64_t asm_read_cr0(void);
extern void asm_write_cr0(uint64_t cr0);
extern uint64_t asm_read_cr2(void);
extern uint64_t asm_read_cr3(void);
extern void asm_write_cr3(uint64_t cr3);
extern uint64_t asm_read_cr4(void);
extern void asm_write_cr4(uint64_t cr4);

#endif
