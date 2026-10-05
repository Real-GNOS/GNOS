#include "libc/user/stdio.h"
#include "libc/user/syscall.h"

#include <stdint.h>

uintptr_t __stack_chk_guard = 0x1f2e3d4c5b6a7900ULL;

__attribute__((no_stack_protector))
void __ssp_init(void) {
    __stack_chk_guard = ((uintptr_t)&__stack_chk_guard) * 1103515245UL;
    ((char *)&__stack_chk_guard)[0] = 0;
}

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    printf("[canary] stack smashing detected\n");
    exit(-1);
}
