/* clone_stress: clone flags 语义与地址空间引用计数自测
 *
 * clone 为 Linux clone(3) 形式：clone(fn, stack_top, flags, arg)。
 *
 * 用例：
 *   1. CLONE_VM 线程先退出，父线程随后读共享内存 —— 退出只减引用，
 *      空间必须继续可用（修复前子退出可能拆掉父的页表）。
 *   2. 不带 CLONE_VM 的 clone —— COW 私有副本，子写不影响父。
 *   3. 多个 CLONE_VM 线程并发写共享计数 —— 引用计数表支持多持有者。
 *   4. CLONE_THREAD 分离线程退出 —— 不产生僵尸，父进程继续存活。
 */
#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define STACK_SIZE 0x4000

static volatile int shared;
static volatile int done;

static void wait_until(volatile int *v, int want) {
    while (*v != want) {
        futex((uint32_t)(uintptr_t)v, FUTEX_WAIT, want == 1 ? 0 : 1, 0);
    }
}

static void notify(volatile int *v, int want) {
    *v = want;
    futex((uint32_t)(uintptr_t)v, FUTEX_WAKE, 1, 0);
}

/* 用例 1 + 4 的线程体：写共享变量后退出，父验证共享可见 */
static int thread_share(void *arg) {
    shared = 200;
    notify(&done, 1);
    exit(0);
    return 0;
}

/* 用例 3 的线程体：按 arg 累加共享计数 */
static int thread_add(void *arg) {
    shared += (int)(intptr_t)arg;
    if ((int)(intptr_t)arg == 5) {
        notify(&done, 1);
    }
    exit(0);
    return 0;
}

/* 用例 2 的线程体：写自己的私有副本 */
static int thread_copy(void *arg) {
    (void)arg;
    shared = 77;
    exit(0);
    return 0;
}

int main(void) {
    char *stk = (char *)malloc(STACK_SIZE);
    char *stk2 = (char *)malloc(STACK_SIZE);
    char *stk3 = (char *)malloc(STACK_SIZE);
    int32_t st = 0;
    int fail = 0;

    /* 用例 1：CLONE_VM 子先退出，父随后访问共享内存 */
    shared = 0;
    done = 0;
    int32_t tid = clone(thread_share, stk + STACK_SIZE,
                        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD,
                        (void *)0);
    if (tid < 0) {
        printf("FAIL: clone(CLONE_VM) returned %d\n", (int)tid);
        fail = 1;
    } else {
        wait_until(&done, 1);
        if (shared == 200) {
            printf("PASS: CLONE_VM child-exit-first shared=%d\n", shared);
        } else {
            printf("FAIL: shared=%d (want 200)\n", shared);
            fail = 1;
        }
    }

    /* 用例 2：无 CLONE_VM —— COW 私有副本，子写不影响父 */
    shared = 0;
    tid = clone(thread_copy, stk2 + STACK_SIZE, CLONE_FS | CLONE_FILES,
                (void *)0);
    if (tid < 0) {
        printf("FAIL: clone(fork-like) returned %d\n", (int)tid);
        fail = 1;
    } else {
        if (wait(&st) != tid) {
            printf("FAIL: wait did not reap child\n");
            fail = 1;
        }
        if (shared == 0) {
            printf("PASS: no-CLONE_VM copy isolated shared=%d\n", shared);
        } else {
            printf("FAIL: parent saw child write shared=%d\n", shared);
            fail = 1;
        }
    }

    /* 用例 3：两个 CLONE_VM 线程都写共享计数，父最后退出（末位引用释放） */
    shared = 0;
    done = 0;
    int32_t t1 = clone(thread_add, stk + STACK_SIZE, CLONE_VM | CLONE_THREAD,
                       (void *)10);
    int32_t t2 = clone(thread_add, stk3 + STACK_SIZE, CLONE_VM | CLONE_THREAD,
                       (void *)5);
    if (t1 < 0 || t2 < 0) {
        printf("FAIL: thread clones returned %d/%d\n", (int)t1, (int)t2);
        fail = 1;
    } else {
        wait_until(&done, 1);
        if (shared == 15) {
            printf("PASS: two CLONE_VM threads shared=%d\n", shared);
        } else {
            printf("FAIL: shared=%d (want 15)\n", shared);
            fail = 1;
        }
    }

    if (fail) {
        exit(1);
    }
    printf("clone_stress: ALL PASS\n");
    exit(0);
    return 0;
}
