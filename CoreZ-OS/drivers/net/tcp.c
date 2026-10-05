#include "drivers/net/tcp.h"
#include "drivers/char/console/io.h"

#include "kernel/asm_func.h"
#include "lib/str/str.h"
#include "drivers/net/ip.h"
#include "drivers/net/net.h"
#include "drivers/net/socket.h"

#define RCV_MASK (TCP_RCV_BUF - 1)

static struct TCP_PCB s_pcb[MAX_TCP_PCB];

static uint8_t s_rand_seed;

void tcp_init(void) {
    memset(s_pcb, 0, sizeof s_pcb);
    s_rand_seed = 0x5A;
}

static uint32_t tcp_iss(void) {
    return net_now_ms() ^ ((uint32_t)s_pcb[0].iss << 3);
}

static inline int seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static uint16_t tcp_sum(const uint8_t *seg, uint32_t seglen, uint32_t saddr,
                        uint32_t daddr) {
    uint32_t sum = 0;
    sum += (saddr >> 16) & 0xffff;
    sum += saddr & 0xffff;
    sum += (daddr >> 16) & 0xffff;
    sum += daddr & 0xffff;
    sum += 0x0006;
    sum += seglen;
    const uint8_t *p = seg;
    uint32_t n = seglen;
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

static struct TCP_PCB *pcb_find(uint32_t daddr, uint16_t dport, uint32_t saddr,
                                uint16_t sport) {
    struct TCP_PCB *any = 0;
    for (int i = 0; i < MAX_TCP_PCB; i++) {
        struct TCP_PCB *pcb = &s_pcb[i];
        if (!pcb->active)
            continue;
        if (pcb->state == TCP_LISTEN) {
            if (pcb->local_port == dport)
                any = pcb;
            continue;
        }
        if (pcb->remote_port == sport && pcb->remote_ip == saddr &&
            pcb->local_port == dport &&
            (pcb->local_ip == 0 || pcb->local_ip == daddr))
            return pcb;
    }
    return any;
}

struct TCP_PCB *tcp_pcb_alloc(void) {
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_TCP_PCB; i++) {
        if (!s_pcb[i].active) {
            memset(&s_pcb[i], 0, sizeof s_pcb[i]);
            s_pcb[i].active = 1;
            lock_release(&net_lock);
            return &s_pcb[i];
        }
    }
    lock_release(&net_lock);
    return 0;
}

void tcp_pcb_free(struct TCP_PCB *pcb) {
    lock_acquire(&net_lock);
    pcb->active = 0;
    lock_release(&net_lock);
}

static int tcp_emit(NETIF *ifp, struct TCP_PCB *pcb, uint32_t seq, uint32_t ack,
                    uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t seg[TCP_HDR_LEN + TCP_MSS];
    if (TCP_HDR_LEN + len > sizeof seg)
        return -1;
    memset(seg, 0, sizeof seg);
    net_put16(seg + 0, pcb->local_port);
    net_put16(seg + 2, pcb->remote_port);
    net_put32(seg + 4, seq);
    net_put32(seg + 8, ack);
    uint16_t wnd =
        (uint16_t)(TCP_RCV_BUF - (uint16_t)(pcb->rx_tail - pcb->rx_head));
    net_put16(seg + 12, (uint16_t)((TCP_HDR_LEN / 4) << 12) | flags);
    net_put16(seg + 14, wnd);
    net_put16(seg + 16, 0);
    net_put16(seg + 18, 0);
    if (len)
        memcpy(seg + TCP_HDR_LEN, data, len);
    net_put16(seg + 16,
              tcp_sum(seg, TCP_HDR_LEN + len, pcb->local_ip, pcb->remote_ip));
    return ip_output(ifp, pcb->remote_ip, IPPROTO_TCP, seg, TCP_HDR_LEN + len);
}

static int tcp_rx_room(struct TCP_PCB *pcb) {
    return TCP_RCV_BUF - (int)(pcb->rx_tail - pcb->rx_head);
}

static void tcp_rx_put(struct TCP_PCB *pcb, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        pcb->rx[pcb->rx_tail++ & RCV_MASK] = data[i];
}

