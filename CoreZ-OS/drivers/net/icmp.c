#include "drivers/net/icmp.h"

#include "kernel/asm_func.h"
#include "kernel/nt_ping_reply.h"
#include "lib/str/str.h"
#include "drivers/net/ip.h"
#include "drivers/net/net.h"

extern NETIF g_netif;

extern uint32_t net_now_ms(void);

static struct NET_PING_REPLY s_ping_q[PING_QUEUE_MAX];
static uint32_t s_ping_head;
static uint32_t s_ping_tail;
static uint32_t s_ping_cnt;
static uint32_t s_ping_tmo;
static int s_ping_active;

static void ping_push(uint32_t src, uint16_t id, uint16_t seq) {
    if (s_ping_cnt >= PING_QUEUE_MAX)
        return;
    struct NET_PING_REPLY *r = &s_ping_q[s_ping_tail];
    r->src = src;
    r->id = id;
    r->seq = seq;
    r->rtt_ms = net_now_ms() - s_ping_tmo;
    s_ping_tail = (s_ping_tail + 1) % PING_QUEUE_MAX;
    s_ping_cnt++;
    s_ping_active = 0;
}

void icmp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len) {
    if (len < ICMP_HDR_LEN)
        return;
    uint8_t type = pkt[0];
    uint8_t code = pkt[1];
    if (ip_csum(pkt, len) != 0)
        return;

    if (type == ICMP_ECHO_REQUEST && code == 0) {
        uint8_t reply[64];
        uint32_t rlen = len < sizeof reply ? len : sizeof reply;
        memcpy(reply, pkt, rlen);
        reply[0] = ICMP_ECHO_REPLY;
        reply[1] = 0;
        net_put16(reply + 2, ip_csum(reply, rlen));
        ip_output(ifp, src, IPPROTO_ICMP, reply, rlen);
    } else if (type == ICMP_ECHO_REPLY && code == 0) {
        lock_acquire(&net_lock);
        if (s_ping_active)
            ping_push(src, net_be16(pkt + 4), net_be16(pkt + 6));
        lock_release(&net_lock);
    }
}

int nt_icmp_send(uint32_t dst, uint16_t id, uint16_t seq) {
    if (dst == 0 || dst == g_netif.ip)
        return -1;
    uint8_t req[ICMP_HDR_LEN + ICMP_PAYLOAD];
    uint32_t rlen = sizeof req;
    req[0] = ICMP_ECHO_REQUEST;
    req[1] = 0;
    net_put16(req + 2, 0);
    net_put16(req + 4, id);
    net_put16(req + 6, seq);
    for (uint32_t i = ICMP_HDR_LEN; i < rlen; i++)
        req[i] = (uint8_t)(0x41 + (i & 0x1F));
    net_put16(req + 2, ip_csum(req, rlen));
    lock_acquire(&net_lock);
    s_ping_tmo = net_now_ms();
    s_ping_active = 1;
    lock_release(&net_lock);
    return ip_output(&g_netif, dst, IPPROTO_ICMP, req, rlen);
}

int nt_icmp_recv(struct NET_PING_REPLY *out, int max) {
    int n = 0;
    lock_acquire(&net_lock);
    while (n < max && s_ping_cnt > 0) {
        out[n] = s_ping_q[s_ping_head];
        s_ping_head = (s_ping_head + 1) % PING_QUEUE_MAX;
        s_ping_cnt--;
        n++;
    }
    lock_release(&net_lock);
    return n;
}
