#include <stdint.h>

#include "libc/user/stdio.h"
#include "syscall.h"

static uint64_t frame[23] __attribute__((aligned(16)));

static void fill_frame(void) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = (uint64_t)(uintptr_t)fill_frame;
    frame[3] = 0x33;
    frame[4] = 0x202;
    frame[5] = (uint64_t)sp;
    frame[6] = 0x23;
    for (int i = 7; i < 22; i++) {
        frame[i] = 0;
    }
    frame[22] = 0;
}

static void forge_sigreturn(void) {
    __asm__ volatile("mov %0, %%rsp\n\t"
                     "mov %1, %%eax\n\t"
                     "int $0x80\n\t"
                     "1:\n\t"
                     "jmp 1b\n\t"
                     :
                     : "r"((uint64_t)(uintptr_t)frame + 8),
                       "r"((unsigned int)SYS_SIGRETURN)
                     : "memory");
}

int main(void) {
    printf("sig_test: start\n");
    int ok = 1;
    for (int t = 0; t < 2; t++) {
        int pid = fork();
        if (pid == 0) {
            fill_frame();
            if (t == 0) {
                frame[3] = 0x08;
                printf("sig_test: firing forged cs=0x08 frame\n");
            } else {
                frame[4] = 0x3202;
                printf("sig_test: firing forged IOPL=3 frame\n");
            }
            forge_sigreturn();
        }
        int st = -1;
        wait(&st);
        printf("sig_test: case %d status=%d\n", t, st);
        if (st != 139) {
            ok = 0;
        }
    }
    printf("sig_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
