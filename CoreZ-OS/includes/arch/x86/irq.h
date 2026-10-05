#ifndef ARCH_X86_IRQ_H
#define ARCH_X86_IRQ_H

#include <stdint.h>

extern uint64_t asm_save_eflags(void);
extern void asm_restore_eflags(uint64_t eflags);

#endif
