/* SPDX-License-Identifier: GPL-2.0 */
/* memtest.c — userland memory-management behaviour, checked end to end.
 *
 * Each case exercises a path a broken VM layer gets wrong: an anonymous
 * mapping that does not read back as zero, a MAP_SHARED page that fails to
 * carry a write across fork, a munmap that leaves the mapping live, and a
 * heap that cannot grow through repeated malloc.  Failures print FAIL and
 * set the exit status.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-38s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* A fresh anonymous mapping must read back as zeros, accept writes and
 * keep them, and disappear on munmap (a second munmap fails with EINVAL). */
static void test_anon_mmap(void)
{
    unsigned char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("  mmap failed: errno=%d\n", errno);
        fails++;
        return;
    }
    int zero = 1;
    for (int i = 0; i < 8192; i++)
        if (p[i])
            zero = 0;
    check("anon mmap reads as zero", zero);

    memset(p, 0x5A, 8192);
    check("anon mmap retains writes", p[0] == 0x5A && p[8191] == 0x5A);

    check("munmap succeeds", munmap(p, 8192) == 0);
    check("second munmap fails", munmap(p, 8192) == -1 && errno == EINVAL);
}

/* A MAP_SHARED page must carry a child's write back to the parent: the
 * mapping is the same physical memory in both address spaces. */
static void test_shared_across_fork(void)
{
    volatile int *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("  mmap shared failed: errno=%d\n", errno);
        fails++;
        return;
    }
    p[0] = 1;

    pid_t pid = fork();
    if (pid < 0) {
        printf("  fork failed: errno=%d\n", errno);
        fails++;
        return;
    }
    if (pid == 0) {
        p[0] = 42;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("MAP_SHARED write crosses fork", p[0] == 42);
    munmap((void *)p, 4096);
}

/* PROT_READ protection must actually fault: a write through a read-only
 * mapping dies with SIGSEGV, which the child reports via its wait status. */
static void test_protection(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("  mmap ro failed: errno=%d\n", errno);
        fails++;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        printf("  fork failed: errno=%d\n", errno);
        fails++;
        return;
    }
    if (pid == 0) {
        p[0] = 1; /* SIGSEGV expected */
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int segv = WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
    check("write to PROT_READ faults (SIGSEGV)", segv);
    munmap((void *)p, 4096);
}

/* The heap must survive sustained pressure: 256 allocations of varying
 * size, all written, all verified, all freed. */
static void test_heap_pressure(void)
{
    static void  *blocks[256];
    static size_t sizes[256];
    int           ok = 1;
    for (int i = 0; i < 256; i++) {
        sizes[i]  = (size_t)(i % 511) * 37 + 1;
        blocks[i] = malloc(sizes[i]);
        if (!blocks[i]) {
            ok = 0;
            break;
        }
        memset(blocks[i], i & 0xFF, sizes[i]);
    }
    check("heap: 256 allocations succeed", ok);
    if (ok)
        for (int i = 0; i < 256; i++)
            if (((unsigned char *)blocks[i])[sizes[i] - 1] != (unsigned char)(i & 0xFF))
                ok = 0;
    check("heap: contents survive pressure", ok);
    for (int i = 0; i < 256; i++)
        free(blocks[i]);
}

int main(void)
{
    printf("memtest: userland memory management\n");
    test_anon_mmap();
    test_shared_across_fork();
    test_protection();
    test_heap_pressure();
    printf("memtest: %s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
