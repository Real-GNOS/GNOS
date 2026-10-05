#include "drivers/net/net.h"

#include "arch/interrupt/interrupt.h"
#include "drivers/char/console/io.h"
#include "arch/cpu.h"
#include "kernel/init/pit/pit.h"
#include "lib/str/str.h"
#include "kernel/sched/thread.h"
#include "drivers/net/arp.h"
#include "drivers/net/e1000.h"
#include "drivers/net/eth.h"
#include "drivers/net/ip.h"
#include "drivers/net/rtl8139.h"
#include "drivers/net/socket.h"
#include "drivers/net/tcp.h"
#include "drivers/net/udp.h"

NETIF g_netif;

struct SCHED_LOCK net_lock;

static void net_probe(NETIF *ifp) {
    ifp->tx = rtl8139_tx;
    ifp->rx = rtl8139_rx;
    if (rtl8139_init(ifp) == 0)
        return;
    ifp->tx = e1000_tx;
    ifp->rx = e1000_rx;
    if (e1000_init(ifp) == 0)
        return;
    kprintf("[NET] no supported NIC\n");
}

static void net_rx(NETIF *ifp) {
    static uint8_t buf[ETH_FRAME_MAX];
    int n;
    while ((n = ifp->rx(ifp, buf, sizeof buf)) > 0)
        eth_input(ifp, buf, (uint32_t)n);
}

static void net_thread(void *arg) {
    (void)arg;
    memset(&g_netif, 0, sizeof g_netif);
    g_netif.ip = 0x0A00020F;
    g_netif.mask = 0xFFFFFF00;
    g_netif.gw = 0x0A000202;
    net_probe(&g_netif);
    kprintf("[NET] up %u.%u.%u.%u\n", (g_netif.ip >> 24) & 0xFF,
            (g_netif.ip >> 16) & 0xFF, (g_netif.ip >> 8) & 0xFF,
            g_netif.ip & 0xFF);
    for (;;) {
        net_rx(&g_netif);
        arp_tick(&g_netif);
        tcp_tick(&g_netif);
        sock_poll();
        thread_yield();
    }
}

volatile uint32_t net_guard_before = 0x5A5A0001u;
int net_enable = 1;
volatile uint32_t net_guard_after = 0x5A5A0002u;

void net_check_guards(void) {
    if (net_guard_before != 0x5A5A0001u || net_guard_after != 0x5A5A0002u ||
        (net_enable != 0 && net_enable != 1)) {
        kprintf("\n[CORRUPT] net_enable=%d before=%x after=%x\n",
                net_enable, net_guard_before, net_guard_after);
        for (;;)
            cpu_hlt();
    }
}

void net_init(void) {
    if (!net_enable) {
        kprintf("[NET] disabled\n");
        return;
    }
    lock_init(&net_lock);
    arp_init();
    tcp_init();
    udp_init();
    sock_init();
    kernel_thread("net", 8, net_thread, NULL, 0xF);
}

uint32_t net_now_ms(void) {
    return (uint32_t)tick * (1000u / PIT_HZ);
}
