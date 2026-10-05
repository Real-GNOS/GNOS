#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define STACK_SIZE 0x4000

static uint32_t g_words[128];
static uint8_t g_stk_a[STACK_SIZE];
static uint8_t g_stk_b[STACK_SIZE];
static volatile uint32_t g_a_woke;
static volatile uint32_t g_b_woke;

static void sleep_ms(uint32_t ms) {
    struct SYS_TIMESPEC ts;
    ts.tv_sec = (int32_t)(ms / 1000u);
    ts.tv_nsec = (int32_t)((ms % 1000u) * 1000000u);
    nanosleep(&ts, 0);
}

static int wait_a(void *arg) {
    (void)arg;
    futex((uint32_t)(uintptr_t)&g_words[0], FUTEX_WAIT, 0x1111u, 0);
    g_a_woke = 1;
    return 0;
}

static int wait_b(void *arg) {
    (void)arg;
    futex((uint32_t)(uintptr_t)&g_words[64], FUTEX_WAIT, 0x2222u, 0);
    g_b_woke = 1;
    return 0;
}

int main(void) {
    g_words[0] = 0x1111u;
    g_words[64] = 0x2222u;
    g_a_woke = 0;
    g_b_woke = 0;

    if (clone(wait_b, g_stk_b + STACK_SIZE, CLONE_VM | CLONE_THREAD, 0) < 0) {
        printf("futex_probe: clone B failed\n");
        return 1;
    }
    sleep_ms(200);
    if (clone(wait_a, g_stk_a + STACK_SIZE, CLONE_VM | CLONE_THREAD, 0) < 0) {
        printf("futex_probe: clone A failed\n");
        return 1;
    }
    sleep_ms(300);

    int32_t w1 = futex((uint32_t)(uintptr_t)&g_words[0], FUTEX_WAKE, 1, 0);
    sleep_ms(300);
    printf("futex_probe: wake(key0)=%d a=%d b=%d\n", (int)w1, (int)g_a_woke,
           (int)g_b_woke);
    int ok1 = (w1 == 1 && g_a_woke == 1 && g_b_woke == 0);

    int32_t w2 = futex((uint32_t)(uintptr_t)&g_words[64], FUTEX_WAKE, 1, 0);
    sleep_ms(300);
    printf("futex_probe: wake(key1)=%d b=%d\n", (int)w2, (int)g_b_woke);
    int ok2 = (w2 == 1 && g_b_woke == 1);

    if (ok1 && ok2) {
        printf("futex_probe: PASS\n");
        return 0;
    }
    printf("futex_probe: FAIL\n");
    return 1;
}
