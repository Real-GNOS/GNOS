#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

int main(void) {
    uint32_t rfds[4];

    for (int i = 0; i < 4; i++) {
        rfds[i] = 0xFFFFFFFFu;
    }
    int32_t n1 = sock_select(128, rfds, 0, 0, 0);
    int high_cleared = (rfds[2] == 0u && rfds[3] == 0u);
    printf("sel_probe: select(128)=%d high_cleared=%d\n", (int)n1,
           high_cleared);

    int32_t n2 = sock_select(1025, rfds, 0, 0, 0);
    printf("sel_probe: select(1025)=%d (want -1)\n", (int)n2);

    for (int i = 0; i < 4; i++) {
        rfds[i] = 0xFFFFFFFFu;
    }
    int32_t n3 = sock_select(4, rfds, 0, 0, 0);
    printf("sel_probe: select(4)=%d low=%d\n", (int)n3, (int)rfds[0]);

    if (n1 >= 0 && high_cleared && n2 == -1 && n3 >= 0) {
        printf("sel_probe: PASS\n");
        return 0;
    }
    printf("sel_probe: FAIL\n");
    return 1;
}
