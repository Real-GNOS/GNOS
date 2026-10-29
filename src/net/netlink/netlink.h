/* SPDX-License-Identifier: GPL-2.0 */
/*
 * netlink.h — AF_NETLINK socket family. (GPLv2)
 *
 * Ported from the Uinxed net stack (net/netlink/netlink.h), sized to what
 * GNOS userland drives: the KOBJECT_UEVENT family (udev hotplug), generic
 * datagram unicast/multicast between user sockets, and the rtnetlink
 * GETLINK/GETADDR/GETROUTE dumps.  Wire formats are the Linux ones.
 *
 * Sockets live in their own table; the syscall layer encodes a netlink
 * socket index u as fd_sock value s = -1000 - u (AF_UNIX uses -2 - u with
 * u < 16, so the bands never touch).
 */
#ifndef GNUCOS_NETLINK_H
#define GNUCOS_NETLINK_H

#include <stdint.h>

#define NL_MAX_SOCKS 16

/* Protocol families (Linux numbers). */
#define NETLINK_ROUTE          0
#define NETLINK_KOBJECT_UEVENT 15

#define NL_GRP_NONE   0
#define NL_GRP_KERNEL 1 /* uevent: group 1 is what libudev subscribes to */

/* Message flags. */
#define NLM_F_REQUEST 0x01
#define NLM_F_MULTI   0x02
#define NLM_F_ACK     0x04
#define NLM_F_DUMP    0x03 /* REQUEST | ROOT */

/* Message types. */
#define NLMSG_NOOP  0x01
#define NLMSG_ERROR 0x02
#define NLMSG_DONE  0x03

#define NLMSG_HDRLEN            16
#define NLMSG_ALIGN(len)        (((len) + 3) & ~3U)
#define NLMSG_LENGTH(len)       ((len) + NLMSG_HDRLEN)
#define NLMSG_PAYLOAD(nh, base) ((int)((nh)->nlmsg_len) - NLMSG_ALIGN(base))

typedef struct {
    uint16_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
} nlmsghdr_t;

typedef struct {
    int        error;
    nlmsghdr_t msg;
} nlmsgerr_t;

#define NLMSG_OK(nh, len)                                                    \
    ((len) >= sizeof(nlmsghdr_t) && (nh)->nlmsg_len >= sizeof(nlmsghdr_t) && \
     (nh)->nlmsg_len <= (len))

typedef struct {
    uint16_t rta_len;
    uint16_t rta_type;
} rtattr_t;

#define RTA_ALIGN(len)  (((len) + 3) & ~3U)
#define RTA_LENGTH(len) (sizeof(rtattr_t) + (len))
#define RTA_DATA(rta)   ((void *)((char *)(rta) + RTA_ALIGN(sizeof(rtattr_t))))

/* rtnetlink message types. */
#define RTM_NEWLINK  16
#define RTM_GETLINK  18
#define RTM_NEWADDR  20
#define RTM_GETADDR  22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26

#define RTA_DST     1
#define RTA_SRC     2
#define RTA_OIF     4
#define RTA_GATEWAY 5
#define RTA_PREFSRC 7

/* Link attributes. */
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_ADDRESS   1
#define IFLA_OPERSTATE 16

/* Address attributes. */
#define IFA_ADDRESS   1
#define IFA_LOCAL     2
#define IFA_LABEL     3
#define IFA_BROADCAST 4

#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

#ifndef IFF_UP /* net.h's ioctl layer carries its own copy */
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#endif

/* Socket options. */
#define NETLINK_ADD_MEMBERSHIP  1
#define NETLINK_DROP_MEMBERSHIP 2
#define NETLINK_PKTINFO         3
#define NETLINK_BROADCAST_ERROR 4
#define NETLINK_NO_ENOBUFS      5

typedef struct {
    uint16_t nl_family;
    uint16_t _pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
} sockaddr_nl_t;

typedef struct {
    uint8_t  ifi_family;
    uint8_t  _pad;
    uint16_t ifi_type;
    int32_t  ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
} ifinfomsg_t;

typedef struct {
    uint8_t  ifa_family;
    uint8_t  ifa_prefixlen;
    uint8_t  ifa_flags;
    uint8_t  ifa_scope;
    uint32_t ifa_index;
} ifaddrmsg_t;

typedef struct {
    uint8_t  rtm_family;
    uint8_t  rtm_dst_len;
    uint8_t  rtm_src_len;
    uint8_t  rtm_tos;
    uint8_t  rtm_table;
    uint8_t  rtm_protocol;
    uint8_t  rtm_scope;
    uint8_t  rtm_type;
    uint32_t rtm_flags;
} rtmsg_t;

/* ---- syscall-facing API (index u = -1000 - fd_sock value) ---------------- */

int  nl_create(int type, int protocol);
void nl_close(int u);
int  nl_bind(int u, uint32_t pid, uint32_t groups);
int  nl_getname(int u, uint32_t *pid, uint32_t *groups);

/* sendto(2): dest_pid 0 + groups != 0 multicasts; dest_pid != 0 unicasts;
 * uevent-protocol writes carry the raw action@/devpath ABI. */
int nl_sendmsg(int u, const void *buf, uint32_t len, uint32_t dest_pid, uint32_t dest_groups);
int nl_recvmsg(int u, void *buf, uint32_t len, int flags, uint32_t *out_pid, uint32_t *out_groups);
int nl_readable(int u);
int nl_writable(int u);
int nl_set_nonblock(int u, int on);
int nl_setsockopt(int u, int name, const void *val, uint32_t vlen);

/* Kernel uplink: queue a datagram for every subscriber of `group`. */
int  netlink_broadcast(uint32_t protocol, uint32_t group, const void *data, uint32_t len);
void netlink_init(void);

#endif /* GNUCOS_NETLINK_H */
