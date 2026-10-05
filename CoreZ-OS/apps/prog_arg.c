#include "libc/user/stdio.h"
#include "syscall.h"

int main(int argc, char **argv) {
    int arg_idx = 0;
    while (arg_idx < argc) {
        printf("argv[%d] is %s.\n", arg_idx, argv[arg_idx]);
        ++arg_idx;
    }
    printf("prog_arg: total %d arg(s), pid=%d\n", argc, (int)getpid());

    return 0;
}
