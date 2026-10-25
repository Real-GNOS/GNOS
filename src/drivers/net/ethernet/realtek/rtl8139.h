/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8139.h — Realtek RTL8139 "10/100" ethernet driver. (GPLv2)
 *
 * The RTL8139 is the other classic teaching NIC: unlike the e1000 it has no
 * descriptor rings at all.  Receive is one circular byte buffer that the card
 * writes into (each frame prefixed by a 4-byte status/length header), and
 * transmit is four "start address / status" register pairs.  Everything is
 * reached through an I/O port BAR, which makes the driver a long sequence of
 * inb/outb rather than memory-mapped descriptor walks.
 *
 * The register protocol follows Uinxed's drivers/net/ethernet/realtek/
 * rtl8139.c (Copyright (C) 2020 ViudiraTech, Apache-2.0), reworked for GNOS:
 * no worker kthread, no net_device_t, and no private IRQ slots.  The IP stack
 * polls nic_recv(); the IRQ handler only acks the card and nudges net_poll().
 */
#ifndef GNOS_RTL8139_H
#define GNOS_RTL8139_H

#include <stdint.h>

/* Register map (RTL8139D datasheet Rev 1.11), offsets from the I/O BAR. */
#define RTL8139_REG_IDR0    0x00 /* MAC address, bytes 0-5 */
#define RTL8139_REG_MAR0    0x08 /* multicast address filter */
#define RTL8139_REG_TSD0    0x10 /* transmit status, descriptors 0-3 */
#define RTL8139_REG_TSAD0   0x20 /* transmit start address, descriptors 0-3 */
#define RTL8139_REG_RBSTART 0x30 /* receive buffer start address */
#define RTL8139_REG_CR      0x37 /* command register (byte) */
#define RTL8139_REG_CAPR    0x38 /* current address of packet read (word) */
#define RTL8139_REG_CBR     0x3a /* current buffer address (word, read-only) */
#define RTL8139_REG_IMR     0x3c /* interrupt mask register (word) */
#define RTL8139_REG_ISR     0x3e /* interrupt status register (word, W1C) */
#define RTL8139_REG_TCR     0x40 /* transmit configuration register */
#define RTL8139_REG_RCR     0x44 /* receive configuration register */
#define RTL8139_REG_9346CR  0x50 /* 93C46 command register (byte) */
#define RTL8139_REG_BMSR    0x64 /* basic mode status register (word) */

/* Command register (0x37). */
#define RTL8139_CR_BUFE  (1u << 0) /* receive buffer empty */
#define RTL8139_CR_TE    (1u << 2) /* transmitter enable */
#define RTL8139_CR_RE    (1u << 3) /* receiver enable */
#define RTL8139_CR_RESET (1u << 4) /* software reset */

/* Interrupt mask / status (0x3c/0x3e). */
#define RTL8139_ISR_ROK   (1u << 0) /* receive OK */
#define RTL8139_ISR_RER   (1u << 1) /* receive error */
#define RTL8139_ISR_TOK   (1u << 2) /* transmit OK */
#define RTL8139_ISR_TER   (1u << 3) /* transmit error */
#define RTL8139_ISR_RXOVW (1u << 4) /* receive buffer overflow */
#define RTL8139_ISR_PUN   (1u << 5) /* packet underrun / link change */
#define RTL8139_ISR_FOVW  (1u << 6) /* receive FIFO overflow */

/* Transmit configuration (0x40). */
#define RTL8139_TCR_IFG_NORMAL (3u << 24) /* standard interframe gap */
#define RTL8139_TCR_MXDMA      6u         /* 1024-byte DMA bursts */

/* Receive configuration (0x44). */
#define RTL8139_RCR_APM   (1u << 1) /* accept physical match */
#define RTL8139_RCR_AM    (1u << 2) /* accept multicast */
#define RTL8139_RCR_AB    (1u << 3) /* accept broadcast */
#define RTL8139_RCR_RXFTH 4u        /* 256-byte RX FIFO threshold */
#define RTL8139_RCR_MXDMA 6u        /* 1024-byte DMA bursts */

/* 93C46 command register (0x50). */
#define RTL8139_9346_UNLOCK 0xc0
#define RTL8139_9346_LOCK   0x00

/* Basic mode status register (0x64). */
#define RTL8139_BMSR_LINK (1u << 2) /* valid link established */

/* Transmit status descriptor (TSD) bits. */
#define RTL8139_TX_LEN_MASK 0x1fff
#define RTL8139_TX_OWN      (1u << 13) /* 1 = DMA complete, descriptor free */
#define RTL8139_TX_TUN      (1u << 14) /* transmit FIFO underrun */
#define RTL8139_TX_TOK      (1u << 15) /* transmit OK */
#define RTL8139_TX_OWC      (1u << 29) /* out of window collision */
#define RTL8139_TX_TABT     (1u << 30) /* transmit aborted */
#define RTL8139_TX_ERTXTH   8u         /* early transmit threshold: 8 * 32 bytes */

/* Receive packet header status bits. */
#define RTL8139_RX_ROK  (1u << 0) /* receive OK */
#define RTL8139_RX_FAE  (1u << 1) /* frame alignment error */
#define RTL8139_RX_CRC  (1u << 2) /* CRC error */
#define RTL8139_RX_LONG (1u << 3) /* frame exceeds 4K bytes */
#define RTL8139_RX_RUNT (1u << 4) /* runt packet */
#define RTL8139_RX_ISE  (1u << 5) /* invalid symbol error */

/* Probe the bus and bring the card up.  Returns 1 if a card was bound. */
int rtl8139_init(void);

int rtl8139_present(void);

/* Live link state, straight out of BMSR. */
int rtl8139_link_up(void);

/* The card's MAC address (6 bytes). */
const uint8_t *rtl8139_mac(void);

/*
 * Transmit one complete ethernet frame and wait for the card to finish with
 * it.  Short frames are padded to the 60-byte ethernet minimum here because
 * the RTL8139 does not do it for us.  Returns 1 on success.
 */
int rtl8139_send(const void *frame, uint16_t len);

/*
 * Take one received frame, if any is waiting.  Returns its length in bytes,
 * or 0 when the ring is empty.  Never blocks.
 */
uint16_t rtl8139_recv(void *buf, uint16_t max);

/* Counters, for whoever wants to print them.  Any pointer may be NULL. */
void rtl8139_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs);

/*
 * Headless self-test: broadcast an ARP request for the QEMU gateway
 * (10.0.2.2) and check that an answer comes back.  That proves a frame can
 * leave the card and that the receive ring delivers what comes in.  Returns
 * 1 on success (or when there is no link to test).
 */
int rtl8139_selftest(void);

#endif /* GNOS_RTL8139_H */
