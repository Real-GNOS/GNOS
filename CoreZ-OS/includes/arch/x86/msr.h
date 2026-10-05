#ifndef ARCH_X86_MSR_H
#define ARCH_X86_MSR_H

#include <stdint.h>

extern uint64_t asm_rdmsr(uint32_t msr);
extern void asm_wrmsr(uint32_t msr, uint64_t value);

#define X86_MSR_IA32_EFER 0xC0000080u

#endif
