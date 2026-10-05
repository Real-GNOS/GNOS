#include "libc/user/stdio.h"
#include "syscall.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_fail = 1;                                                        \
            printf("  [FAIL] %s\n", msg);                                      \
        }                                                                      \
    } while (0)

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("futex_demo: start\n");

    volatile int fut = 10;

    int r = futex((uint32_t)&fut, FUTEX_WAIT, 5, 0);
    CHECK(r == -11, "wait on mismatch returns EAGAIN");

    r = futex((uint32_t)&fut, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0);
    CHECK(r == 0, "wake with no waiters returns 0");

    r = futex(0, FUTEX_WAIT, 0, 0);
    CHECK(r == -1, "futex null uaddr rejected");

    r = futex((uint32_t)&fut, 5, 0, 0);
    CHECK(r == -1, "unknown futex op rejected");

    if (g_fail == 0) {
        printf("futex_demo: PASS\n");
        exit(0);
    }
    printf("futex_demo: FAIL\n");
    exit(1);
    return 0;
}
