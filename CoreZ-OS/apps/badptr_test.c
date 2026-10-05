#include "libc/user/stdio.h"
#include "kernel/syscall/mmap.h"
#include "syscall.h"

static unsigned int raw_syscall(unsigned int nr, unsigned int a, unsigned int b,
                        unsigned int c) {
    unsigned int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

int main(void) {
    static char buf[64];
    int ok = 1;
    unsigned int fd = raw_syscall(SYS_OPEN, (unsigned int)"/font_subset.ttf", 0, 0);
    struct {
        unsigned int nr;
        unsigned int a, b, c;
        unsigned int want;
    } cases[] = {
        {SYS_OPEN, 0x30000000u, 0, 0, 0xFFFFFFFFu},
        {SYS_OPEN, 0xC0003000u, 0, 0, 0xFFFFFFFFu},
        {SYS_READ, fd, 0x30000000u, 16, 0xFFFFFFFFu},
        {SYS_WRITE, 1, 0x30000000u, 16, 0xFFFFFFFFu},
        {SYS_WRITE, 1, 0xC0001000u, 16, 0xFFFFFFFFu},
        {SYS_STAT, 0x30000000u, (unsigned int)buf, 0, 0xFFFFFFFFu},
        {SYS_SIGACTION, 10, 0x30000000u, 0, 0xFFFFFFFFu},
        {SYS_FUTEX, 0x30000000u, 0, 0, 0xFFFFFFFFu},
        {SYS_CHDIR, 0x30000000u, 0, 0, 0xFFFFFFFFu},
        {SYS_NANOSLEEP, 0x30000000u, 0, 0, 0xFFFFFFFFu},
        {SYS_EXECV, (unsigned int)"/heap_demo", 0x30000000u, 0, 0xFFFFFFFFu},
        {SYS_EXECV, 0x30000000u, (unsigned int)buf, 0, 0xFFFFFFFFu},
    };
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned int got =
            raw_syscall(cases[i].nr, cases[i].a, cases[i].b, cases[i].c);
        printf("badptr: case %d got %d want %d\n", (int)i, (int)got,
               (int)cases[i].want);
        if (got != cases[i].want) {
            ok = 0;
        }
    }
    if (fd != 0xFFFFFFFFu) {
        raw_syscall(SYS_CLOSE, fd, 0, 0);
    }
    unsigned int h = raw_syscall(SYS_OPENDIR, (unsigned int)"/", 0, 0);
    if (h == 0 || h == 0xFFFFFFFFu) {
        printf("badptr: opendir broken\n");
        ok = 0;
    } else {
        if (raw_syscall(SYS_READDIR, 0x12345678u, 0, 0) != 0) {
            printf("badptr: bogus readdir not rejected\n");
            ok = 0;
        }
        if (raw_syscall(SYS_READDIR, h, 0, 0) == 0) {
            printf("badptr: valid readdir broken\n");
            ok = 0;
        }
        raw_syscall(SYS_REWINDDIR, 0x12345678u, 0, 0);
        raw_syscall(SYS_REWINDDIR, h, 0, 0);
        raw_syscall(SYS_CLOSEDIR, h, 0, 0);
    }
    unsigned int margs[6];
    margs[0] = 0x30000000u;
    margs[1] = 4096u;
    margs[2] = 3u;
    margs[3] = 0x10u;
    margs[4] = 0xFFFFFFFFu;
    margs[5] = 0;
    if (raw_syscall(SYS_MMAP, (unsigned int)margs, 0, 0) != 0x30000000u) {
        printf("badptr: valid mmap broken\n");
        ok = 0;
    }
    if (raw_syscall(SYS_MUNMAP, 0x30000000u, 4096u, 0) != 0) {
        printf("badptr: valid munmap broken\n");
        ok = 0;
    }
    struct {
        unsigned int nr, a, b, c, want;
    } edge[] = {
        {SYS_MMAP, (unsigned int)margs, 0, 0, 0xFFFFFFFFu},
        {SYS_MUNMAP, 0xC0000000u, 4096u, 0, 0xFFFFFFFFu},
        {SYS_MUNMAP, 0x40000000u, 4096u, 0, 0xFFFFFFFFu},
        {SYS_MUNMAP, 0xFFFFF000u, 0x100000u, 0, 0xFFFFFFFFu},
        {SYS_MPROTECT, 0x40000000u, 4096u, 3, 0xFFFFFFFFu},
        {SYS_WRITE, 999u, (unsigned int)buf, 1, 0xFFFFFFFFu},
    };
    margs[0] = 0xC0000000u;
    for (unsigned int i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        unsigned int got = raw_syscall(edge[i].nr, edge[i].a, edge[i].b, edge[i].c);
        printf("badptr: edge %d got %d want %d\n", (int)i, (int)got,
               (int)edge[i].want);
        if (got != edge[i].want) {
            ok = 0;
        }
    }
    printf("badptr: getpid=%d\n", (int)raw_syscall(SYS_GETPID, 0, 0, 0));
    printf("badptr: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
