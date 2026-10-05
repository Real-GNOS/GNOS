#ifndef NIITAN_NET_ETH_H
#define NIITAN_NET_ETH_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define ETH_HDR_LEN 14
#define ETH_FRAME_MAX 1518

void eth_input(NETIF *ifp, const void *frame, uint32_t len);
int eth_output(NETIF *ifp, const uint8_t *dst, uint16_t ethertype,
               const void *payload, uint32_t len);

#endif
