/* clone_demo: CLONE_VM 线程最小示例。
 * clone 现为 Linux clone(3) 形式：clone(fn, stack_top, flags, arg)，
 * 子进程从 fn(arg) 进入（父子分叉由 libc 的 asm 包装完成）。 */
#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define STACK_SIZE 0x4000

static int shared = 0;

static int child_main(void *arg) {
    shared = 200;
    printf("child: arg=%d my pid=%d, shared=%d\n", (int)(intptr_t)arg,
           (int)getpid(), shared);
    exit(0);
    return 0;
}

int main(void) {
    char *stk = (char *)malloc(STACK_SIZE);
    int32_t pid = clone(child_main, stk + STACK_SIZE,
                        CLONE_VM | CLONE_FS | CLONE_FILES, (void *)42);
    if (pid > 0) {
        shared = 100;
        printf("parent: clone returned %d, shared=%d\n", pid, shared);
    } else {
        printf("clone failed\n");
        exit(1);
    }
    exit(0);
    return 0;
}
