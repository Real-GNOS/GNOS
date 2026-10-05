#include "drivers/net/eth.h"
#include "drivers/char/console/io.h"

#include "lib/str/str.h"
#include "drivers/net/arp.h"
#include "drivers/net/ip.h"

static void eth_parse(const uint8_t *f, uint8_t *dst, uint16_t *type) {
    for (int i = 0; i < 6; i++)
        dst[i] = f[i];
    *type = net_be16(f + 12);
}

void eth_input(NETIF *ifp, const void *frame, uint32_t len) {
    if (len < ETH_HDR_LEN)
        return;
    const uint8_t *f = (const uint8_t *)frame;
    uint8_t dst[6];
    uint16_t type;
    eth_parse(f, dst, &type);

    int to_us = 1;
    for (int i = 0; i < 6; i++) {
        if (dst[i] != ifp->mac[i])
            to_us = 0;
    }
    int to_bcast = 1;
    for (int i = 0; i < 6; i++) {
        if (dst[i] != 0xFF)
            to_bcast = 0;
    }
    if (!to_us && !to_bcast)
        return;

    uint32_t plen = len - ETH_HDR_LEN;
    const uint8_t *p = f + ETH_HDR_LEN;
    switch (type) {
    case ETH_ARP:
        arp_input(ifp, p, plen);
        break;
    case ETH_IP:
        ip_input(ifp, p, plen);
        break;
    }
}

int eth_output(NETIF *ifp, const uint8_t *dst, uint16_t ethertype,
               const void *payload, uint32_t len) {
    if (ifp == NULL || ifp->tx == NULL)
        return -1;
    uint8_t frame[ETH_FRAME_MAX];
    uint32_t fidx = ETH_HDR_LEN + len;
    if (fidx > sizeof frame)
        return -1;
    for (int i = 0; i < 6; i++)
        frame[i] = dst[i];
    for (int i = 0; i < 6; i++)
        frame[6 + i] = ifp->mac[i];
    net_put16(frame + 12, ethertype);
    memcpy(frame + ETH_HDR_LEN, payload, len);
    return ifp->tx(ifp, frame, fidx);
}
