#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

int main(void) {
    printf("gs_probe: touching gs:0\n");
    uint64_t v = 0;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(v) : : "memory");
    printf("gs_probe: read gs:0 = %llx (no fault!)\n", (uint32_t)v);
    exit(0);
    return 0;
}
