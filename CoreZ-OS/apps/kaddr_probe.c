#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define LINUX_UNAME_NR 63u
#define LINUX_TIMES_NR 153u
#define LINUX_SYSINFO_NR 179u
#define LINUX_SIGALTSTACK_NR 131u

#define EFAULT (-14)

static int32_t raw_sys(uint32_t nr, uint32_t a1) {
    int32_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

int main(void) {
    uint32_t kaddr = 0xC02E0000u;

    int32_t r = raw_sys(LINUX_UNAME_NR, kaddr);
    printf("uname(kaddr) -> %d (want %d)\n", r, EFAULT);

    r = raw_sys(LINUX_SYSINFO_NR, kaddr);
    printf("sysinfo(kaddr) -> %d (want %d)\n", r, EFAULT);

    r = raw_sys(LINUX_TIMES_NR, kaddr);
    printf("times(kaddr) -> %d (want %d)\n", r, EFAULT);

    r = raw_sys(LINUX_SIGALTSTACK_NR, kaddr);
    printf("sigaltstack(kaddr) -> %d (want %d)\n", r, EFAULT);

    char ubuf[512];
    r = raw_sys(LINUX_UNAME_NR, (uint32_t)ubuf);
    printf("uname(valid) -> %d, sysname=%.5s\n", r, ubuf);

    printf("kaddr_probe: ALIVE\n");
    exit(0);
    return 0;
}