static void tcp_fire(NETIF *ifp, struct TCP_PCB *pcb) {
    uint32_t limit = pcb->snd_una + pcb->tx_len;
    uint32_t window = pcb->snd_wnd ? pcb->snd_wnd : TCP_MSS;
    uint32_t cursor = pcb->snd_nxt;
    while (seq_lt(cursor, limit) &&
           (int32_t)(cursor - pcb->snd_una) < (int32_t)(window + 1)) {
        uint32_t avail = limit - cursor;
        uint32_t seg = avail;
        if (seg > TCP_MSS)
            seg = TCP_MSS;
        if (seg > window)
            seg = window;
        if (!seg)
            break;
        const uint8_t *d = pcb->txb + (cursor - pcb->snd_una);
        tcp_emit(ifp, pcb, cursor, pcb->rcv_nxt, TCP_FLAG_ACK, d, seg);
        cursor += seg;
    }
    if (cursor > pcb->snd_nxt) {
        pcb->snd_nxt = cursor;
        pcb->tmo = net_now_ms() + TCP_INIT_RTO;
        if (pcb->retry < TCP_MAX_RETRY)
            pcb->retry++;
    }
}

int tcp_bind(struct TCP_PCB *pcb, uint32_t ip, uint16_t port) {
    (void)ip;
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_TCP_PCB; i++) {
        struct TCP_PCB *other = &s_pcb[i];
        if (other->active && other != pcb && other->local_port == port) {
            lock_release(&net_lock);
            return -1;
        }
    }
    pcb->local_port = port;
    pcb->local_ip = 0;
    lock_release(&net_lock);
    return 0;
}

int tcp_listen(struct TCP_PCB *pcb) {
    lock_acquire(&net_lock);
    if (!pcb->local_port) {
        lock_release(&net_lock);
        return -1;
    }
    pcb->state = TCP_LISTEN;
    pcb->accept_head = pcb->accept_tail = pcb->accept_len = 0;
    lock_release(&net_lock);
    return 0;
}

int tcp_connect(struct TCP_PCB *pcb, uint32_t ip, uint16_t port) {
    extern NETIF g_netif;
    lock_acquire(&net_lock);
    pcb->local_ip = g_netif.ip;
    if (!pcb->local_port) {
        uint32_t p = (uint16_t)((net_now_ms() ^ (uint32_t)&pcb) & 0x7FFF) + 1024;
        pcb->local_port = (uint16_t)(p % 20000 + 10000);
    }
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    pcb->iss = tcp_iss();
    pcb->snd_una = pcb->iss + 1;
    pcb->snd_nxt = pcb->iss + 1;
    pcb->rcv_nxt = 0;
    pcb->state = TCP_SYN_SENT;
    pcb->tx_len = 0;
    pcb->tmo = net_now_ms() + TCP_INIT_RTO;
    pcb->retry = 0;
    int rc = tcp_emit(&g_netif, pcb, pcb->iss, 0, TCP_FLAG_SYN, 0, 0);
    lock_release(&net_lock);
    return rc;
}

int tcp_send(struct TCP_PCB *pcb, const void *data, uint32_t len) {
    extern NETIF g_netif;
    lock_acquire(&net_lock);
    if (pcb->state != TCP_ESTABLISHED || !len) {
        lock_release(&net_lock);
        return -1;
    }
    if (TCP_SND_BUF - pcb->tx_len < len) {
        lock_release(&net_lock);
        return -1;
    }
    memcpy(pcb->txb + pcb->tx_len, data, len);
    pcb->tx_len += len;
    tcp_fire(&g_netif, pcb);
    lock_release(&net_lock);
    return (int)len;
}

int tcp_recv(struct TCP_PCB *pcb, void *buf, uint32_t len) {
    lock_acquire(&net_lock);
    uint32_t avail = pcb->rx_tail - pcb->rx_head;
    if (avail == 0) {
        lock_release(&net_lock);
        return -1;
    }
    if (avail > len)
        avail = len;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < avail; i++)
        dst[i] = pcb->rx[pcb->rx_head++ & RCV_MASK];

    lock_release(&net_lock);
    return (int)avail;
}

int tcp_accept_ready(struct TCP_PCB *listener) {
    lock_acquire(&net_lock);
    if (listener->state != TCP_LISTEN || listener->accept_len == 0) {
        lock_release(&net_lock);
        return 0;
    }
    uint32_t cnt = listener->accept_len;
    for (uint32_t k = 0; k < cnt; k++) {
        int idx = listener->accept_q[(listener->accept_head + k) % TCP_BACKLOG];
        if (idx < 0 || idx >= MAX_TCP_PCB)
            continue;
        struct TCP_PCB *c = &s_pcb[idx];
        if (c->active && c->state == TCP_ESTABLISHED) {
            lock_release(&net_lock);
            return 1;
        }
    }
    lock_release(&net_lock);
    return 0;
}

