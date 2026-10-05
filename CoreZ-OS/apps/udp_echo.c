#include "libc/user/stdio.h"
#include "lib/str/str.h"
#include "syscall.h"

#define AF_INET 2
#define SOCK_DGRAM 2
#define ECHO_PORT 8765
#define SELF_IP 0x0A00020Fu

static uint16_t parse_port(const char *s) {
    uint32_t v = 0;
    if (!s)
        return ECHO_PORT;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > 65535)
            return ECHO_PORT;
        s++;
    }
    return (uint16_t)v;
}

static void fmt_ip(uint32_t ip, char *out) {
    sprintf(out, "%d.%d.%d.%d", (int)((ip >> 24) & 0xff),
            (int)((ip >> 16) & 0xff), (int)((ip >> 8) & 0xff),
            (int)(ip & 0xff));
}

int main(int argc, char **argv) {
    uint16_t port = parse_port(argc >= 2 ? argv[1] : 0);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("udp_echo: socket failed\n");
        exit(-1);
    }
    if (sock_bind(fd, 0, port) < 0) {
        printf("udp_echo: bind port %d failed\n", (int)port);
        exit(-1);
    }
    printf("udp_echo: listening on udp://0.0.0.0:%d\n", (int)port);

    static const char s_msg[] = "selftest";
    if (sock_sendto(fd, s_msg, sizeof(s_msg) - 1, SELF_IP, port) < 0) {
        printf("udp_echo: selftest send failed\n");
        exit(-1);
    }
    char sbuf[64];
    uint32_t saddr = 0;
    uint16_t sport = 0;
    int n = sock_recvfrom(fd, sbuf, sizeof sbuf, &saddr, &sport);
    if (n < 0) {
        printf("udp_echo: selftest recv timeout\n");
        exit(-1);
    }
    printf("udp_echo: selftest got %d bytes: %.254s\n", n, sbuf);

    for (;;) {
        char buf[256];
        uint32_t saddr = 0;
        uint16_t sport = 0;
        int n = sock_recvfrom(fd, buf, sizeof buf, &saddr, &sport);
        if (n < 0)
            continue;
        char ipstr[16];
        fmt_ip(saddr, ipstr);
        uint16_t sport_host =
            (uint16_t)(((sport & 0xff) << 8) | ((sport >> 8) & 0xff));
        printf("udp_echo: %d bytes from %s:%d\n", n, ipstr, (int)sport_host);
        if (sock_sendto(fd, buf, (uint32_t)n, saddr, sport) < 0)
            printf("udp_echo: sendto failed\n");
    }
}
