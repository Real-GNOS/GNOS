#ifndef ARCH_X86_IO_H
#define ARCH_X86_IO_H

#include <stdint.h>
#include "arch/x86/cpu.h"

extern void insw(uint16_t port, void *buf, int words);
extern void outsw(uint16_t port, const void *buf, int words);

#endif