struct TCP_PCB *tcp_accept(struct TCP_PCB *listener) {
    lock_acquire(&net_lock);
    if (listener->state != TCP_LISTEN || listener->accept_len == 0) {
        lock_release(&net_lock);
        return 0;
    }
    uint32_t cnt = listener->accept_len;
    for (uint32_t k = 0; k < cnt; k++) {
        int idx = listener->accept_q[(listener->accept_head + k) % TCP_BACKLOG];
        if (idx < 0 || idx >= MAX_TCP_PCB)
            continue;
        struct TCP_PCB *c = &s_pcb[idx];
        if (!c->active || c->state != TCP_ESTABLISHED)
            continue;
        listener->accept_len--;
        for (uint32_t m = k; m + 1 < cnt; m++) {
            listener->accept_q[(listener->accept_head + m) % TCP_BACKLOG] =
                listener
                    ->accept_q[(listener->accept_head + m + 1) % TCP_BACKLOG];
        }
        listener->accept_tail =
            (listener->accept_tail + TCP_BACKLOG - 1) % TCP_BACKLOG;
        lock_release(&net_lock);
        return c;
    }
    lock_release(&net_lock);
    return 0;
}

int tcp_close(struct TCP_PCB *pcb) {
    extern NETIF g_netif;
    lock_acquire(&net_lock);
    if (pcb->state == TCP_ESTABLISHED) {
        pcb->state = TCP_FIN_WAIT1;
        pcb->fin_sent = 1;
        tcp_emit(&g_netif, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                 TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        pcb->snd_nxt++;
        pcb->tmo = net_now_ms() + TCP_INIT_RTO;
        pcb->retry = 0;
        lock_release(&net_lock);
        return 0;
    }
    if (pcb->state == TCP_SYN_SENT || pcb->state == TCP_CLOSED) {
        pcb->state = TCP_CLOSED;
        pcb->active = 0;
        lock_release(&net_lock);
        return 0;
    }
    lock_release(&net_lock);
    return -1;
}

int tcp_shutdown(struct TCP_PCB *pcb, int how) {
    extern NETIF g_netif;
    lock_acquire(&net_lock);
    if (how & SHUT_WR) {
        if (pcb->state == TCP_ESTABLISHED) {
            pcb->fin_sent = 1;
            tcp_emit(&g_netif, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                     TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            pcb->snd_nxt++;
            pcb->tmo = net_now_ms() + TCP_INIT_RTO;
            pcb->retry = 0;
            pcb->state = TCP_FIN_WAIT1;
        } else if (pcb->state == TCP_CLOSE_WAIT) {
            pcb->fin_sent = 1;
            tcp_emit(&g_netif, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                     TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            pcb->snd_nxt++;
            pcb->tmo = net_now_ms() + TCP_INIT_RTO;
            pcb->retry = 0;
            pcb->state = TCP_LAST_ACK;
        }
    }
    if (how & SHUT_RD)
        pcb->fin_rcvd = 1;
    lock_release(&net_lock);
    return 0;
}

static void tcp_accept_child(NETIF *ifp, struct TCP_PCB *listener,
                             uint32_t saddr, uint16_t sport, uint32_t seq) {
    if (listener->accept_len >= TCP_BACKLOG)
        return;
    struct TCP_PCB *child = tcp_pcb_alloc();
    if (!child)
        return;
    child->state = TCP_SYN_RCVD;
    child->local_ip = ifp->ip;
    child->local_port = listener->local_port;
    child->remote_ip = saddr;
    child->remote_port = sport;
    child->iss = tcp_iss();
    child->irs = seq;
    child->snd_una = child->iss + 1;
    child->snd_nxt = child->iss + 1;
    child->rcv_nxt = seq + 1;
    child->rx_head = child->rx_tail = 0;
    child->tmo = net_now_ms() + TCP_INIT_RTO;
    child->retry = 0;
    tcp_emit(ifp, child, child->iss, child->rcv_nxt,
             TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
    listener->accept_q[listener->accept_tail++] = (int)(child - s_pcb);
    listener->accept_tail %= TCP_BACKLOG;
    listener->accept_len++;
}

static void tcp_input_established(NETIF *ifp, struct TCP_PCB *pcb, uint32_t seq,
                                  uint8_t flags, const uint8_t *data,
                                  uint32_t dlen, uint32_t ack) {
    if (flags & TCP_FLAG_RST) {
        pcb->state = TCP_CLOSED;
        pcb->active = 0;
        return;
    }
    if (flags & TCP_FLAG_SYN)
        return;
    if (flags & TCP_FLAG_ACK) {
        if (seq_lt(pcb->snd_nxt, ack)) {
            tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
            return;
        }
        if (seq_lt(pcb->snd_una, ack)) {
            uint32_t d = ack - pcb->snd_una;
            if (d > pcb->tx_len)
                d = pcb->tx_len;
            if (d) {
                pcb->tx_len -= d;
                memmove(pcb->txb, pcb->txb + d, pcb->tx_len);
                pcb->snd_una = ack;
                if (pcb->snd_una >= pcb->snd_nxt) {
                    pcb->retry = 0;
                    pcb->tmo = 0;
                }
            }
        }
    }
    if (flags & TCP_FLAG_FIN) {
        pcb->fin_rcvd = 1;
        if (pcb->state == TCP_ESTABLISHED) {
            pcb->state = TCP_CLOSE_WAIT;
        }
        pcb->rcv_nxt = seq + 1 + dlen;
        pcb->rx_head = pcb->rx_tail;
        tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
        return;
    }
    if (dlen) {
        uint32_t wnd_end = pcb->rcv_nxt + (uint32_t)tcp_rx_room(pcb);
        uint32_t seg_last = seq + dlen - 1u;
        int starts_in = !seq_lt(seq, pcb->rcv_nxt) && seq_lt(seq, wnd_end);
        int ends_in =
            !seq_lt(seg_last, pcb->rcv_nxt) && seq_lt(seg_last, wnd_end);
        if (!starts_in && !ends_in) {
            tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
            return;
        }
        if (seq_lt(seq, pcb->rcv_nxt)) {
            uint32_t past = pcb->rcv_nxt - seq;
            if (past >= dlen) {
                tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0,
                         0);
                return;
            }
            seq += past;
            data += past;
            dlen -= past;
        }
        uint32_t room = (uint32_t)tcp_rx_room(pcb);
        if (dlen > room)
            dlen = room;
        if (dlen) {
            tcp_rx_put(pcb, data, dlen);
            pcb->rcv_nxt += dlen;
        }
        tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
    }
}

void tcp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len) {
    if (len < TCP_HDR_LEN)
        return;
    uint16_t sport = net_be16(pkt + 0);
    uint16_t dport = net_be16(pkt + 2);
    uint32_t seq = net_be32(pkt + 4);
    uint32_t ack = net_be32(pkt + 8);
    uint16_t off_flags = net_be16(pkt + 12);
    uint8_t hlen = (uint8_t)((off_flags >> 12) * 4);
    uint8_t flags = (uint8_t)(off_flags & 0xFF);
    if (hlen < TCP_HDR_LEN || len < hlen)
        return;
    uint32_t dlen = len - hlen;
    const uint8_t *data = pkt + hlen;

    uint16_t sum = tcp_sum(pkt, len, src, ifp->ip);
    if (sum != 0)
        return;

    lock_acquire(&net_lock);
    struct TCP_PCB *pcb = pcb_find(ifp->ip, dport, src, sport);
    if (!pcb) {
        lock_release(&net_lock);
        return;
    }

    if (pcb->state == TCP_LISTEN) {
        if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
            tcp_accept_child(ifp, pcb, src, sport, seq);
        }
        lock_release(&net_lock);
        return;
    }

    if (pcb->state == TCP_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            pcb->irs = seq;
            pcb->rcv_nxt = seq + 1;
            pcb->snd_una = ack;
            pcb->snd_nxt = ack;
            pcb->state = TCP_ESTABLISHED;
            pcb->retry = 0;
            pcb->tmo = 0;
            tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
            tcp_input_established(ifp, pcb, seq + 1, flags, data, dlen, ack);
        }
        lock_release(&net_lock);
        return;
    }

    if (pcb->state == TCP_SYN_RCVD) {
        if (flags & TCP_FLAG_ACK) {
            if (ack == pcb->snd_nxt) {
                pcb->state = TCP_ESTABLISHED;
                pcb->retry = 0;
                pcb->tmo = 0;
                tcp_input_established(ifp, pcb, seq, flags, data, dlen, ack);
            }
        }
        lock_release(&net_lock);
        return;
    }

    tcp_input_established(ifp, pcb, seq, flags, data, dlen, ack);

    if (pcb->state == TCP_FIN_WAIT1 && (flags & TCP_FLAG_ACK)) {
        pcb->state = TCP_FIN_WAIT2;
        pcb->retry = 0;
    } else if (pcb->state == TCP_FIN_WAIT1 && (flags & TCP_FLAG_FIN)) {
        pcb->rcv_nxt = ack + 1;
        tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                 TCP_FLAG_ACK | TCP_FLAG_FIN, 0, 0);
        pcb->state = TCP_TIME_WAIT;
        pcb->tmo = net_now_ms() + 2000;
    } else if (pcb->state == TCP_FIN_WAIT2 && (flags & TCP_FLAG_FIN)) {
        pcb->rcv_nxt = ack + 1;
        tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt, TCP_FLAG_ACK, 0, 0);
        pcb->state = TCP_TIME_WAIT;
        pcb->tmo = net_now_ms() + 2000;
    } else if (pcb->state == TCP_CLOSE_WAIT) {
        pcb->state = TCP_LAST_ACK;
        tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                 TCP_FLAG_ACK | TCP_FLAG_FIN, 0, 0);
        pcb->snd_nxt++;
        pcb->tmo = net_now_ms() + TCP_INIT_RTO;
        pcb->retry = 0;
    } else if (pcb->state == TCP_LAST_ACK && (flags & TCP_FLAG_ACK)) {
        pcb->active = 0;
    }
    lock_release(&net_lock);
}

