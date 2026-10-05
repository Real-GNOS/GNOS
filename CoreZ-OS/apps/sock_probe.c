#include "libc/user/stdio.h"
#include "libc/user/stdlib.h"
#include "syscall.h"

#define NR_socket 41u
#define NR_bind 49u
#define NR_getsockname 51u
#define NR_getsockopt 55u

#define AF_INET 2
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_ERROR 4
#define SO_SNDBUF 7
#define EFAULT (-14)

struct SOCKADDR_IN {
    uint16_t family;
    uint16_t port;
    uint32_t ip;
    uint8_t zero[8];
};

static int32_t raw_sys5(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3,
                        uint32_t a4, uint32_t a5) {
    int32_t ret;
    register uint32_t r10 __asm__("r10") = a4;
    register uint32_t r8 __asm__("r8") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

#define raw_sys4(nr, a1, a2, a3, a4) raw_sys5(nr, a1, a2, a3, a4, 0)

static void fill_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

int main(void) {
    static uint8_t buf[32];
    for (int i = 0; i < 32; i++) {
        buf[i] = 0x5A;
    }

    int32_t fd = raw_sys4(NR_socket, AF_INET, SOCK_DGRAM, 0, 0);
    printf("socket -> %d\n", fd);
    if (fd < 0) {
        exit(1);
    }

    struct SOCKADDR_IN sa;
    fill_zero(&sa, sizeof(sa));
    sa.family = AF_INET;
    sa.port = (uint16_t)((7777u >> 8) | (7777u << 8));
    sa.ip = 0x0100007Fu; 
    int32_t r = raw_sys4(NR_bind, (uint32_t)fd, (uint32_t)&sa, 16, 0);
    printf("bind -> %d\n", r);

    struct SOCKADDR_IN gn;
    uint32_t alen = sizeof(gn);
    r = raw_sys4(NR_getsockname, (uint32_t)fd, (uint32_t)&gn,
                 (uint32_t)&alen, 0);
    printf("getsockname -> %d alen=%d family=%d port=%d ip=%d.%d.%d.%d\n", r,
           (int)alen, (int)gn.family, (int)((gn.port >> 8) | (gn.port << 8)),
           gn.ip & 0xFF, (gn.ip >> 8) & 0xFF, (gn.ip >> 16) & 0xFF,
           (gn.ip >> 24) & 0xFF);

    fill_zero(&gn, sizeof(gn));
    for (int i = 0; i < 32; i++) {
        buf[i] = 0x5A;
    }
    alen = 4;
    r = raw_sys4(NR_getsockname, (uint32_t)fd, (uint32_t)buf,
                 (uint32_t)&alen, 0);
    int sentinel_ok = 1;
    for (int i = 4; i < 32; i++) {
        if (buf[i] != 0x5A) {
            sentinel_ok = 0;
        }
    }
    printf("getsockname small-buf -> %d alen=%d sentinel=%s\n", r, (int)alen,
           sentinel_ok ? "OK" : "CLOBBERED");

    r = raw_sys5(NR_getsockopt, (uint32_t)fd, SOL_SOCKET, SO_ERROR,
                 0xC02E0000u, (uint32_t)&alen);
    printf("getsockopt(kaddr) -> %d (want %d)\n", r, EFAULT);

    int32_t val = -1;
    alen = 4;
    r = raw_sys5(NR_getsockopt, (uint32_t)fd, SOL_SOCKET, SO_SNDBUF,
                 (uint32_t)&val, (uint32_t)&alen);
    printf("getsockopt(SO_SNDBUF) -> %d val=%d optlen=%d\n", r, (int)val,
           (int)alen);

    printf("sock_probe: ALIVE\n");
    exit(0);
    return 0;
}
