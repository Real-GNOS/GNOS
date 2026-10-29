/* SPDX-License-Identifier: GPL-2.0 */
/* nettest.c — userland network stack behaviour, checked end to end.
 *
 * Three paths the loopback stack must get right: AF_UNIX socketpair
 * byte-exchange in both directions, a loopback TCP connection through
 * listen/accept/connect with payload both ways, and a UDP datagram
 * exchange on 127.0.0.1.  Failures print FAIL and set the exit status.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-38s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

/* socketpair: one kernel buffer, two ends, both directions, then EOF. */
static void test_socketpair(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("  socketpair failed: errno=%d\n", errno);
        fails++;
        return;
    }
    check("socketpair: write A->B", write(sv[0], "ping", 4) == 4);
    char buf[16] = {0};
    check("socketpair: read B gets it", read(sv[1], buf, sizeof buf) == 4);
    check("socketpair: payload intact", memcmp(buf, "ping", 4) == 0);

    check("socketpair: write B->A", write(sv[1], "pong", 4) == 4);
    memset(buf, 0, sizeof buf);
    check("socketpair: read A gets it", read(sv[0], buf, sizeof buf) == 4);
    check("socketpair: payload intact", memcmp(buf, "pong", 4) == 0);

    close(sv[0]);
    check("socketpair: EOF after close", read(sv[1], buf, sizeof buf) == 0);
    close(sv[1]);
}

/* Loopback TCP: server thread binds 127.0.0.1:4390, child connects, both
 * sides exchange a payload and shut down cleanly. */
static void test_tcp_loopback(void)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        printf("  socket failed: errno=%d\n", errno);
        fails++;
        return;
    }
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = htons(4390);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 1) < 0) {
        printf("  bind/listen failed: errno=%d\n", errno);
        fails++;
        close(lfd);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("  fork failed: errno=%d\n", errno);
        fails++;
        close(lfd);
        return;
    }
    if (pid == 0) {
        int cfd = socket(AF_INET, SOCK_STREAM, 0);
        if (cfd < 0)
            _exit(1);
        if (connect(cfd, (struct sockaddr *)&a, sizeof a) < 0)
            _exit(2);
        if (write(cfd, "hello", 5) != 5)
            _exit(3);
        char rb[8] = {0};
        int  r     = read(cfd, rb, sizeof rb);
        if (r != 3 || memcmp(rb, "ack", 3) != 0)
            _exit(4);
        close(cfd);
        _exit(0);
    }

    int cfd = accept(lfd, NULL, NULL);
    int ok  = cfd >= 0;
    check("tcp: accept returns a connection", ok);
    if (ok) {
        char buf[8] = {0};
        ok          = read(cfd, buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0;
        check("tcp: server receives payload", ok);
        check("tcp: server replies", write(cfd, "ack", 3) == 3);
        close(cfd);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("tcp: client round-trip clean", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(lfd);
}

/* UDP on loopback: a datagram sent to a bound port arrives byte-exact,
 * with recvfrom reporting the sender's address. */
static void test_udp_loopback(void)
{
    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) {
        printf("  udp socket failed: errno=%d\n", errno);
        fails++;
        return;
    }
    struct sockaddr_in a = {0};
    a.sin_family         = AF_INET;
    a.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    a.sin_port           = htons(4391);
    check("udp: bind 127.0.0.1:4391", bind(ufd, (struct sockaddr *)&a, sizeof a) == 0);

    struct sockaddr_in dst = a;
    check("udp: sendto", sendto(ufd, "datagram", 8, 0, (struct sockaddr *)&dst, sizeof dst) == 8);

    struct sockaddr_in src     = {0};
    socklen_t          slen    = sizeof src;
    char               buf[16] = {0};
    int                r       = recvfrom(ufd, buf, sizeof buf, 0, (struct sockaddr *)&src, &slen);
    check("udp: recvfrom gets 8 bytes", r == 8);
    check("udp: payload intact", memcmp(buf, "datagram", 8) == 0);
    check("udp: sender address reported", src.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(ufd);
}

int main(void)
{
    printf("nettest: userland network stack\n");
    test_socketpair();
    test_tcp_loopback();
    test_udp_loopback();
    printf("nettest: %s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
