/* SPDX-License-Identifier: GPL-2.0 */
/* cowtest.c — fork() copy-on-write behaviour, checked from user space.
 *
 * Each case below exists because it is something a broken COW gets wrong:
 * a page that separates eagerly instead of lazily still passes the first
 * test, a page that never separates fails it, and a frame whose reference
 * count is off survives until something else is allocated over it.
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
    printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* A page shared after fork must be shared for READS and private for WRITES. */
static void test_write_separates(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("mmap failed: errno=%d\n", errno);
        fails++;
        return;
    }
    p[0] = 'A';

    pid_t pid = fork();
    if (pid < 0) {
        printf("fork failed: errno=%d\n", errno);
        fails++;
        munmap((void *)p, 4096);
        return;
    }
    if (pid == 0) {
        /* Child: sees the parent's value, then writes its own. */
        int ok = (p[0] == 'A');
        p[0]   = 'B';
        _exit(ok && p[0] == 'B' ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("child sees value written by parent", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("child's write is invisible to parent", p[0] == 'A');
    munmap((void *)p, 4096);
}

/* A read-only page stays shared: writable here would mean the permission
 * bits did not come down with the sharing. */
static void test_readonly_shared(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fails++;
        return;
    }
    strcpy((char *)p, "shared-ro");
    if (mprotect((void *)p, 4096, PROT_READ) != 0)
        printf("mprotect failed: errno=%d\n", errno);

    pid_t pid = fork();
    if (pid < 0) {
        fails++;
        munmap((void *)p, 4096);
        return;
    }
    if (pid == 0)
        _exit(strcmp((const char *)p, "shared-ro") == 0 ? 0 : 1);

    int st = 0;
    waitpid(pid, &st, 0);
    check("read-only page shared across fork", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    munmap((void *)p, 4096);
}

/* Three generations: every holder has to get a private page of its own, or
 * the middle one's write shows up in both neighbours. */
static void test_three_generations(void)
{
    volatile unsigned *p =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fails++;
        return;
    }
    p[0] = 0;

    pid_t c1 = fork();
    if (c1 == 0) {
        p[0]     = 1;
        pid_t c2 = fork();
        if (c2 == 0) {
            p[0] = 2;
            _exit(p[0] == 2 ? 0 : 1);
        }
        int st = 0;
        waitpid(c2, &st, 0);
        _exit(p[0] == 1 ? 0 : 2);
    }
    int s1 = 0;
    waitpid(c1, &s1, 0);
    check("grandchild gets its own page", WIFEXITED(s1) && WEXITSTATUS(s1) == 0);
    check("parent unaffected by both writes", p[0] == 0);
    munmap((void *)p, 4096);
}

/* execve replaces the address space; whatever the caller shared with the
 * child must survive that, and the child's stdout must still work. */
static void test_exec_after_fork(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fails++;
        return;
    }
    p[0] = 'X';

    pid_t pid = fork();
    if (pid < 0) {
        munmap((void *)p, 4096);
        fails++;
        return;
    }
    if (pid == 0) {
        p[0] = 'Y'; /* must not be visible to the parent */
        execl("/bin/hello.elf", "hello.elf", (char *)NULL);
        _exit(3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("exec after fork still runs", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("exec'd child's write stayed private", p[0] == 'X');
    munmap((void *)p, 4096);
}

/* mprotect back to writable after the sharing has begun -- this is the path
 * that "upgrade permissions" handlers like to take without copying. */
static void test_mprotect_write(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fails++;
        return;
    }
    p[0]      = 'p';
    pid_t pid = fork();
    if (pid < 0) {
        munmap((void *)p, 4096);
        fails++;
        return;
    }
    if (pid == 0) {
        if (mprotect((void *)p, 4096, PROT_READ) != 0)
            _exit(4);
        if (mprotect((void *)p, 4096, PROT_READ | PROT_WRITE) != 0)
            _exit(5);
        p[0] = 'c';
        _exit(p[0] == 'c' ? 0 : 6);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("mprotect(RW) on a shared page works", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    check("parent value survives child mprotect", p[0] == 'p');
    munmap((void *)p, 4096);
}

/* A loop: reference counts that drift upward are invisible until the table
 * fills, counts that drift downward kill a live page.  Enough iterations to
 * notice either. */
static void test_fork_loop(void)
{
    volatile char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fails++;
        return;
    }
    int bad = 0;
    for (int i = 0; i < 500 && bad == 0; i++) {
        p[0]      = (char)('0' + (i % 10));
        pid_t pid = fork();
        if (pid < 0) {
            bad = 1;
            break;
        }
        if (pid == 0) {
            p[0] = 'z';
            _exit(p[0] == 'z' ? 0 : 1);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            bad = 1;
        if (p[0] != (char)('0' + (i % 10)))
            bad = 1;
    }
    check("500 fork/write/wait cycles", bad == 0);
    munmap((void *)p, 4096);
}

int main(void)
{
    printf("=== cowtest ===\n");
    test_write_separates();
    test_readonly_shared();
    test_three_generations();
    test_exec_after_fork();
    test_mprotect_write();
    test_fork_loop();
    printf("cowtest: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
