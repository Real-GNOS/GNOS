#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/mmu.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/mmu.h"
#endif

#endif
