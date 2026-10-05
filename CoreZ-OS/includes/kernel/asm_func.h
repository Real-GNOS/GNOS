#ifndef KERNEL_ASM_FUNC_H
#define KERNEL_ASM_FUNC_H

#include <stdint.h>
#include "arch/arch.h"

#define asm_cli   cpu_cli
#define asm_sti   cpu_sti
#define asm_hlt   cpu_hlt
#define asm_pause cpu_pause
#define asm_xchg  cpu_xchg32
#define asm_cmpxchg cpu_cmpxchg32
#define asm_xadd  cpu_xadd32

#define outb cpu_outb
#define inb  cpu_inb
#define outw cpu_outw
#define inw  cpu_inw
#define outl cpu_outl
#define inl  cpu_inl

#endif