#include "signal.h"
#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define ITER 50000

static volatile int g_caught = -1;

static void handler(int sig) {
    printf("[signal] handler entered for signal %d\n", sig);
    g_caught = sig;
}

static void spin_until_caught(const char *label) {
    for (int i = 0; i < ITER && g_caught < 0; i++) {
        getpid();
    }
    if (g_caught < 0) {
        printf("[signal] FAIL: %s not delivered\n", label);
    } else {
        printf("[signal] ok: %s delivered (sig=%d)\n", label, g_caught);
    }
}

int main(void) {
    struct SYS_SIGACTION act;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = 0;
    act.sa_restorer = 0;

    printf("signal_demo: start\n");

    sigaction(SIGUSR1, &act, 0);
    sigaction(SIGUSR2, &act, 0);

    int parent_pid = getpid();
    pid_t pid = fork();
    if (pid == 0) {

        kill(parent_pid, SIGUSR1);
        exit(0);
    }

    g_caught = -1;
    spin_until_caught("SIGUSR1 (from child)");
    int from_child = g_caught;

    g_caught = -1;
    kill(getpid(), SIGUSR2);
    spin_until_caught("SIGUSR2 (self)");
    int from_self = g_caught;

    int st;
    wait(&st);

    if (from_child == SIGUSR1 && from_self == SIGUSR2) {
        printf("signal_demo: PASS\n");
        exit(0);
    }
    printf("signal_demo: FAIL (child=%d self=%d)\n", from_child, from_self);
    exit(1);
}
