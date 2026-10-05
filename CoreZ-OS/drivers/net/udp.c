#include "drivers/net/udp.h"

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "drivers/net/ip.h"
#include "drivers/net/net.h"

#define UDP_MASK (UDP_RCV_BUF - 1)

static struct UDP_PCB s_upcb[MAX_UDP_PCB];

void udp_init(void) {
    memset(s_upcb, 0, sizeof s_upcb);
}

static uint16_t udp_sum(const uint8_t *pkt, uint32_t len, uint32_t saddr,
                        uint32_t daddr) {
    uint32_t sum = 0;
    sum += (saddr >> 16) & 0xffff;
    sum += saddr & 0xffff;
    sum += (daddr >> 16) & 0xffff;
    sum += daddr & 0xffff;
    sum += 0x0011;
    sum += (uint32_t)len;
    const uint8_t *p = pkt;
    uint32_t n = len;
    while (n > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (uint16_t)p[0] << 8;
    while (sum >> 16)
        sum = (uint16_t)sum + (sum >> 16);
    return (uint16_t)~sum;
}

struct UDP_PCB *udp_pcb_alloc(void) {
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_UDP_PCB; i++) {
        if (!s_upcb[i].active) {
            memset(&s_upcb[i], 0, sizeof s_upcb[i]);
            s_upcb[i].active = 1;
            lock_release(&net_lock);
            return &s_upcb[i];
        }
    }
    lock_release(&net_lock);
    return 0;
}

void udp_pcb_free(struct UDP_PCB *pcb) {
    lock_acquire(&net_lock);
    pcb->active = 0;
    lock_release(&net_lock);
}

int udp_bind(struct UDP_PCB *pcb, uint32_t ip, uint16_t port) {
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_UDP_PCB; i++) {
        struct UDP_PCB *o = &s_upcb[i];
        if (o->active && o != pcb && o->local_port == port) {
            lock_release(&net_lock);
            return -1;
        }
    }
    pcb->local_ip = ip;
    pcb->local_port = port;
    lock_release(&net_lock);
    return 0;
}

void udp_connect(struct UDP_PCB *pcb, uint32_t ip, uint16_t port) {
    lock_acquire(&net_lock);
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    lock_release(&net_lock);
}

int udp_sendto(NETIF *ifp, struct UDP_PCB *pcb, const void *data, uint32_t len,
               uint32_t daddr, uint16_t dport) {
    uint8_t seg[UDP_HDR_LEN + UDP_PAYLOAD_MAX];
    uint32_t tot = UDP_HDR_LEN + len;
    if (tot > sizeof seg)
        return -1;
    lock_acquire(&net_lock);
    uint32_t laddr = pcb->local_ip ? pcb->local_ip : ifp->ip;
    net_put16(seg + 0, pcb->local_port);
    net_put16(seg + 2, dport);
    net_put16(seg + 4, (uint16_t)tot);
    net_put16(seg + 6, 0);
    if (len)
        memcpy(seg + UDP_HDR_LEN, data, len);
    net_put16(seg + 6, udp_sum(seg, tot, laddr, daddr));
    int rc = ip_output(ifp, daddr, IPPROTO_UDP, seg, tot);
    lock_release(&net_lock);
    return rc;
}

static int udp_rx_room(struct UDP_PCB *pcb) {
    return UDP_RCV_BUF - (int)(pcb->rx_tail - pcb->rx_head);
}

int udp_rx_ready(struct UDP_PCB *pcb) {
    lock_acquire(&net_lock);
    int ready = (uint16_t)(pcb->rx_tail - pcb->rx_head) != 0;
    lock_release(&net_lock);
    return ready;
}

int udp_recv(struct UDP_PCB *pcb, void *buf, uint32_t len, uint32_t *saddr,
             uint16_t *sport) {
    lock_acquire(&net_lock);
    if (!udp_rx_ready(pcb)) {
        lock_release(&net_lock);
        return -1;
    }
    uint32_t n = 0;
    n |= (uint32_t)pcb->rx[pcb->rx_head & UDP_MASK] << 8;
    pcb->rx_head = (uint16_t)(pcb->rx_head + 1) & UDP_MASK;
    n |= pcb->rx[pcb->rx_head & UDP_MASK];
    pcb->rx_head = (uint16_t)(pcb->rx_head + 1) & UDP_MASK;
    uint32_t dlen = n & 0xFFFFu;
    if (dlen > len)
        dlen = len;
    uint32_t sa = net_be32(&pcb->rx[pcb->rx_head & UDP_MASK]);
    pcb->rx_head = (uint16_t)(pcb->rx_head + 4) & UDP_MASK;
    uint16_t sp = net_be16(&pcb->rx[pcb->rx_head & UDP_MASK]);
    pcb->rx_head = (uint16_t)(pcb->rx_head + 2) & UDP_MASK;
    if (dlen) {
        if ((pcb->rx_head & UDP_MASK) + dlen <= UDP_RCV_BUF)
            memcpy((uint8_t *)buf, &pcb->rx[pcb->rx_head & UDP_MASK], dlen);
        else {
            uint32_t first = UDP_RCV_BUF - (pcb->rx_head & UDP_MASK);
            memcpy((uint8_t *)buf, &pcb->rx[pcb->rx_head & UDP_MASK], first);
            memcpy((uint8_t *)buf + first, pcb->rx, dlen - first);
        }
    }
    if (saddr)
        *saddr = sa;
    if (sport)
        *sport = sp;
    pcb->rx_head = (uint16_t)(pcb->rx_head + (n & 0xFFFFu)) & UDP_MASK;
    lock_release(&net_lock);
    return (int)dlen;
}

static void udp_rx_put(struct UDP_PCB *pcb, const uint8_t *data, uint32_t len,
                       uint32_t saddr, uint16_t sport) {
    uint32_t need = 2 + 4 + 2 + len;
    if (need > (uint32_t)udp_rx_room(pcb))
        return;
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(len >> 8);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(len & 0xFF);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(saddr >> 24);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(saddr >> 16);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(saddr >> 8);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)saddr;
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)(sport >> 8);
    pcb->rx[pcb->rx_tail++ & UDP_MASK] = (uint8_t)sport;
    for (uint32_t i = 0; i < len; i++)
        pcb->rx[pcb->rx_tail++ & UDP_MASK] = data[i];
}

void udp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len) {
    if (len < UDP_HDR_LEN)
        return;
    uint16_t sport = net_be16(pkt + 0);
    uint16_t dport = net_be16(pkt + 2);
    uint16_t udp_len = net_be16(pkt + 4);
    if (udp_len < UDP_HDR_LEN || udp_len > len)
        return;
    uint16_t csum_seg = net_be16(pkt + 6);
    if (csum_seg) {
        uint16_t sum = udp_sum(pkt, udp_len, src, ifp->ip);
        if (sum != 0)
            return;
    }
    uint32_t dlen = udp_len - UDP_HDR_LEN;
    const uint8_t *data = pkt + UDP_HDR_LEN;

    struct UDP_PCB *pcb = 0;
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_UDP_PCB; i++) {
        struct UDP_PCB *p = &s_upcb[i];
        if (!p->active || p->local_port != dport)
            continue;
        if (p->remote_port && (p->remote_port != sport || p->remote_ip != src))
            continue;
        if (p->local_ip && p->local_ip != ifp->ip)
            continue;
        pcb = p;
        break;
    }
    if (pcb)
        udp_rx_put(pcb, data, dlen, src, sport);
    lock_release(&net_lock);
}
