#include "libc/compat/lc.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    lc_puts("[gui] starting compositor...\n");
    __lc_syscall6(26, 0, 0, 0, 0, 0, 0);
    return 0;
}
