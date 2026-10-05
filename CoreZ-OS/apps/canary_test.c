#include "libc/user/stdio.h"

int main(void) {
    printf("[canary_test] overflow start\n");
    char buf[8];
    for (int i = 0; i < 128; i++) {
        buf[i] = 0x41;
    }
    printf("[canary_test] returned past overflow\n");
    return 0;
}
