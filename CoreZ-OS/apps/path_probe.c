#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define PROBE_BASE 0x3FF00000u
#define PROBE_LEN 0x10000u

static void fill_nonzero(void) {
    volatile char *p = (volatile char *)PROBE_BASE;
    for (uint32_t i = 0; i < PROBE_LEN; i++) {
        p[i] = 'A';
    }
}

static void fill_tail_zero(void) {
    volatile char *p = (volatile char *)(PROBE_BASE + PROBE_LEN - 16);
    for (int i = 0; i < 16; i++) {
        p[i] = 0;
    }
}

int main(void) {
    void *m = mmap((void *)PROBE_BASE, PROBE_LEN, PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    if (m == (void *)PROBE_BASE) {
        printf("mmap ok at 0x%x\n", PROBE_BASE);
    } else {
        printf("mmap failed (got %d)\n", (int)(uintptr_t)m);
        exit(1);
    }
    fill_nonzero();

    int r = access((const char *)PROBE_BASE, 0);
    printf("access overrun -> %d (want -1)\n", r);

    char buf[64];
    r = readlink((const char *)PROBE_BASE, buf, sizeof(buf));
    printf("readlink overrun -> %d (want -1)\n", r);

    r = chmod((const char *)PROBE_BASE, 0644);
    printf("chmod overrun -> %d (want -1)\n", r);

    fill_tail_zero();
    r = access((const char *)PROBE_BASE, 0);
    printf("access terminated -> %d (want -1)\n", r);

    printf("path_probe: ALIVE\n");
    exit(0);
    return 0;
}
