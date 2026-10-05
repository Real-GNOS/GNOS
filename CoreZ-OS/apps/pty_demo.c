#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define MY_TIOCGPTN 0x8004540Fu
#define MY_TIOCSPTLCK 0x40045411u

static int readn(int fd, void *buf, int n) {
    int got = 0;
    char *b = buf;
    while (got < n) {
        int r = read(fd, b + got, (size_t)(n - got));
        if (r <= 0)
            break;
        got += r;
    }
    return got;
}

static void fail(const char *why) {
    char b[64];
    int k = snprintf(b, sizeof b, "pty_demo: FAIL %s\n", why);
    write(2, b, (size_t)k);
    _exit(1);
}

int main(void) {
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0)
        fail("open ptmx");

    int n = -1;
    if (ioctl(m, MY_TIOCGPTN, &n) < 0 || n < 0)
        fail("TIOCGPTN");
    int unlock = 0;
    ioctl(m, MY_TIOCSPTLCK, &unlock);

    char sp[32];
    snprintf(sp, sizeof sp, "/dev/pts/%d", n);
    int s = open(sp, O_RDWR);
    if (s < 0)
        fail("open /dev/pts slave");

    fcntl(s, F_SETFL, O_NONBLOCK);
    char probe;
    errno = 0;
    int pr = read(s, &probe, 1);
    if (pr != -1 || errno != EAGAIN)
        fail("nonblock EAGAIN");
    fcntl(s, F_SETFL, 0);

    pid_t pid = fork();
    if (pid == 0) {
        close(m);
        char c = 0;
        if (read(s, &c, 1) != 1 || c != 'A')
            _exit(4);
        if (write(s, "BA", 2) != 2)
            _exit(5);
        close(s);
        _exit(0);
    }

    close(s);
    if (write(m, "A", 1) != 1)
        fail("master write");
    char rb[4] = {0};
    int got = readn(m, rb, 2);
    int st = 0;
    wait(&st);
    if (got != 2 || rb[0] != 'B' || rb[1] != 'A')
        fail("roundtrip");

    char z;
    int er = read(m, &z, 1);
    if (er != 0)
        fail("EOF after hangup");

    write(1, "pty_demo: PASS\n", 15);
    _exit(0);
}
