#include "libc/user/stdio.h"
#include "syscall.h"

int main(void) {
    int32_t pid = fork();
    if (pid > 0) {
        printf("P: orphan child pid=%d\n", (int)pid);
        int32_t pid2 = fork();
        if (pid2 > 0) {
            printf("P: forking waiter for pid=%d\n", (int)pid2);
            int32_t st = -999;
            int32_t got = wait(&st);
            printf("P: wait reaped pid=%d status=%d\n", (int)got, (int)st);
            exit(0);
        } else if (pid2 == 0) {
            printf("C2: exiting 42\n");
            exit(42);
        }
        printf("P: second fork failed\n");
        exit(0);
    } else if (pid == 0) {
        printf("C1: orphan child running\n");
        for (volatile int i = 0; i < 20000000; i++) {
        }
        printf("C1: orphan exiting 7\n");
        exit(7);
    }
    return 0;
}
