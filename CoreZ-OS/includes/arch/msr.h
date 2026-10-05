#ifndef ARCH_MSR_H
#define ARCH_MSR_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/msr.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/msr.h"
#endif

#endif
