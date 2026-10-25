/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ipv6.c — IPv6 + ICMPv6 + NDP for GNOS. (GPLv2)
 *
 * fused into one module: address helpers, RFC 2460 header parse, pseudo-
 * header checksum, output through NDP or the multicast MAC map, input with
 * local-destination checks, the ICMPv6 echo/error paths, and the neighbor
 * cache that backstops unicast output.  GNOS adaptations: netif_t, kmalloc
 * frames, timer_ticks, the vfs.h errno names.
 */
#include <stdint.h>

#include "heap.h"
#include "kstring.h"
#include "net.h"
#include "ipv6.h"
#include "timer.h"
#include "vfs.h" /* errno names */

#include "drm_vsnprintf.h" /* plogk */

#define NDP_CACHE     8
#define NDP_QUEUE     4 /* packets parked per cache entry */
#define NDP_PROBES    3
#define NDP_PROBE_MS  1000
#define NDP_ENTRY_MS  60000

/* ---- the checksum ---------------------------------------------------------- */

static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t acc)
{
    while (len > 1) {
        acc += net_get16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        acc += (uint32_t)p[0] << 8;
    return acc;
}

static uint16_t fold(uint32_t acc)
{
    while (acc >> 16)
        acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)~acc;
}

uint16_t net_checksum_ipv6_pseudo(const ipv6_address_t *source, const ipv6_address_t *destination,
                                  uint8_t protocol, const void *data, uint32_t length)
{
    if (!source || !destination || (!data && length))
        return 0;
    uint8_t pseudo[40];
    memcpy(pseudo, source->bytes, IPV6_ADDRESS_LEN);
    memcpy(pseudo + 16, destination->bytes, IPV6_ADDRESS_LEN);
    net_put32(pseudo + 32, length);
    pseudo[36] = pseudo[37] = pseudo[38] = 0;
    pseudo[39]                           = protocol;
    return fold(sum16(data, length, sum16(pseudo, sizeof(pseudo), 0)));
}

/* ---- address helpers --------------------------------------------------------- */

int ipv6_address_equal(const ipv6_address_t *a, const ipv6_address_t *b)
{
    return a && b && !memcmp(a->bytes, b->bytes, IPV6_ADDRESS_LEN);
}

int ipv6_address_is_unspecified(const ipv6_address_t *a)
{
    static const ipv6_address_t zero;
    return a && ipv6_address_equal(a, &zero);
}

int ipv6_address_is_loopback(const ipv6_address_t *a)
{
    static const uint8_t lo[IPV6_ADDRESS_LEN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return a && !memcmp(a->bytes, lo, sizeof(lo));
}

int ipv6_address_is_multicast(const ipv6_address_t *a)
{
    return a && a->bytes[0] == 0xff;
}

int ipv6_address_is_link_local(const ipv6_address_t *a)
{
    return a && a->bytes[0] == 0xfe && (a->bytes[1] & 0xc0U) == 0x80U;
}

int ipv6_address_is_unicast(const ipv6_address_t *a)
{
    return a && !ipv6_address_is_unspecified(a) && !ipv6_address_is_multicast(a);
}

/* EUI-64 link-local (fe80::/64, U/L bit flipped) from a MAC. */
void ipv6_link_local_from_mac(ipv6_address_t *address, const uint8_t mac[6])
{
    if (!address || !mac)
        return;
    memset(address, 0, sizeof(*address));
    address->bytes[0]  = 0xfe;
    address->bytes[1]  = 0x80;
    address->bytes[8]  = mac[0] ^ 0x02U;
    address->bytes[9]  = mac[1];
    address->bytes[10] = mac[2];
    address->bytes[11] = 0xff;
    address->bytes[12] = 0xfe;
    address->bytes[13] = mac[3];
    address->bytes[14] = mac[4];
    address->bytes[15] = mac[5];
}

/* ff02::1:ffxx:xxxx solicited-node multicast for an address. */
void ipv6_solicited_node(const ipv6_address_t *address, ipv6_address_t *multicast)
{
    if (!address || !multicast)
        return;
    memset(multicast, 0, sizeof(*multicast));
    multicast->bytes[0]  = 0xff;
    multicast->bytes[1]  = 0x02;
    multicast->bytes[11] = 0x01;
    multicast->bytes[12] = 0xff;
    memcpy(multicast->bytes + 13, address->bytes + 13, 3);
}

/* 33:33:xx:xx:xx:xx Ethernet MAC for an IPv6 multicast address. */
void ipv6_multicast_ethernet(const ipv6_address_t *address, uint8_t mac[6])
{
    if (!address || !mac)
        return;
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, address->bytes + 12, 4);
}

/* ---- neighbor cache (the IPv6 ARP) -------------------------------------------- */

