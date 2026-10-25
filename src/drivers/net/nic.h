/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nic.h — the one NIC the IP stack talks to. (GPLv2)
 *
 * net.c used to call e1000_* directly, which meant that adding a second
 * ethernet driver required editing the protocol stack.  This is the seam
 * that removes that coupling: a driver publishes a nic_ops_t, the first one
 * to register wins, and net.c only ever sees nic_send()/nic_recv().
 *
 * The contract is deliberately small and polled:
 *
 *   send()      one complete ethernet frame in, 1 on success, 0 on failure.
 *               It may spin waiting for the card, like e1000_send() always
 *               has; there is no transmit queue above this line.
 *   recv()      one frame out, 0 when the ring is empty.  Never blocks.
 *   link_up()   the current link state, read from the card.
 *   mac()       the six bytes the card transmits from.
 *
 * One NIC is enough for the current stack (g_ifs has a single ethernet
 * interface), so the registry holds exactly one entry rather than a list.
 */
#ifndef GNOS_NIC_H
#define GNOS_NIC_H

#include <stdint.h>

typedef struct nic_ops {
    const char *name; /* "e1000", "rtl8139", ... */

    /* The card's own unicast address; must stay valid for the driver's life. */
    const uint8_t *(*mac)(void);

    /* Non-zero while the link is up. */
    int (*link_up)(void);

    /* Transmit one frame (destination MAC first).  Returns 1 on success. */
    int (*send)(const void *frame, uint16_t len);

    /* Receive one frame; returns its length, or 0 when nothing is waiting. */
    uint16_t (*recv)(void *buf, uint16_t max);

    /* Optional boot-time check; may be NULL. */
    int (*selftest)(void);
} nic_ops_t;

/*
 * Publish a driver.  The first registration wins and later ones are ignored,
 * so probe order in kernel.c is the priority order.  Returns 1 if this driver
 * became the active NIC, 0 if another one already had the slot.
 */
int nic_register(const nic_ops_t *ops);

/* Was a NIC bound?  Everything below is safe to call when the answer is no. */
int nic_present(void);

/* Name of the bound driver, or "none". */
const char *nic_name(void);

const uint8_t *nic_mac(void);
int            nic_link_up(void);
int            nic_send(const void *frame, uint16_t len);
uint16_t       nic_recv(void *buf, uint16_t max);
int            nic_selftest(void);

#endif /* GNOS_NIC_H */
