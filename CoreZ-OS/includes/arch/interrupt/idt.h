#ifndef ARCH_INTERRUPT_IDT_H
#define ARCH_INTERRUPT_IDT_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/interrupt/idt.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/interrupt/idt.h"
#endif

#endif /* ARCH_INTERRUPT_IDT_H */