#ifndef CLONE_H
#define CLONE_H

#include "kernel/asm/stub.h"
#include "kernel/sched/thread.h"
#include <stdint.h>

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_SYSVSEM 0x00040000

pid_t sys_clone(struct ARCH_REGS *r);
pid_t sys_clone_ex(uint32_t flags, uint32_t child_user_stack, uint32_t tls,
                   struct ARCH_REGS *r);

#endif
