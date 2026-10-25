/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ipv6.h — IPv6 + ICMPv6 + NDP for GNOS. (GPLv2)
 *
 * into one module and fitted to the GNOS socket layer: netif_t instead of
 * net_device_t, plain kmalloc'd frames instead of net_pbuf.
 *
 * Scope, aligned with the rest of this stack ("small, honest"):
 *   - link-local scope: the interface carries an EUI-64 fe80::/64 address
 *     derived from the MAC; destinations are reached directly or via
 *     multicast (no router discovery, no global prefixes yet).
 *   - neighbor discovery: solicitation, advertisement and a small cache
 *     back the unicast output path, mirroring net.c's ARP.
 *   - ICMPv6 echo and error generation; IPv6 fragments are rejected with
 *     ICMPv6 parameter problems (in-order only, like tcp.c).
 */
#ifndef GNUCOS_IPV6_H
#define GNUCOS_IPV6_H

#include <stdint.h>

#define IPV6_ADDRESS_LEN 16U
#define IPV6_HEADER_LEN  40U
#define IPV6_MIN_MTU     1280U

#define IPV6_NEXT_HOP_BY_HOP 0U
#define IPV6_NEXT_TCP        6U
#define IPV6_NEXT_UDP        17U
#define IPV6_NEXT_ROUTING    43U
#define IPV6_NEXT_FRAGMENT   44U
#define IPV6_NEXT_ICMP       58U
#define IPV6_NEXT_NONE       59U
#define IPV6_NEXT_DEST_OPTS  60U

#define ICMPV6_ECHO_REQUEST      128U
#define ICMPV6_ECHO_REPLY        129U
#define ICMPV6_ROUTER_SOLICIT    133U
#define ICMPV6_ROUTER_ADVERT     134U
#define ICMPV6_NEIGHBOR_SOLICIT  135U
#define ICMPV6_NEIGHBOR_ADVERT   136U
#define ICMPV6_DEST_UNREACHABLE  1U
#define ICMPV6_PACKET_TOO_BIG    2U
#define ICMPV6_TIME_EXCEEDED     3U
#define ICMPV6_PARAMETER_PROBLEM 4U
#define ICMPV6_PORT_UNREACHABLE  4U

#define ETH_TYPE_IPV6 0x86DD

typedef struct ipv6_address {
    uint8_t bytes[IPV6_ADDRESS_LEN];
} ipv6_address_t;

/* Decoded header info handed to the transport input. */
typedef struct {
    ipv6_address_t source;
    ipv6_address_t destination;
    uint16_t       payload_length;
    uint8_t        protocol;
    uint8_t        hop_limit;
} ipv6_info_t;

/* Parsed packet: header fields plus a pointer into the caller's buffer. */
typedef struct {
    ipv6_address_t source;
    ipv6_address_t destination;
    uint32_t       flow_label;
    uint16_t       total_len;
    uint16_t       payload_len;
    uint16_t       transport_offset;
    uint8_t        protocol;
    uint8_t        hop_limit;
    const uint8_t *payload;
} net_ipv6_packet_t;

/* A kmalloc'd frame the IPv6 layer owns and frees. */
typedef struct {
    uint8_t *data;
    uint32_t length;
} ipv6_pbuf_t;

struct netif;

void     ipv6_init(void);
/* Assign the EUI-64 link-local address for an interface (net_init calls this). */
void     ipv6_if_up(struct netif *ifp);

int      net_ipv6_parse(const void *data, uint32_t length, net_ipv6_packet_t *packet);
uint16_t net_checksum_ipv6_pseudo(const ipv6_address_t *source, const ipv6_address_t *destination,
                                  uint8_t protocol, const void *data, uint32_t length);

int  ipv6_address_equal(const ipv6_address_t *a, const ipv6_address_t *b);
int  ipv6_address_is_unspecified(const ipv6_address_t *a);
int  ipv6_address_is_loopback(const ipv6_address_t *a);
int  ipv6_address_is_multicast(const ipv6_address_t *a);
int  ipv6_address_is_link_local(const ipv6_address_t *a);
int  ipv6_address_is_unicast(const ipv6_address_t *a);
void ipv6_link_local_from_mac(ipv6_address_t *address, const uint8_t mac[6]);
void ipv6_solicited_node(const ipv6_address_t *address, ipv6_address_t *multicast);
void ipv6_multicast_ethernet(const ipv6_address_t *address, uint8_t mac[6]);

/* Input takes ownership of the frame (always frees it). */
int ipv6_input(struct netif *ifp, ipv6_pbuf_t *packet);

/* Output: builds the header around `payload` and transmits.  Frees nothing. */
int ipv6_output(struct netif *ifp, const ipv6_address_t *source, const ipv6_address_t *destination,
                uint8_t protocol, const void *payload, uint16_t len);

/* NDP: resolve a neighbor (queue + solicit) or learn from a wire reply. */
int  ndp_resolve(struct netif *ifp, const ipv6_address_t *address, const void *payload, uint16_t len,
                 uint8_t protocol);
void ndp_learn(struct netif *ifp, const ipv6_address_t *address, const uint8_t mac[6]);

#endif /* GNUCOS_IPV6_H */
