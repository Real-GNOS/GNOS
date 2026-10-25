/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8169.h — Realtek RTL8169 gigabit ethernet driver. (GPLv2)
 *
 * The RTL8169 is the gigabit successor to the RTL8139, but almost nothing
 * below the software interface is shared: receive and transmit both use
 * 16-byte descriptor rings in memory (the card DMAs them itself), the
 * registers live in a memory-mapped BAR instead of I/O ports, and there are
 * 256 descriptors per direction instead of four start-address registers.
 *
 * The descriptor layout and the bring-up order follow Uinxed's
 * drivers/net/ethernet/realtek/rtl8169.c (Copyright (C) 2020 ViudiraTech,
 * Apache-2.0), reworked for GNOS: no worker kthread, no net_device_t, and no
 * private IRQ slots.  The IP stack pulls frames with nic_recv(); the IRQ
 * handler only acknowledges the card and nudges net_poll().
 */
#ifndef GNOS_RTL8169_H
#define GNOS_RTL8169_H

#include <stdint.h>

/* Register map (RTL8169S/RTL8110S datasheet Rev 1.3), offsets from BAR0. */
#define RTL8169_REG_IDR0      0x0000 /* MAC address, bytes 0-5 */
#define RTL8169_REG_TNPDS     0x0020 /* TX descriptor start, 64-bit */
#define RTL8169_REG_CR        0x0037 /* command register (byte) */
#define RTL8169_REG_TPPOLL    0x0038 /* transmit priority polling (byte) */
#define RTL8169_REG_IMR       0x003c /* interrupt mask (word) */
#define RTL8169_REG_ISR       0x003e /* interrupt status (word, W1C) */
#define RTL8169_REG_TCR       0x0040 /* transmit configuration */
#define RTL8169_REG_RCR       0x0044 /* receive configuration */
#define RTL8169_REG_9346CR    0x0050 /* 93C46/93C56 command (byte) */
#define RTL8169_REG_PHYSTATUS 0x006c /* PHY status (byte) */
#define RTL8169_REG_RMS       0x00da /* receive maximum size (word) */
#define RTL8169_REG_CPLUSCR   0x00e0 /* C+ command (word) */
#define RTL8169_REG_RDSAR     0x00e4 /* RX descriptor start, 64-bit */
#define RTL8169_REG_MTPS      0x00ec /* maximum transmit packet size (byte) */

/* Command register (0x37). */
#define RTL8169_CR_TE    (1u << 2) /* transmitter enable */
#define RTL8169_CR_RE    (1u << 3) /* receiver enable */
#define RTL8169_CR_RESET (1u << 4) /* software reset */

/* Transmit priority polling (0x38). */
#define RTL8169_TPPOLL_NPQ (1u << 6) /* normal priority queue poll */

/* Interrupt mask / status (0x3c/0x3e). */
#define RTL8169_ISR_ROK     (1u << 0) /* receive OK */
#define RTL8169_ISR_RER     (1u << 1) /* receive error */
#define RTL8169_ISR_TOK     (1u << 2) /* transmit OK */
#define RTL8169_ISR_TER     (1u << 3) /* transmit error */
#define RTL8169_ISR_RDU     (1u << 4) /* receive descriptor unavailable */
#define RTL8169_ISR_LINKCHG (1u << 5) /* link change */
#define RTL8169_ISR_FOVW    (1u << 6) /* receive FIFO overflow */
#define RTL8169_ISR_TDU     (1u << 7) /* transmit descriptor unavailable */
#define RTL8169_ISR_SWINT   (1u << 8) /* software interrupt */
#define RTL8169_ISR_TIMEOUT (1u << 14)
#define RTL8169_ISR_SERR    (1u << 15)

/* Transmit configuration (0x40). */
#define RTL8169_TCR_IFG_NORMAL      (3u << 24)
#define RTL8169_TCR_MXDMA_UNLIMITED (7u << 8)

/* Receive configuration (0x44). */
#define RTL8169_RCR_AAP   (1u << 0)  /* accept all packets */
#define RTL8169_RCR_APM   (1u << 1)  /* accept physical match */
#define RTL8169_RCR_AM    (1u << 2)  /* accept multicast */
#define RTL8169_RCR_AB    (1u << 3)  /* accept broadcast */
#define RTL8169_RCR_AR    (1u << 4)  /* accept runt */
#define RTL8169_RCR_AER   (1u << 5)  /* accept error packets */
#define RTL8169_RCR_MXDMA (7u << 8)  /* unlimited DMA burst */
#define RTL8169_RCR_RXFTH (7u << 13) /* no FIFO threshold */

/* C+ command (0xe0). */
#define RTL8169_CPLUS_DAC (1u << 4) /* PCI dual address cycle (64-bit DMA) */

/* 93C46/93C56 command (0x50). */
#define RTL8169_9346_UNLOCK 0xc0
#define RTL8169_9346_LOCK   0x00

/* PHY status (0x6c). */
#define RTL8169_PHYSTATUS_LINKSTS (1u << 1)

/* Descriptor dword 0 bits. */
#define RTL8169_DESC_OWN     (1u << 31) /* set: owned by the card */
#define RTL8169_DESC_EOR     (1u << 30) /* end of ring */
#define RTL8169_DESC_FS      (1u << 29) /* first segment */
#define RTL8169_DESC_LS      (1u << 28) /* last segment */
#define RTL8169_TX_LEN_MASK  0x0000ffffu
#define RTL8169_RX_LEN_MASK  0x00003fffu

/* RX status error summary: RWT(22) | RES(21) | RUNT(20) | CRC(19). */
#define RTL8169_RX_ERROR_MASK (0x0fu << 19)

/* Descriptor dword 0 carries ownership/status/length; dwords 2/3 hold the
 * buffer address.  packed: the card reads this layout byte for byte. */
typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t vlan;
    uint32_t low_buf;
    uint32_t high_buf;
} rtl8169_desc_t;

/* Probe the bus and bring the card up.  Returns 1 if a card was bound. */
int rtl8169_init(void);

int rtl8169_present(void);

/* Live link state, straight out of the PHY status register. */
int rtl8169_link_up(void);

/* The card's MAC address (6 bytes). */
const uint8_t *rtl8169_mac(void);

/*
 * Transmit one complete ethernet frame and wait for the descriptor to come
 * back.  Short frames are padded to the 60-byte ethernet minimum here
 * because the RTL8169 does not do it for us.  Returns 1 on success.
 */
int rtl8169_send(const void *frame, uint16_t len);

/*
 * Take one received frame, if any is waiting.  Returns its length in bytes,
 * or 0 when the ring is empty.  Never blocks.
 */
uint16_t rtl8169_recv(void *buf, uint16_t max);

/* Counters, for whoever wants to print them.  Any pointer may be NULL. */
void rtl8169_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs);

/*
 * Headless self-test: broadcast an ARP request for the QEMU gateway
 * (10.0.2.2) and check that an answer comes back.  Returns 1 on success (or
 * when there is no link to test).
 */
int rtl8169_selftest(void);

#endif /* GNOS_RTL8169_H */
