#ifndef NIITAN_NET_UDP_H
#define NIITAN_NET_UDP_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define IPPROTO_UDP 17
#define UDP_HDR_LEN 8
#define UDP_PAYLOAD_MAX 1400
#define UDP_RCV_BUF 16384
#define MAX_UDP_PCB 32

struct UDP_PCB {
    uint8_t active;
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint8_t rx[UDP_RCV_BUF];
    uint16_t rx_head;
    uint16_t rx_tail;
};

void udp_init(void);
void udp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len);

struct UDP_PCB *udp_pcb_alloc(void);
void udp_pcb_free(struct UDP_PCB *pcb);

int udp_bind(struct UDP_PCB *pcb, uint32_t ip, uint16_t port);
void udp_connect(struct UDP_PCB *pcb, uint32_t ip, uint16_t port);
int udp_sendto(NETIF *ifp, struct UDP_PCB *pcb, const void *data,
               uint32_t len, uint32_t daddr, uint16_t dport);
int udp_recv(struct UDP_PCB *pcb, void *buf, uint32_t len, uint32_t *saddr,
             uint16_t *sport);
int udp_rx_ready(struct UDP_PCB *pcb);

#endif