typedef struct {
    int            used;
    ipv6_address_t addr;
    uint8_t        mac[6];
    uint64_t       seen_at;
    /* Packets parked while the neighbor resolves. */
    uint8_t *pend[NDP_QUEUE];
    uint16_t pend_len[NDP_QUEUE];
    uint8_t  pend_proto[NDP_QUEUE];
    int      pend_n;
    uint32_t probes;
    uint64_t probe_at;
} ndp_entry_t;

static ndp_entry_t g_ndp[NDP_CACHE];

static ndp_entry_t *ndp_find(const ipv6_address_t *addr)
{
    for (int i = 0; i < NDP_CACHE; i++)
        if (g_ndp[i].used && ipv6_address_equal(&g_ndp[i].addr, addr))
            return &g_ndp[i];
    return 0;
}

static ndp_entry_t *ndp_slot(const ipv6_address_t *addr)
{
    ndp_entry_t *e = ndp_find(addr), *oldest = &g_ndp[0];
    if (e)
        return e;
    for (int i = 0; i < NDP_CACHE; i++)
        if (!g_ndp[i].used)
            return &g_ndp[i];
    for (int i = 1; i < NDP_CACHE; i++)
        if (g_ndp[i].seen_at < oldest->seen_at)
            oldest = &g_ndp[i];
    for (int i = 0; i < oldest->pend_n; i++)
        kfree(oldest->pend[i]);
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

void ndp_learn(struct netif *ifp, const ipv6_address_t *address, const uint8_t mac[6])
{
    (void)ifp;
    ndp_entry_t *e = ndp_slot(address);
    if (!e || !address || !mac)
        return;
    e->addr    = *address;
    memcpy(e->mac, mac, 6);
    e->seen_at = timer_ticks();
    e->used    = 1;
    e->probes  = 0;
    /* Flushing is the whole point of learning: parked packets go out now. */
    for (int i = 0; i < e->pend_n; i++) {
        net_eth_output6(e->mac, e->pend[i], e->pend_len[i]);
        kfree(e->pend[i]);
    }
    e->pend_n = 0;
}

/* Send one ICMPv6 packet of the given type/code plus its payload. */
static void icmpv6_send(struct netif *ifp, const ipv6_address_t *dst, uint8_t type, uint8_t code,
                        const void *payload, uint16_t plen)
{
    uint16_t  len = (uint16_t)(8 + plen);
    uint8_t  *msg = kmalloc(len);
    if (!msg)
        return;
    memset(msg, 0, 8);
    msg[0] = type;
    msg[1] = code;
    memcpy(msg + 8, payload, plen);
    ipv6_address_t src;
    memcpy(src.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
    uint16_t csum = net_checksum_ipv6_pseudo(&src, dst, IPV6_NEXT_ICMP, msg, len);
    net_put16(msg + 2, csum ? csum : 0xFFFFu);
    ipv6_output(ifp, &src, dst, IPV6_NEXT_ICMP, msg, len);
    kfree(msg);
}

/* Build and send a neighbor solicitation for `target`. */
static void ndp_solicit(struct netif *ifp, const ipv6_address_t *target)
{
    uint8_t        opt[8] = {2, 1, 0, 0, 0, 0, 0, 0};
    uint8_t        msg[24];
    ipv6_address_t src, dst;
    memcpy(src.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
    ipv6_solicited_node(target, &dst);
    memset(msg, 0, sizeof(msg));
    memcpy(msg + 8, target->bytes, IPV6_ADDRESS_LEN);
    memcpy(msg + 16, opt, 8); /* source link-layer address option */
    uint16_t csum = net_checksum_ipv6_pseudo(&src, &dst, IPV6_NEXT_ICMP, msg, sizeof(msg));
    net_put16(msg + 2, csum ? csum : 0xFFFFu);
    ipv6_output(ifp, &src, &dst, IPV6_NEXT_ICMP, msg, sizeof(msg));
}

/* Process one inbound ICMPv6 message (echo, NDP, errors). */
static void icmpv6_input(struct netif *ifp, const ipv6_info_t *info, const uint8_t *msg, uint16_t len)
{
    if (len < 8)
        return;
    uint8_t type = msg[0];
    if (type == ICMPV6_ECHO_REQUEST) {
        if (info->payload_length < 8)
            return;
        uint8_t *reply = kmalloc(info->payload_length);
        if (!reply)
            return;
        memcpy(reply, msg, info->payload_length);
        reply[0] = ICMPV6_ECHO_REPLY;
        reply[2] = reply[3] = 0;
        ipv6_address_t src;
        memcpy(src.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
        uint16_t csum = net_checksum_ipv6_pseudo(&src, &info->source, IPV6_NEXT_ICMP, reply, info->payload_length);
        net_put16(reply + 2, csum ? csum : 0xFFFFu);
        ipv6_output(ifp, &src, &info->source, IPV6_NEXT_ICMP, reply, (uint16_t)info->payload_length);
        kfree(reply);
        return;
    }
    if (type == ICMPV6_NEIGHBOR_SOLICIT || type == ICMPV6_NEIGHBOR_ADVERT) {
        /* NS/NA carry target address (+ option list) after the 8-byte header. */
        if (len < 24)
            return;
        ipv6_address_t target;
        memcpy(target.bytes, msg + 8, IPV6_ADDRESS_LEN);
        if (type == ICMPV6_NEIGHBOR_SOLICIT) {
            /* Reply with our link-local MAC in a neighbor advertisement. */
            uint8_t        adv[32];
            ipv6_address_t dst;
            memset(adv, 0, sizeof(adv));
            adv[0] = ICMPV6_NEIGHBOR_ADVERT;
            adv[4] = 0x20; /* solicited flag */
            memcpy(adv + 8, target.bytes, IPV6_ADDRESS_LEN);
            memcpy(adv + 24, ifp->mac, 6);
            ipv6_address_t src;
            memcpy(src.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
            if (ipv6_address_is_multicast(&info->source))
                ipv6_solicited_node(&target, &dst);
            else
                dst = info->source;
            uint16_t csum = net_checksum_ipv6_pseudo(&src, &dst, IPV6_NEXT_ICMP, adv, sizeof(adv));
            net_put16(adv + 2, csum ? csum : 0xFFFFu);
            ipv6_output(ifp, &src, &dst, IPV6_NEXT_ICMP, adv, sizeof(adv));
            ndp_learn(ifp, &info->source, ifp->mac);
        } else {
            ndp_learn(ifp, &target, msg + 24); /* target link-layer option */
        }
    }
    /* Error types (type < 128) quote the offending datagram; the socket
     * layer gets them once IPv6 transports exist. */
}

/* Parse the IPv6 header (extension headers: hop-by-hop parsed, fragment
 * rejected with a parameter problem, everything else refused). */
int net_ipv6_parse(const void *data, uint32_t length, net_ipv6_packet_t *packet)
{
    if (!data || !packet || length < IPV6_HEADER_LEN)
        return -E_INVAL;
    const uint8_t *bytes = data;
    if ((bytes[0] >> 4) != 6)
        return -E_INVAL;
    uint16_t payload_length = net_get16(bytes + 4);
    if ((uint32_t)payload_length > length - IPV6_HEADER_LEN)
        return -E_INVAL;
    memset(packet, 0, sizeof(*packet));
    packet->flow_label = ((uint32_t)(bytes[1] & 0x0fU) << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
    packet->total_len  = (uint16_t)(IPV6_HEADER_LEN + payload_length);
    packet->hop_limit  = bytes[7];
    memcpy(packet->source.bytes, bytes + 8, IPV6_ADDRESS_LEN);
    memcpy(packet->destination.bytes, bytes + 24, IPV6_ADDRESS_LEN);

    uint8_t  next   = bytes[6];
    uint32_t offset = IPV6_HEADER_LEN;
    if (next == IPV6_NEXT_FRAGMENT) {
        /* In-order only, like tcp.c: fragments are not held for reassembly. */
        return -E_PROTONOSUPPORT;
    }
    if (next == IPV6_NEXT_HOP_BY_HOP || next == IPV6_NEXT_ROUTING || next == IPV6_NEXT_DEST_OPTS)
        return -E_PROTONOSUPPORT;
    packet->protocol         = next;
    packet->transport_offset = (uint16_t)offset;
    packet->payload_len      = (uint16_t)(packet->total_len - offset);
    packet->payload          = bytes + offset;
    return 0;
}

int ipv6_input(struct netif *ifp, ipv6_pbuf_t *packet)
{
    net_ipv6_packet_t parsed;
    if (!ifp || !packet)
        goto bad;
    if (net_ipv6_parse(packet->data, packet->length, &parsed))
        goto bad;
    /* Local-destination check: our link-local, our solicited-node group, or
     * the all-nodes multicast. */
    int local = !memcmp(parsed.destination.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
    if (!local) {
        ipv6_address_t mine, solicited;
        memcpy(mine.bytes, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
        ipv6_solicited_node(&mine, &solicited);
        local = ipv6_address_equal(&parsed.destination, &solicited);
    }
    if (!local) {
        static const uint8_t all_nodes[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        local = !memcmp(parsed.destination.bytes, all_nodes, 16);
    }
    if (!local) {
        kfree(packet->data);
        kfree(packet);
        return -E_NODEV;
    }

    if (parsed.protocol == IPV6_NEXT_ICMP) {
        ipv6_info_t info;
        memset(&info, 0, sizeof(info));
        info.source         = parsed.source;
        info.destination    = parsed.destination;
        info.payload_length = parsed.payload_len;
        info.protocol       = parsed.protocol;
        info.hop_limit      = parsed.hop_limit;
        icmpv6_input(ifp, &info, parsed.payload, parsed.payload_len);
    } else if (parsed.protocol == IPV6_NEXT_UDP || parsed.protocol == IPV6_NEXT_TCP) {
        /* No IPv6 transports yet: tell the peer, port unreachable. */
        uint8_t        quote[IPV6_HEADER_LEN + 8];
        ipv6_address_t dst;
        memcpy(quote, packet->data, parsed.transport_offset < IPV6_HEADER_LEN + 8 ? parsed.transport_offset : IPV6_HEADER_LEN + 8);
        dst = parsed.source;
        icmpv6_send(ifp, &dst, ICMPV6_DEST_UNREACHABLE, ICMPV6_PORT_UNREACHABLE, quote, (uint16_t)sizeof(quote));
    }
    kfree(packet->data);
    kfree(packet);
    return 0;
bad:
    if (packet) {
        kfree(packet->data);
        kfree(packet);
    }
    return -E_INVAL;
}

int ipv6_output(struct netif *ifp, const ipv6_address_t *source, const ipv6_address_t *destination,
                uint8_t protocol, const void *payload, uint16_t len)
{
    if (!ifp || !destination || !payload || ipv6_address_is_unspecified(destination) ||
        ipv6_address_is_loopback(destination))
        return -E_INVAL;
    /* The fixed 1500-byte MTU (NET_MTU) clears IPv6's 1280-byte floor. */

    uint16_t total = (uint16_t)(IPV6_HEADER_LEN + len);
    uint8_t *frame = kmalloc(total);
    if (!frame)
        return -E_NOMEM;
    memset(frame, 0, IPV6_HEADER_LEN);
    frame[0] = 0x60;
    net_put16(frame + 4, len);
    frame[6] = protocol;
    frame[7] = 64;
    if (source && !ipv6_address_is_unspecified(source))
        memcpy(frame + 8, source->bytes, IPV6_ADDRESS_LEN);
    else
        memcpy(frame + 8, ifp->ipv6_link_local, IPV6_ADDRESS_LEN);
    memcpy(frame + 24, destination->bytes, IPV6_ADDRESS_LEN);
    memcpy(frame + IPV6_HEADER_LEN, payload, len);

    int status;
    if (ipv6_address_is_multicast(destination)) {
        uint8_t mac[6];
        ipv6_multicast_ethernet(destination, mac);
        status = net_eth_output6(mac, frame, total);
    } else {
        ndp_entry_t *e = ndp_find(destination);
        if (e) {
            status = net_eth_output6(e->mac, frame, total);
        } else if (net_ip6_queuedown(destination, frame, total)) {
            /* First contact: park the frame and solicit. */
            status = 0;
        } else {
            status = -E_NOBUFS;
        }
    }
    kfree(frame);
    return status;
}

/* Park an outgoing packet on its (absent) neighbor entry and solicit. */
int net_ip6_queuedown(const ipv6_address_t *destination, const void *frame, uint16_t len)
{
    ndp_entry_t *e = ndp_slot(destination);
    if (!e)
        return 0;
    if (e->pend_n < NDP_QUEUE && len <= NET_FRAME_MAX) {
        uint8_t *copy = kmalloc(len);
        if (copy) {
            memcpy(copy, frame, len);
            e->pend[e->pend_n]      = copy;
            e->pend_len[e->pend_n]  = len;
            e->pend_n++;
        }
    }
    e->addr    = *destination;
    e->seen_at = timer_ticks();
    e->used    = 1;
    uint64_t now = timer_ticks();
    if (!e->probe_at || now - e->probe_at >= NDP_PROBE_MS) {
        if (e->probes++ < NDP_PROBES) {
            struct netif *ifp = net_if(0);
            if (ifp)
                ndp_solicit(ifp, destination);
            e->probe_at = now;
        } else {
            /* Concede: drop the parked packets, caller sees -E_NOBUFS. */
            for (int i = 0; i < e->pend_n; i++)
                kfree(e->pend[i]);
            e->pend_n = 0;
            e->probes = 0;
            e->probe_at = 0;
            return 0;
        }
    }
    return 1;
}

void ipv6_if_up(struct netif *ifp)
{
    ipv6_address_t ll;
    ipv6_link_local_from_mac(&ll, ifp->mac);
    memcpy(ifp->ipv6_link_local, ll.bytes, IPV6_ADDRESS_LEN);
}

void ipv6_init(void)
{
    memset(g_ndp, 0, sizeof(g_ndp));
    plogk("ipv6: link-local IPv6 + NDP + ICMPv6 initialized.\n");
}
