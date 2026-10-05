#include "libc/user/stdio.h"
#include "syscall.h"

int main(void) {
    char buf[64];
    for (;;) {
        int32_t n = read(0, buf, sizeof buf);
        if (n <= 0)
            break;
        write(1, buf, (uint32_t)n);
    }
    return 0;
}
