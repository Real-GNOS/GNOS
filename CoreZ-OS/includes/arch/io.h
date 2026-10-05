#ifndef ARCH_IO_H
#define ARCH_IO_H

#if defined(CONFIG_ARCH_X86_64)
#include "arch/x86/io.h"
#elif defined(CONFIG_ARCH_AARCH64)
#error "aarch64 HAL not yet implemented: arch/arm64/io.h"
#endif

#endif
