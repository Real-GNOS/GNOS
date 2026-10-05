#ifndef NIITAN_NET_ARP_H
#define NIITAN_NET_ARP_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP 0x0800
#define ARP_HLEN 6
#define ARP_PLEN 4
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ARP_CACHE_SIZE 16

struct ARP_ENTRY {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t tmo;
    int valid;
};

void arp_init(void);
void arp_input(NETIF *ifp, const uint8_t *pkt, uint32_t len);
int arp_resolve(NETIF *ifp, uint32_t ip, uint8_t *out_mac);
void arp_tick(NETIF *ifp);

#endif
