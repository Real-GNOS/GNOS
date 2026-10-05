#include "drivers/net/arp.h"

#include "lib/str/str.h"
#include "drivers/net/eth.h"
#include "drivers/net/ip.h"
#include "drivers/net/net.h"

static struct ARP_ENTRY s_cache[ARP_CACHE_SIZE];
static uint32_t s_tick;

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        s_cache[i].valid = 0;
    s_tick = 0;
}

static int arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].ip == ip)
            return i;
    }
    return -1;
}

static int arp_slot(void) {
    int oldest = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!s_cache[i].valid)
            return i;
        if (s_cache[i].tmo < s_cache[oldest].tmo)
            oldest = i;
    }
    return oldest;
}

static void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    int idx = arp_find(ip);
    if (idx < 0)
        idx = arp_slot();
    s_cache[idx].ip = ip;
    memcpy(s_cache[idx].mac, mac, 6);
    s_cache[idx].tmo = s_tick;
    s_cache[idx].valid = 1;
}

static int arp_build(uint8_t *p, NETIF *ifp, uint32_t op, const uint8_t *tha,
                     uint32_t spa, uint32_t ipa) {
    net_put16(p + 0, ARP_HTYPE_ETH);
    net_put16(p + 2, ARP_PTYPE_IP);
    p[4] = ARP_HLEN;
    p[5] = ARP_PLEN;
    net_put16(p + 6, (uint16_t)op);
    memcpy(p + 8, ifp->mac, 6);
    net_put32(p + 14, spa);
    memcpy(p + 18, tha, 6);
    net_put32(p + 24, ipa);
    return 0;
}

void arp_input(NETIF *ifp, const uint8_t *pkt, uint32_t len) {
    if (len < 28)
        return;
    if (net_be16(pkt + 0) != ARP_HTYPE_ETH)
        return;
    if (net_be16(pkt + 2) != ARP_PTYPE_IP)
        return;
    if (pkt[4] != ARP_HLEN || pkt[5] != ARP_PLEN)
        return;
    uint16_t op = net_be16(pkt + 6);
    uint32_t spa = net_be32(pkt + 14);

    lock_acquire(&net_lock);
    if (spa)
        arp_cache_add(spa, pkt + 8);
    int known = (op == ARP_OP_REPLY) && (arp_find(spa) >= 0);
    lock_release(&net_lock);

    if (op == ARP_OP_REQUEST) {
        uint32_t tpa = net_be32(pkt + 24);
        if (tpa != ifp->ip)
            return;
        uint8_t rep[28];
        arp_build(rep, ifp, ARP_OP_REPLY, pkt + 8, ifp->ip, spa);
        eth_output(ifp, pkt + 8, ETH_ARP, rep, sizeof rep);
    } else if (known) {
        ip_arp_resolved(ifp, spa, pkt + 8);
    }
}

int arp_resolve(NETIF *ifp, uint32_t ip, uint8_t *out_mac) {
    lock_acquire(&net_lock);
    int idx = arp_find(ip);
    if (idx >= 0) {
        memcpy(out_mac, s_cache[idx].mac, 6);
        lock_release(&net_lock);
        return 0;
    }
    lock_release(&net_lock);

    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t req[28];
    arp_build(req, ifp, ARP_OP_REQUEST, bcast, ifp->ip, ip);
    eth_output(ifp, bcast, ETH_ARP, req, sizeof req);
    return -1;
}

void arp_tick(NETIF *ifp) {
    (void)ifp;
    lock_acquire(&net_lock);
    s_tick++;
    lock_release(&net_lock);
}
