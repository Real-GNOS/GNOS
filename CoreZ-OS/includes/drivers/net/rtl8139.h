#ifndef NIITAN_NET_RTL8139_H
#define NIITAN_NET_RTL8139_H

#include <stdint.h>
#include "drivers/net/netif.h"

#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE 0x8139

int rtl8139_init(NETIF *ifp);
int rtl8139_tx(NETIF *ifp, const void *frame, uint32_t len);
int rtl8139_rx(NETIF *ifp, void *buf, uint32_t maxlen);

#endif
