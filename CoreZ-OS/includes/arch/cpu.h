#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/cpu.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/cpu.h"
#endif

#endif /* ARCH_CPU_H */