#ifndef NIITAN_NET_NETIF_H
#define NIITAN_NET_NETIF_H

#include <stdint.h>

typedef struct NETIF NETIF;

typedef int (*netif_init_fn)(NETIF *ifp);
typedef int (*netif_tx_fn)(NETIF *ifp, const void *frame, uint32_t len);
typedef int (*netif_rx_fn)(NETIF *ifp, void *buf, uint32_t maxlen);

struct NETIF {
    uint8_t mac[6];
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    void *priv;
    netif_init_fn init;
    netif_tx_fn tx;
    netif_rx_fn rx;
};

#endif
