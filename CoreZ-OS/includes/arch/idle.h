#ifndef ARCH_IDLE_H
#define ARCH_IDLE_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/idle.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/idle.h"
#endif

#endif
