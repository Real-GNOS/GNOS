#include "libc/user/stdio.h"
#include "syscall.h"

static char tls_buf[64] __attribute__((aligned(16)));

static unsigned int raw_set_thread_area(unsigned int base) {
    unsigned int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((unsigned int)SYS_SET_THREAD_AREA), "b"(base)
                     : "memory");
    return ret;
}

int main(void) {
    unsigned int r1 = raw_set_thread_area(0xC0003000u);
    unsigned int r2 = raw_set_thread_area(0x40200000u);
    unsigned int r3 = raw_set_thread_area(0x30000000u);
    unsigned int r4 = raw_set_thread_area(0xBFFFFFFEu);
    unsigned int r5 = raw_set_thread_area(0u);
    unsigned int r6 = raw_set_thread_area((unsigned int)(uintptr_t)tls_buf);
    int zeroed = *(volatile int *)tls_buf == 0;
    printf("tls_test: kernel=%x mmio=%x unmap=%x straddle=%x null=%x "
           "valid=%x zeroed=%d\n",
           r1, r2, r3, r4, r5, r6, zeroed);
    if (r1 == 0xFFFFFFFFu && r2 == 0xFFFFFFFFu && r3 == 0xFFFFFFFFu &&
        r4 == 0xFFFFFFFFu && r5 == 0xFFFFFFFFu && r6 == 0u && zeroed) {
        printf("tls_test: PASS\n");
        return 0;
    }
    printf("tls_test: FAIL\n");
    return 1;
}
