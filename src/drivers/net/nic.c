/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nic.c — first-registered NIC wins. (GPLv2)
 *
 * See nic.h for why this indirection exists.  There is no locking: probing
 * happens once during boot, before any other CPU or task can touch the
 * registry, and the pointer never changes afterwards.
 */
#include "nic.h"

static const nic_ops_t *g_nic;

int nic_register(const nic_ops_t *ops)
{
    if (!ops || !ops->send || !ops->recv)
        return 0;
    if (g_nic)
        return 0;
    g_nic = ops;
    return 1;
}

int nic_present(void)
{
    return g_nic != 0;
}

const char *nic_name(void)
{
    return g_nic ? g_nic->name : "none";
}

const uint8_t *nic_mac(void)
{
    static const uint8_t zero[6];

    if (!g_nic || !g_nic->mac)
        return zero;
    return g_nic->mac();
}

int nic_link_up(void)
{
    return g_nic && g_nic->link_up ? g_nic->link_up() : 0;
}

int nic_send(const void *frame, uint16_t len)
{
    if (!g_nic)
        return 0;
    return g_nic->send(frame, len);
}

uint16_t nic_recv(void *buf, uint16_t max)
{
    if (!g_nic)
        return 0;
    return g_nic->recv(buf, max);
}

int nic_selftest(void)
{
    if (!g_nic || !g_nic->selftest)
        return 0;
    return g_nic->selftest();
}
