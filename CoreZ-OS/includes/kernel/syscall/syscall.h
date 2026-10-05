#ifndef SYSCALL_H
#define SYSCALL_H

#include "kernel/asm/stub.h"
#include "kernel/syscall_nr.h"
#include <stdint.h>

#define SYSCALL_NR_MAX 63

void syscall_init(void);

uint64_t syscall_handler(struct ARCH_REGS *r);

#endif
