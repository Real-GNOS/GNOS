#ifndef ARCH_INTERRUPT_INTERRUPT_H
#define ARCH_INTERRUPT_INTERRUPT_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/interrupt/interrupt.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/interrupt/interrupt.h"
#endif

#endif /* ARCH_INTERRUPT_INTERRUPT_H */