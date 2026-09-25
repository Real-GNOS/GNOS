/*
 * test.c — the shell-side smoke program for the guest toolchain. (GPLv2)
 *
 * Compiled and run by /etc/rc (`clang /root/test.c -o /tmp/test`), which is
 * how the clang install is regression-tested: the driver spawns cc1 and ld
 * through posix_spawn, so this also exercises vfork, /proc/self/exe and the
 * ELF loader.
 */
#include <stdio.h>

int main(void)
{
    printf("hello from GNOS: clang works\n");
    return 42;
}
