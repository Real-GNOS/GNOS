/*
 * wiggle.c — inject a small mouse movement (GPLv2, musl).
 *
 * Headless Xorg test helper: the DIX pointer sprite is only materialised
 * once the pointer has moved, so rc runs this after the server is up to
 * make the X cursor appear on screen without a real PS/2 device.
 */
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define EV_REL       0x02
#define EV_SYN       0x00
#define REL_X        0x00
#define REL_Y        0x01
#define SYN_REPORT   0x00
#define INJECT       405        /* SYS_inputinject, see src/shared/sysnum.h */

static void rel(int code, int value)
{
    syscall(INJECT, (long)EV_REL, code, value);
}

static void sync_frame(void)
{
    syscall(INJECT, (long)EV_SYN, SYN_REPORT, 0);
}

int main(void)
{
    /* A triangle wave: drift right-down, then back, so the sprite is
     * drawn at several positions. */
    for (int i = 0; i < 25; i++) {
        rel(REL_X, 8);
        rel(REL_Y, (i < 12) ? 6 : -6);
        sync_frame();
        usleep(30000);
    }
    for (int i = 0; i < 25; i++) {
        rel(REL_X, -8);
        rel(REL_Y, (i < 12) ? -6 : 6);
        sync_frame();
        usleep(30000);
    }
    printf("wiggle: done\n");
    return 0;
}
