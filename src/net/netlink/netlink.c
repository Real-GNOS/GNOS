/* SPDX-License-Identifier: GPL-2.0 */
/*
 * netlink.c — AF_NETLINK socket family. (GPLv2)
 *
 * fitted to the GNOS socket model: a plain table of datagram sockets with a receive
 * queue each, bind by port id + multicast groups, unicast by port id,
 * multicast by group mask, and the uevent wire-ABI validation for
 * NETLINK_KOBJECT_UEVENT writers.  The rtnetlink GETLINK/GETADDR/GETROUTE
 * dumps walk the two static interfaces from net.h.
 */
#include <stdint.h>

#include "heap.h"
#include "kstring.h"
#include "net.h"
#include "netlink.h"
#include "proc.h"
#include "sock.h" /* MSG_PEEK / MSG_DONTWAIT */
#include "vfs.h"

#include "drm_vsnprintf.h" /* plogk */

typedef struct nl_msg {
    struct nl_msg *next;
    uint32_t       len;
    uint32_t       sender_pid;
    uint32_t       sender_groups;
    uint8_t        data[];
} nl_msg_t;

typedef struct {
    uint8_t   used;
    uint8_t   nonblock;
    uint8_t   closed;
    uint8_t   no_enobufs;
    int       protocol;
    uint32_t  nl_pid; /* 0 = unbound */
    uint32_t  nl_groups;
    nl_msg_t *recvq; /* FIFO of queued datagrams */
    uint32_t  recvq_len;
    uint32_t  recvq_max;
    uint32_t  recvq_bytes;
} nl_sock_t;

static nl_sock_t g_nl[NL_MAX_SOCKS];

/* Multicast registry: which sockets listen on which protocol/groups. */
typedef struct {
    int      sock; /* g_nl index, -1 = free */
    uint32_t groups;
} nl_member_t;

#define NL_MCAST_MAX 16
static struct {
    nl_member_t m[NL_MCAST_MAX];
    uint32_t    count;
} g_nl_mcast[8]; /* indexed by protocol, only 0 and 15 used */

static nl_sock_t *nl_at(int u)
{
    if (u < 0 || u >= NL_MAX_SOCKS || !g_nl[u].used)
        return 0;
    return &g_nl[u];
}

/* Queue one datagram on a socket; wakes any blocked reader. */
static int nl_queue(nl_sock_t *ns, const void *data, uint32_t len, uint32_t pid, uint32_t groups)
{
    if (!ns || ns->closed)
        return -E_BADF;
    if (!data || !len)
        return -E_INVAL;
    if (ns->recvq_len >= ns->recvq_max || ns->recvq_bytes + len > 65536) {
        if (!ns->no_enobufs)
            return -E_NOBUFS;
        return -E_AGAIN; /* broadcast-error mode: drop silently */
    }
    nl_msg_t *m = kmalloc(sizeof(*m) + len);
    if (!m)
        return -E_NOMEM;
    memcpy(m->data, data, len);
    m->len           = len;
    m->sender_pid    = pid;
    m->sender_groups = groups;
    m->next          = NULL;

    if (!ns->recvq) {
        ns->recvq = m;
    } else {
        nl_msg_t *t = ns->recvq;
        while (t->next)
            t = t->next;
        t->next = m;
    }
    ns->recvq_len++;
    ns->recvq_bytes += len;
    return 0;
}

/* Deliver to every socket subscribed to `groups` on `protocol`. */
static int nl_broadcast(uint32_t protocol, uint32_t groups, const void *data, uint32_t len,
                        uint32_t pid)
{
    if (protocol >= 8)
        return -E_PROTONOSUPPORT;
    if (!data || !len || !groups)
        return -E_INVAL;
    int delivered = 0, first_err = 0;
    for (uint32_t i = 0; i < g_nl_mcast[protocol].count; i++) {
        nl_member_t *mb = &g_nl_mcast[protocol].m[i];
        if (mb->sock < 0 || !(mb->groups & groups))
            continue;
        int r = nl_queue(&g_nl[mb->sock], data, len, pid, groups);
        if (r == 0)
            delivered++;
        else if (!first_err && r != -E_NOBUFS)
            first_err = r;
    }
    if (delivered)
        return delivered;
    return first_err ? first_err : -E_SRCH; /* no listeners */
}

int netlink_broadcast(uint32_t protocol, uint32_t group, const void *data, uint32_t len)
{
    return nl_broadcast(protocol, group, data, len, 0);
}

/* ---- socket lifecycle ----------------------------------------------------- */

