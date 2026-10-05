#ifndef ARCH_X86_IDLE_H
#define ARCH_X86_IDLE_H

#include <stdint.h>

extern void asm_stihlt(void);
extern int asm_mwait_supported(void);
extern void asm_sti_mwait(uint64_t addr);

#endif
