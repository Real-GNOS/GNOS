#ifndef FORK_H
#define FORK_H

#include "kernel/asm/stub.h"
#include "kernel/sched/thread.h"
#include <stdint.h>

pid_t sys_fork(struct ARCH_REGS *r);

/* 把父任务的地址空间以 COW 方式复制到 child（child 需已建好自己的页目录），
 * 供 sys_fork 与不带 CLONE_VM 的 sys_clone 共用 */
int copy_user_space(struct TASK *parent, struct TASK *child);

#endif
