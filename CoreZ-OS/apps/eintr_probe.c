#include "signal.h"
#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define EINTR_CODE (-4)

static volatile int g_caught = -1;

static void on_sigusr1(int sig) {
    g_caught = sig;
}

static void sleep_ms(uint32_t ms) {
    struct SYS_TIMESPEC ts;
    ts.tv_sec = (int32_t)(ms / 1000u);
    ts.tv_nsec = (int32_t)((ms % 1000u) * 1000000u);
    nanosleep(&ts, 0);
}

int main(void) {
    struct SYS_SIGACTION act;
    act.sa_handler = on_sigusr1;
    act.sa_mask = 0;
    act.sa_flags = 0;
    act.sa_restorer = 0;
    sigaction(SIGUSR1, &act, 0);

    int32_t me = (int32_t)getpid();
    int32_t pid = fork();
    if (pid == 0) {
        sleep_ms(300);
        kill(me, SIGUSR1);
        exit(0);
    }

    struct SYS_TIMESPEC req;
    req.tv_sec = 3;
    req.tv_nsec = 0;
    struct SYS_TIMESPEC rem;
    rem.tv_sec = -1;
    rem.tv_nsec = -1;
    int32_t r = nanosleep(&req, &rem);

    printf("eintr_probe: nanosleep -> %d (want %d)\n", (int)r, EINTR_CODE);
    printf("eintr_probe: rem sec=%d nsec=%d\n", (int)rem.tv_sec,
           (int)rem.tv_nsec);
    printf("eintr_probe: handler sig=%d\n", g_caught);

    int st = 0;
    wait(&st);

    if (r == EINTR_CODE && g_caught == SIGUSR1 && rem.tv_sec == 2 &&
        rem.tv_nsec > 0) {
        printf("eintr_probe: PASS\n");
        return 0;
    }
    printf("eintr_probe: FAIL\n");
    return 1;
}
