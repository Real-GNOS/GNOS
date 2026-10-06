/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bufoktest.c — the user-buffer probe regression test. (GPLv2, musl)
 *
 * writev(2) with a garbage iovec was the syscall that could take the machine
 * down: the range check passed (the range sits in the user half), the kernel
 * copied until its cursor walked off the end of the process image, and the
 * ring-0 page fault on that unmapped address fell through fault_back_lazy()
 * -- nobody's record covers it -- into fault_halt(), freezing the boot.
 * mount(8) hit exactly that shape through a stdio flush whose iovec ran
 * ~4 MB past the image end.
 *
 * user_buf_ok() (vmm.c) now walks every page of a copy range before the
 * kernel touches it: present with the permissions the copy needs, or
 * covered by an mmap record fault_back_lazy() could honour on the spot.
 * Anything else comes back as -EINVAL instead of a fault.  This drives the
 * shapes deliberately:
 *
 *   1. writev whose length spans the image end -> EINVAL, machine alive
 *   2. write() from an address nobody owns     -> EINVAL, machine alive
 *   3. readv with a wild iovec                 -> EINVAL, and no blocking
 *      read ever happens (the probe runs first)
 *   4. a well-formed writev still succeeds     -> the probe is not a wall
 *
 * Cases 1 and 2 are the pre-fix machine killers; if the probe ever regresses
 * they freeze the boot and `make test` loses its agetty assertions, and if
 * it over-rejects, case 4 (or the FAIL verdicts) trips the FAIL grep.
 *
 * Verdicts go to the debug console via dbgputs(GNOS private) for headless
 * `make test` runs, exactly like the other *test programs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static int  g_failed;
static void report(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    syscall(1001, buf);
}

static void check(int ok, int n, const char *what)
{
    report("BUFOKTEST: %s %d (%s) errno=%d", ok ? "PASS" : "FAIL", n, what, errno);
    if (!ok && !g_failed)
        g_failed = n;
}

int main(void)
{
    static const char msg[] = "bufoktest says hi";

    /* 1. A length that runs off the image end: EINVAL, not a frozen boot. */
    struct iovec iov = {(void *)msg, 64u * 1024 * 1024};
    errno            = 0;
    ssize_t r        = writev(2, &iov, 1);
    check(r == -1 && errno == EINVAL, 1, "writev past image end -> EINVAL");

    /* 2. An address nobody owns at all (image ends near 0x407000; brk and
     *    mmap arenas live at 0x5000_0000_0000 and up). */
    errno = 0;
    r     = write(2, (const void *)(uintptr_t)0x1234000, 64);
    check(r == -1 && errno == EINVAL, 2, "write from wild address -> EINVAL");

    /* 3. The probe runs before any transfer, so this cannot block on the
     *    console even though fd 0 is a tty. */
    errno = 0;
    r     = readv(0, &iov, 1);
    check(r == -1 && errno == EINVAL, 3, "readv wild iovec -> EINVAL");

    /* 4. The well-formed path must still work. */
    iov.iov_base = (void *)msg;
    iov.iov_len  = sizeof msg - 1;
    errno        = 0;
    r            = writev(2, &iov, 1);
    check(r == (ssize_t)sizeof msg - 1, 4, "well-formed writev succeeds");

    report("BUFOKTEST: done (%s)", g_failed ? "FAILURES" : "PASS");
    return g_failed ? 1 : 0;
}
