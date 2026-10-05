#ifndef NIITAN_NET_TCP_H
#define NIITAN_NET_TCP_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define IPPROTO_TCP 6
#define TCP_HDR_LEN 20
#define TCP_MSS 1460
#define TCP_RCV_BUF 4096
#define TCP_SND_BUF 8192
#define TCP_BACKLOG 8
#define MAX_TCP_PCB 64
#define TCP_INIT_RTO 1000
#define TCP_MAX_RETRY 5

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

enum TCP_STATE {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

struct TCP_PCB {
    uint8_t active;
    uint8_t state;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t iss;
    uint32_t irs;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint16_t snd_wnd;
    uint32_t tmo;
    uint8_t retry;
    int accept_q[TCP_BACKLOG];
    uint16_t accept_head;
    uint16_t accept_tail;
    uint16_t accept_len;
    uint8_t rx[TCP_RCV_BUF];
    uint16_t rx_head;
    uint16_t rx_tail;
    uint8_t txb[TCP_SND_BUF];
    uint32_t tx_len;
    uint8_t fin_sent;
    uint8_t fin_rcvd;
};

void tcp_init(void);
void tcp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len);
void tcp_tick(NETIF *ifp);

struct TCP_PCB *tcp_pcb_alloc(void);
void tcp_pcb_free(struct TCP_PCB *pcb);

int tcp_bind(struct TCP_PCB *pcb, uint32_t ip, uint16_t port);
int tcp_listen(struct TCP_PCB *pcb);
int tcp_connect(struct TCP_PCB *pcb, uint32_t ip, uint16_t port);
int tcp_send(struct TCP_PCB *pcb, const void *data, uint32_t len);
int tcp_recv(struct TCP_PCB *pcb, void *buf, uint32_t len);
struct TCP_PCB *tcp_accept(struct TCP_PCB *listener);
int tcp_accept_ready(struct TCP_PCB *listener);
int tcp_close(struct TCP_PCB *pcb);
int tcp_shutdown(struct TCP_PCB *pcb, int how);

int tcp_state(struct TCP_PCB *pcb);

#endif
