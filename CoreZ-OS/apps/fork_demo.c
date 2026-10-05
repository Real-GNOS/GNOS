#include "libc/user/stdio.h"
#include "syscall.h"

int main(void) {
    int32_t pid = fork();
    if (pid > 0) {
        int32_t st = -1;
        printf("parent: fork returned %d\n", pid);
        int32_t got = wait(&st);
        printf("parent: child %d exited, status %d\n", (int)got, (int)st);
    } else if (pid == 0) {
        printf("child: fork returned 0, my pid=%d\n", (int)getpid());
        exit(66);
    } else {
        printf("fork failed\n");
    }
    return 0;
}
