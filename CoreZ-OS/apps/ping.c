#include "libc/user/stdio.h"
#include "lib/str/str.h"
#include "syscall.h"

#define PING_ID 0x1234
#define PING_MAX 4
#define PING_TIMEOUT_MS 2000

static uint32_t parse_ip(const char *s) {
    uint32_t ip = 0;
    int octets = 0;
    for (;;) {
        if (*s < '0' || *s > '9')
            return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return 0;
            s++;
        }
        ip = (ip << 8) | v;
        octets++;
        if (*s == '.') {
            s++;
        } else if (*s == 0) {
            break;
        } else {
            return 0;
        }
    }
    return (octets == 4) ? ip : 0;
}

static uint32_t now_ms(void) {
    struct SYS_TIMESPEC tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (uint32_t)tp.tv_sec * 1000u + (uint32_t)(tp.tv_nsec / 1000000u);
}

static uint32_t ip_to_ipv4_str(uint32_t ip, char *out) {
    uint32_t n = sprintf(out, "%d.%d.%d.%d", (int)((ip >> 24) & 0xff),
                         (int)((ip >> 16) & 0xff), (int)((ip >> 8) & 0xff),
                         (int)(ip & 0xff));
    return n;
}

static int wait_reply(uint16_t id, uint16_t seq, uint32_t *rtt) {
    uint32_t t0 = now_ms();
    struct NET_PING_REPLY rep[4];
    for (;;) {
        int n = icmp_recv(rep, 4);
        for (int i = 0; i < n; i++) {
            if (rep[i].id == id && rep[i].seq == seq) {
                *rtt = rep[i].rtt_ms;
                return 1;
            }
        }
        if (now_ms() - t0 >= PING_TIMEOUT_MS)
            return 0;
        struct SYS_TIMESPEC sl = {0, 200 * 1000 * 1000};
        nanosleep(&sl, 0);
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argv[1] == NULL) {
        printf("ping: argument error\neg: ping 10.0.2.2\n");
        exit(-2);
    }
    uint32_t dst = parse_ip(argv[1]);
    if (dst == 0) {
        printf("ping: invalid IP \"%s\"\n", argv[1]);
        exit(-2);
    }

    uint32_t dst_host = dst;
    char ipstr[16];
    ip_to_ipv4_str(dst, ipstr);
    printf("PING %s: 56 data bytes\n", ipstr);

    uint32_t min_ms = 0xffffffffu, max_ms = 0, sum_ms = 0;
    int sent = 0, recv = 0;
    for (uint16_t seq = 1; sent < PING_MAX; seq++) {
        uint32_t t_send = now_ms();
        if (icmp_send(dst_host, PING_ID, seq) >= 0)
            sent++;
        uint32_t rtt = 0;
        if (wait_reply(PING_ID, seq, &rtt)) {
            recv++;
            if (rtt < min_ms)
                min_ms = rtt;
            if (rtt > max_ms)
                max_ms = rtt;
            sum_ms += rtt;
            printf("64 bytes from %s: icmp_seq=%d rtt=%d ms\n", ipstr, (int)seq,
                   (int)rtt);
        } else {
            printf("Request timeout for icmp_seq=%d\n", (int)seq);
        }

        (void)t_send;
        struct SYS_TIMESPEC gap = {0, 300 * 1000 * 1000};
        nanosleep(&gap, 0);
    }

    int lost = sent - recv;
    printf("--- %s ping statistics ---\n", ipstr);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", sent,
           recv, sent > 0 ? (lost * 100 / sent) : 0);
    if (recv > 0) {
        printf("rtt min/avg/max = %d/%d/%d ms\n", (int)min_ms,
               (int)(sum_ms / (uint32_t)recv), (int)max_ms);
    }
    exit(0);
    return 0;
}
