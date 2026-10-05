#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MY_TIOCGPTN 0x8004540Fu
#define MY_TIOCSPTLCK 0x40045411u
#define MY_TIOCSCTTY 0x540Eu
#define MY_TIOCGPGRP 0x540fu

static int read1(int fd, char *c) {
    for (;;) {
        int r = read(fd, c, 1);
        if (r == 1)
            return 1;
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static volatile sig_atomic_t g_u;
static volatile sig_atomic_t g_w;

static void on_usr1(int sig) {
    (void)sig;
    g_u++;
    char b = 'U';
    write(2, &b, 1);
}

static void on_winch(int sig) {
    (void)sig;
    g_w++;
    char b = 'W';
    write(2, &b, 1);
}

static int handler_return_probe(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    int sentinel = 0x5A5A;
    g_u = 0;
    g_w = 0;
    for (int i = 0; i < 4; i++) {
        kill(getpid(), SIGUSR1);
        kill(getpid(), SIGWINCH);
    }
    char flush = 'F';
    write(2, &flush, 1);
    nanosleep(&(struct timespec){0, 30000000}, NULL);
    return (g_u > 0 && g_w > 0 && sentinel == 0x5A5A) ? 1 : 0;
}

int main(void) {
    int sigret = handler_return_probe();
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0)
        _exit(0x10);
    int n = -1;
    ioctl(m, MY_TIOCGPTN, &n);
    int unlock = 0;
    ioctl(m, MY_TIOCSPTLCK, &unlock);
    char sp[32];
    snprintf(sp, sizeof sp, "/dev/pts/%d", n);
    int s = open(sp, O_RDWR);
    if (s < 0)
        _exit(0x20);

    pid_t pid = fork();
    if (pid == 0) {
        close(m);
        setsid();
        ioctl(s, MY_TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        write(1, "S", 1);
        struct timespec ts = {0, 20000000};
        for (int i = 0; i < 500; i++)
            nanosleep(&ts, NULL);
        _exit(0x11); 
    }

    close(s);
    char c;
    if (read1(m, &c) != 1 || c != 'S')
        _exit(0x30);

    uint32_t fg = 0;
    ioctl(m, MY_TIOCGPGRP, &fg);
    int grp_ok = (fg == (uint32_t)pid);

    kill(-pid, SIGTERM);
    int st = 0;
    wait(&st);
    int term = WIFSIGNALED(st) ? WTERMSIG(st) : -1;
    int killed = (term == SIGTERM);

    char b[64];
    int ok = sigret && grp_ok && killed;
    int k = snprintf(b, sizeof b, "jc: sigret=%d grp=%d term=%d -> %s\n", sigret,
                     grp_ok, term, ok ? "PASS" : "FAIL");
    write(2, b, (size_t)k);
    _exit(ok ? 0 : 1);
}