int nl_create(int type, int protocol)
{
    (void)type;
    if (protocol != NETLINK_ROUTE && protocol != NETLINK_KOBJECT_UEVENT)
        return -E_PROTONOSUPPORT;
    for (int u = 0; u < NL_MAX_SOCKS; u++) {
        nl_sock_t *ns = &g_nl[u];
        if (ns->used)
            continue;
        memset(ns, 0, sizeof(*ns));
        ns->used      = 1;
        ns->protocol  = protocol;
        ns->recvq_max = 32;
        return u;
    }
    return -E_NOSPC;
}

void nl_close(int u)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns)
        return;
    ns->closed = 1;
    /* leave the multicast registry */
    if (ns->protocol < 8) {
        for (uint32_t i = 0; i < g_nl_mcast[ns->protocol].count; i++) {
            if (g_nl_mcast[ns->protocol].m[i].sock == u) {
                g_nl_mcast[ns->protocol].m[i] =
                    g_nl_mcast[ns->protocol].m[g_nl_mcast[ns->protocol].count - 1];
                g_nl_mcast[ns->protocol].count--;
                break;
            }
        }
    }
    nl_msg_t *m = ns->recvq;
    while (m) {
        nl_msg_t *n = m->next;
        kfree(m);
        m = n;
    }
    memset(ns, 0, sizeof(*ns));
}

/* Bind: publish the port id and join the multicast groups. */
int nl_bind(int u, uint32_t pid, uint32_t groups)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns || ns->nl_pid)
        return -E_INVAL;
    if (ns->protocol >= 8)
        return -E_PROTONOSUPPORT;

    /* Port-id uniqueness across the protocol's registry. */
    if (pid) {
        for (uint32_t i = 0; i < g_nl_mcast[ns->protocol].count; i++) {
            int o = g_nl_mcast[ns->protocol].m[i].sock;
            if (o >= 0 && g_nl[o].nl_pid == pid)
                return -E_ADDRINUSE;
        }
    } else {
        pid = (uint32_t)(uintptr_t)proc_current(); /* auto: use the pid */
        if (!pid)
            pid = 0x80000000u;
    }

    /* Join: one registry entry per socket (rebind updates the mask). */
    int slot = -1;
    for (uint32_t i = 0; i < g_nl_mcast[ns->protocol].count; i++)
        if (g_nl_mcast[ns->protocol].m[i].sock == u)
            slot = (int)i;
    if (slot < 0) {
        if (g_nl_mcast[ns->protocol].count >= NL_MCAST_MAX)
            return -E_NOBUFS;
        slot                                  = (int)g_nl_mcast[ns->protocol].count++;
        g_nl_mcast[ns->protocol].m[slot].sock = u;
    }
    g_nl_mcast[ns->protocol].m[slot].groups = groups;
    ns->nl_pid                              = pid;
    ns->nl_groups                           = groups;
    return 0;
}

int nl_getname(int u, uint32_t *pid, uint32_t *groups)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns)
        return -E_BADF;
    *pid    = ns->nl_pid;
    *groups = ns->nl_groups;
    return 0;
}

/* Validate a raw uevent write: "action@/devpath\0KEY=VAL\0..." with a known
 * action; udev's synthetic events come through here from user space. */
static int nl_uevent_valid(const uint8_t *buf, uint32_t len)
{
    uint32_t nul = 0;
    while (nul < len && buf[nul])
        nul++;
    if (nul == len || nul < 3 || nul >= 128)
        return 0;
    const char *at = 0;
    for (uint32_t i = 0; i < nul; i++)
        if (buf[i] == '@') {
            at = (const char *)buf + i;
            break;
        }
    if (!at || at == (const char *)buf || at[1] != '/')
        return 0;
    static const char *const actions[] = {"add",    "remove",  "change", "move",
                                          "online", "offline", "bind",   "unbind"};
    size_t                   alen      = (size_t)(at - (const char *)buf);
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (strlen(actions[i]) == alen && memcmp(buf, actions[i], alen) == 0)
            return 1;
    return 0;
}

