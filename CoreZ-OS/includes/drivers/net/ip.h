#ifndef NIITAN_NET_IP_H
#define NIITAN_NET_IP_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define ETH_ARP 0x0806
#define ETH_IP 0x0800
#define IPPROTO_ICMP 1
#define IP_HDR_LEN 20

static inline uint16_t net_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t net_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void net_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void net_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint16_t ip_csum(const void *data, uint32_t len);
void ip_input(NETIF *ifp, const uint8_t *pkt, uint32_t len);
int ip_output(NETIF *ifp, uint32_t daddr, uint8_t proto, const void *data,
              uint32_t len);
void ip_arp_resolved(NETIF *ifp, uint32_t ip, const uint8_t *mac);

#endif