void tcp_tick(NETIF *ifp) {
    uint32_t now = net_now_ms();
    lock_acquire(&net_lock);
    for (int i = 0; i < MAX_TCP_PCB; i++) {
        struct TCP_PCB *pcb = &s_pcb[i];
        if (!pcb->active)
            continue;
        if (pcb->state == TCP_LISTEN) {
            int h = pcb->accept_head;
            while (h != pcb->accept_tail) {
                int idx = pcb->accept_q[h++];
                h %= TCP_BACKLOG;
                struct TCP_PCB *c = &s_pcb[idx];
                if (c->active && c->state == TCP_SYN_RCVD && c->retry &&
                    (int32_t)(now - c->tmo) >= 0) {
                    tcp_emit(ifp, c, c->iss, c->rcv_nxt,
                             TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
                    c->tmo = now + TCP_INIT_RTO * c->retry;
                    c->retry++;
                    if (c->retry > TCP_MAX_RETRY)
                        c->active = 0;
                }
            }
            continue;
        }
        if (!pcb->tmo || (int32_t)(now - pcb->tmo) < 0)
            continue;
        if (pcb->state == TCP_SYN_SENT) {
            if (pcb->retry >= TCP_MAX_RETRY) {
                pcb->active = 0;
                continue;
            }
            tcp_emit(ifp, pcb, pcb->iss, 0, TCP_FLAG_SYN, 0, 0);
            pcb->retry = 0;
            {
                uint32_t rto = TCP_INIT_RTO;
                for (int k = 0; k < pcb->retry; k++)
                    rto *= 2;
                pcb->tmo = now + rto;
            }
            pcb->retry++;
            continue;
        }
        if (seq_lt(pcb->snd_una, pcb->snd_nxt)) {
            pcb->retry++;
            if (pcb->retry > TCP_MAX_RETRY) {
                pcb->active = 0;
                continue;
            }
            uint32_t rto = TCP_INIT_RTO;
            rto <<= (pcb->retry);
            {
                uint32_t ack = pcb->rcv_nxt;
                uint32_t cursor = pcb->snd_una;
                uint32_t limit = pcb->snd_una + pcb->tx_len;
                while (seq_lt(cursor, limit)) {
                    uint32_t seg = limit - cursor;
                    if (seg > TCP_MSS)
                        seg = TCP_MSS;
                    tcp_emit(ifp, pcb, cursor, ack, TCP_FLAG_ACK,
                             pcb->txb + (cursor - pcb->snd_una), seg);
                    cursor += seg;
                }
                pcb->tmo = now + rto;
            }
        } else if (pcb->state == TCP_FIN_WAIT1 || pcb->state == TCP_LAST_ACK) {
            pcb->retry++;
            if (pcb->retry > TCP_MAX_RETRY) {
                pcb->active = 0;
                continue;
            }
            tcp_emit(ifp, pcb, pcb->snd_nxt, pcb->rcv_nxt,
                     TCP_FLAG_ACK | TCP_FLAG_FIN, 0, 0);
            pcb->tmo = now + TCP_INIT_RTO * pcb->retry;
        } else if (pcb->state == TCP_TIME_WAIT) {
            pcb->active = 0;
        }
    }
    lock_release(&net_lock);
}