int nl_sendmsg(int u, const void *buf, uint32_t len, uint32_t dest_pid, uint32_t dest_groups)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns || !buf || !len)
        return -E_INVAL;
    if (!ns->nl_pid) {
        int r = nl_bind(u, 0, 0);
        if (r)
            return r;
    }

    if (ns->protocol == NETLINK_KOBJECT_UEVENT) {
        /* Raw kernel-ABI writes ("action@/devpath\0KEY=VAL\0...") must parse
         * as a uevent before we relay them; libudev worker records are
         * covered by the unicast path below. */
        if (!dest_pid && !nl_uevent_valid(buf, len))
            return -E_INVAL;
        /* Unicast by pid (libudev worker handoff) */
        if (dest_pid) {
            for (uint32_t i = 0; i < g_nl_mcast[ns->protocol].count; i++) {
                int o = g_nl_mcast[ns->protocol].m[i].sock;
                if (o >= 0 && g_nl[o].nl_pid == dest_pid)
                    return nl_queue(&g_nl[o], buf, len, ns->nl_pid, dest_groups) == 0
                               ? (int)len
                               : -E_CONNREFUSED;
            }
            return -E_CONNREFUSED;
        }
        uint32_t groups = dest_groups ? dest_groups : ns->nl_groups;
        if (!groups)
            groups = NL_GRP_KERNEL;
        int r = nl_broadcast(ns->protocol, groups, buf, len, ns->nl_pid);
        return (r >= 0 || r == -E_SRCH) ? (int)len : r;
    }

    if (dest_pid) {
        for (uint32_t i = 0; i < g_nl_mcast[ns->protocol].count; i++) {
            int o = g_nl_mcast[ns->protocol].m[i].sock;
            if (o >= 0 && g_nl[o].nl_pid == dest_pid)
                return nl_queue(&g_nl[o], buf, len, ns->nl_pid, dest_groups) == 0 ? (int)len
                                                                                  : -E_CONNREFUSED;
        }
        return -E_CONNREFUSED;
    }
    int r = nl_broadcast(ns->protocol, dest_groups, buf, len, ns->nl_pid);
    return (r >= 0 || r == -E_SRCH) ? (int)len : r;
}

int nl_recvmsg(int u, void *buf, uint32_t len, int flags, uint32_t *out_pid, uint32_t *out_groups)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns || (!buf && len))
        return -E_INVAL;
    if (out_pid)
        *out_pid = 0;
    if (out_groups)
        *out_groups = 0;

    for (;;) {
        if (ns->closed)
            return -E_BADF;
        nl_msg_t *m = ns->recvq;
        if (m) {
            uint32_t copy = m->len < len ? m->len : len;
            if (copy)
                memcpy(buf, m->data, copy);
            if (out_pid)
                *out_pid = m->sender_pid;
            if (out_groups)
                *out_groups = m->sender_groups;
            int ret = m->len > len ? (int)len : (int)m->len;
            if (!(flags & MSG_PEEK)) {
                ns->recvq = m->next;
                ns->recvq_len--;
                ns->recvq_bytes -= m->len;
                kfree(m);
            }
            return ret;
        }
        if (ns->nonblock || (flags & MSG_DONTWAIT))
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0); /* unix.c's block shape */
    }
}

int nl_readable(int u)
{
    nl_sock_t *ns = nl_at(u);
    return ns && ns->recvq;
}

int nl_writable(int u)
{
    return nl_at(u) != 0;
}

int nl_set_nonblock(int u, int on)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns)
        return -E_BADF;
    ns->nonblock = on != 0;
    return 0;
}

int nl_setsockopt(int u, int name, const void *val, uint32_t vlen)
{
    nl_sock_t *ns = nl_at(u);
    if (!ns)
        return -E_BADF;
    if (vlen < sizeof(int) || !val)
        return -E_INVAL;
    int ival;
    memcpy(&ival, val, sizeof(ival));
    if (name == NETLINK_ADD_MEMBERSHIP || name == NETLINK_DROP_MEMBERSHIP) {
        if (ival <= 0 || ival > 32)
            return -E_INVAL;
        uint32_t bit = 1u << (uint32_t)(ival - 1);
        uint32_t mask =
            name == NETLINK_ADD_MEMBERSHIP ? (ns->nl_groups | bit) : (ns->nl_groups & ~bit);
        if (ns->nl_pid) /* rebind updates the registry mask */
            return nl_bind(u, ns->nl_pid, mask) == -E_ADDRINUSE ? 0 : nl_bind(u, ns->nl_pid, mask);
        ns->nl_groups = mask;
        return 0;
    }
    if (name == NETLINK_NO_ENOBUFS) {
        ns->no_enobufs = ival != 0;
        return 0;
    }
    return -E_NOPROTOOPT;
}

void netlink_init(void)
{
    memset(g_nl, 0, sizeof(g_nl));
    memset(g_nl_mcast, 0, sizeof(g_nl_mcast));
    plogk("netlink: AF_NETLINK family initialized.\n");
}
