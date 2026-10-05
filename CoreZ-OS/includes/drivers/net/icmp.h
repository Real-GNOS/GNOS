#ifndef NIITAN_NET_ICMP_H
#define NIITAN_NET_ICMP_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8
#define ICMP_HDR_LEN 8
#define ICMP_PAYLOAD 56
#define PING_QUEUE_MAX 32

void icmp_input(NETIF *ifp, uint32_t src, const uint8_t *pkt, uint32_t len);

#endif
