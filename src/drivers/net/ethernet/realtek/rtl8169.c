/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8169.c — Realtek RTL8169 gigabit ethernet driver. (GPLv2)
 * what changed is everything around it:
 *
 *   - no worker kthread and no private IRQ slots.  The card's interrupt only
 *     acknowledges the cause and calls net_poll(), exactly like e1000.c.
 *   - no net_device_t.  The driver publishes a nic_ops_t (see nic.h) and the
 *     IP stack pulls frames with nic_recv().
 *   - transmit is synchronous, like e1000_send(): fill one descriptor, set
 *     OWN, kick the normal priority queue, and wait for OWN to come back.
 *
 * Bring-up order, and why each step exists:
 *
 *   1. Find the card and turn on memory decoding + bus mastering.  The
 *      descriptor rings are DMA memory; without bus mastering the card can
 *      never read one and every transmit silently disappears.
 *   2. Map BAR0 uncacheable.  These are device registers, not memory.
 *   3. Software reset, then read the MAC out of IDR0-5.
 *   4. Allocate one 4 KiB page per ring (256 descriptors * 16 bytes is
 *      exactly one page, and a page is far more than the 256-byte alignment
 *      the datasheet asks for) plus one page per frame buffer.
 *   5. Program RDSAR/TNPDS, unlock the config registers, set RCR/TCR, and
 *      enable the transmitter and receiver.
 *   6. Hook the PCI interrupt line.  The stack polls anyway; the interrupt
 *      exists so a process blocked in recvfrom() is woken as soon as a frame
 *      lands instead of waiting for the next timer tick.
 *
 * The one non-obvious descriptor rule: a TX slot is free when OWN is clear,
 * and the card clears OWN when the DMA is done.  Setting OWN must be the
 * last write to the descriptor, otherwise the card can start reading a
 * half-built descriptor.  That is what the barriers below are for.
 */
#include "rtl8169.h"

#include "nic.h"
#include "pci.h"
#include "io.h"
#include "idt.h"
#include "pmm.h"
#include "kstring.h"
#include "timer.h"
#include "debugcon.h"
#include "net.h"
#include "proc.h"

#define RTL8169_VENDOR_REALTEK 0x10EC

#define RTL8169_RX_COUNT 256
#define RTL8169_TX_COUNT 256
#define RTL8169_BUFFER_SIZE 2048 /* one frame buffer per descriptor */
#define RTL8169_MAX_FRAME (NET_MTU + 18)
#define RTL8169_CRC_LEN 4 /* the length reported by the chip includes the FCS */
#define RTL8169_ETH_ZLEN 60
#define RTL8169_RESET_POLL 100000
#define RTL8169_TX_POLL 200000

#define RTL8169_RX_INTS \
    (RTL8169_ISR_ROK | RTL8169_ISR_RER | RTL8169_ISR_RDU | RTL8169_ISR_FOVW)
#define RTL8169_INT_MASK \
    (RTL8169_RX_INTS | RTL8169_ISR_TOK | RTL8169_ISR_TER | RTL8169_ISR_LINKCHG | RTL8169_ISR_TDU)

/* Device IDs that use the classic RTL8169 descriptor and register layout.
 * The RTL8168 (10ec:8168) has a different TX descriptor and is deliberately
 * not listed here. */
typedef struct {
    uint16_t vendor;
    uint16_t device;
} rtl8169_id_t;

static const rtl8169_id_t rtl8169_ids[] = {
    {RTL8169_VENDOR_REALTEK, 0x8161}, /* RTL8169/RTL8111SC */
    {RTL8169_VENDOR_REALTEK, 0x8169}, /* RTL8169 */
    {0x1259, 0xc107},                 /* Kontron */
    {0x1737, 0x1032},                 /* Linksys EG1032 */
    {0x16ec, 0x0116},                 /* US Robotics */
};

static volatile uint8_t *g_mmio; /* BAR0, uncacheable */
static int               g_ok;
static uint8_t           g_irq;
static uint8_t           g_mac[6];

static volatile rtl8169_desc_t *g_rx;
static volatile rtl8169_desc_t *g_tx;
static uint64_t                 g_rx_phys;
static uint64_t                 g_tx_phys;

static uint8_t *g_rxbuf[RTL8169_RX_COUNT];
static uint64_t g_rxbuf_phys[RTL8169_RX_COUNT];
static uint8_t *g_txbuf[RTL8169_TX_COUNT];
static uint64_t g_txbuf_phys[RTL8169_TX_COUNT];

static unsigned g_rx_next;
static unsigned g_tx_next;

static uint64_t g_tx_frames, g_rx_frames, g_irqs, g_rx_errors;

static const nic_ops_t rtl8169_nic = {
    .name     = "rtl8169",
    .mac      = rtl8169_mac,
    .link_up  = rtl8169_link_up,
    .send     = rtl8169_send,
    .recv     = rtl8169_recv,
    .selftest = rtl8169_selftest,
};

/* ---- register access -------------------------------------------------- */
static inline uint8_t r8(uint32_t reg)
{
    return *(volatile uint8_t *)(g_mmio + reg);
}

static inline uint16_t r16(uint32_t reg)
{
    return *(volatile uint16_t *)(g_mmio + reg);
}

static inline uint32_t r32(uint32_t reg)
{
    return *(volatile uint32_t *)(g_mmio + reg);
}

static inline void w8(uint32_t reg, uint8_t v)
{
    *(volatile uint8_t *)(g_mmio + reg) = v;
}

static inline void w16(uint32_t reg, uint16_t v)
{
    *(volatile uint16_t *)(g_mmio + reg) = v;
}

static inline void w32(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(g_mmio + reg) = v;
}

/* Force a previous MMIO write to complete by reading back the PHY status
 * register, exactly as Uinxed does. */
static inline void write_flush(void)
{
    (void)r32(RTL8169_REG_PHYSTATUS);
}

/* ---- probing ---------------------------------------------------------- */
static const rtl8169_id_t *rtl8169_match(uint16_t vendor, uint16_t device)
{
    for (unsigned i = 0; i < sizeof(rtl8169_ids) / sizeof(rtl8169_ids[0]); i++)
        if (rtl8169_ids[i].vendor == vendor && rtl8169_ids[i].device == device)
            return &rtl8169_ids[i];
    return 0;
}

static int rtl8169_valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    uint8_t all = 0xFF;

    for (int i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    return any != 0 && all != 0xFF && !(mac[0] & 1);
}

static void rtl8169_reset(void)
{
    w8(RTL8169_REG_CR, RTL8169_CR_RESET);
    write_flush();

    for (int i = 0; i < RTL8169_RESET_POLL; i++) {
        if (!(r8(RTL8169_REG_CR) & RTL8169_CR_RESET))
            return;
        io_delay();
    }
    dbg_puts("RTL8169: reset bit did not clear, continuing\r\n");
}

static void rtl8169_read_mac(void)
{
    for (int i = 0; i < 6; i++)
        g_mac[i] = r8(RTL8169_REG_IDR0 + (uint32_t)i);

    if (!rtl8169_valid_mac(g_mac)) {
        dbg_puts("RTL8169: invalid MAC, using a locally administered one\r\n");
        g_mac[0] = 0x02;
        g_mac[1] = 0x00;
        g_mac[2] = 0x00;
        g_mac[3] = 0x00;
        g_mac[4] = 0x00;
        g_mac[5] = 0x01;
    }
}

/* ---- DMA memory ------------------------------------------------------- */
static void rtl8169_free_dma(void)
{
    for (int i = 0; i < RTL8169_RX_COUNT; i++) {
        if (g_rxbuf_phys[i])
            pmm_free(g_rxbuf_phys[i]);
        g_rxbuf_phys[i] = 0;
        g_rxbuf[i]      = 0;
    }
    for (int i = 0; i < RTL8169_TX_COUNT; i++) {
        if (g_txbuf_phys[i])
            pmm_free(g_txbuf_phys[i]);
        g_txbuf_phys[i] = 0;
        g_txbuf[i]      = 0;
    }
    if (g_rx_phys) {
        pmm_free(g_rx_phys);
        g_rx_phys = 0;
        g_rx      = 0;
    }
    if (g_tx_phys) {
        pmm_free(g_tx_phys);
        g_tx_phys = 0;
        g_tx      = 0;
    }
}

static int rtl8169_alloc_dma(void)
{
    /* 256 descriptors * 16 bytes = 4096 bytes, one page each. */
    g_rx_phys = pmm_alloc_zeroed();
    g_tx_phys = pmm_alloc_zeroed();
    if (!g_rx_phys || !g_tx_phys) {
        dbg_puts("RTL8169: out of memory for the descriptor rings\r\n");
        rtl8169_free_dma();
        return 0;
    }
    g_rx = (volatile rtl8169_desc_t *)pmm_virt(g_rx_phys);
    g_tx = (volatile rtl8169_desc_t *)pmm_virt(g_tx_phys);

    for (int i = 0; i < RTL8169_RX_COUNT; i++) {
        g_rxbuf_phys[i] = pmm_alloc_zeroed();
        if (!g_rxbuf_phys[i]) {
            dbg_puts("RTL8169: out of memory for a receive buffer\r\n");
            rtl8169_free_dma();
            return 0;
        }
        g_rxbuf[i] = (uint8_t *)pmm_virt(g_rxbuf_phys[i]);

        uint32_t cmd = RTL8169_DESC_OWN | RTL8169_BUFFER_SIZE;
        if (i == RTL8169_RX_COUNT - 1)
            cmd |= RTL8169_DESC_EOR;
        g_rx[i].command  = cmd;
        g_rx[i].vlan     = 0;
        g_rx[i].low_buf  = (uint32_t)g_rxbuf_phys[i];
        g_rx[i].high_buf = (uint32_t)(g_rxbuf_phys[i] >> 32);
    }

    for (int i = 0; i < RTL8169_TX_COUNT; i++) {
        g_txbuf_phys[i] = pmm_alloc_zeroed();
        if (!g_txbuf_phys[i]) {
            dbg_puts("RTL8169: out of memory for a transmit buffer\r\n");
            rtl8169_free_dma();
            return 0;
        }
        g_txbuf[i] = (uint8_t *)pmm_virt(g_txbuf_phys[i]);

        uint32_t cmd = 0; /* OWN clear: the slot starts free */
        if (i == RTL8169_TX_COUNT - 1)
            cmd |= RTL8169_DESC_EOR;
        g_tx[i].command  = cmd;
        g_tx[i].vlan     = 0;
        g_tx[i].low_buf  = (uint32_t)g_txbuf_phys[i];
        g_tx[i].high_buf = (uint32_t)(g_txbuf_phys[i] >> 32);
    }

    __sync_synchronize();
    return 1;
}

/* ---- hardware programming --------------------------------------------- */
static void rtl8169_program_hw(void)
{
    w16(RTL8169_REG_IMR, 0);
    w16(RTL8169_REG_ISR, 0xFFFF); /* W1C: clear anything left from reset */
    write_flush();

    /* The C+ command register must be programmed before the rings. */
    w16(RTL8169_REG_CPLUSCR, RTL8169_CPLUS_DAC);

    /* 64-bit descriptor ring base addresses. */
    w32(RTL8169_REG_TNPDS, (uint32_t)g_tx_phys);
    w32(RTL8169_REG_TNPDS + 4, (uint32_t)(g_tx_phys >> 32));
    w32(RTL8169_REG_RDSAR, (uint32_t)g_rx_phys);
    w32(RTL8169_REG_RDSAR + 4, (uint32_t)(g_rx_phys >> 32));

    w8(RTL8169_REG_9346CR, RTL8169_9346_UNLOCK);

    /* Receive: accept everything, unlimited DMA burst/FIFO. */
    w32(RTL8169_REG_RCR,
        RTL8169_RCR_AAP | RTL8169_RCR_APM | RTL8169_RCR_AM | RTL8169_RCR_AB |
            RTL8169_RCR_MXDMA | RTL8169_RCR_RXFTH);

    /* The transmitter must be enabled before TCR is written. */
    w8(RTL8169_REG_CR, RTL8169_CR_TE);
    w32(RTL8169_REG_TCR, RTL8169_TCR_IFG_NORMAL | RTL8169_TCR_MXDMA_UNLIMITED);

    w16(RTL8169_REG_RMS, RTL8169_BUFFER_SIZE);
    w8(RTL8169_REG_MTPS, 0x3b); /* 1518-byte maximum transmit packet */

    w8(RTL8169_REG_9346CR, RTL8169_9346_LOCK);
    w8(RTL8169_REG_CR, RTL8169_CR_TE | RTL8169_CR_RE);
    write_flush();
}

/* ---- link ------------------------------------------------------------- */
static int rtl8169_link_state(void)
{
    return !!(r8(RTL8169_REG_PHYSTATUS) & RTL8169_PHYSTATUS_LINKSTS);
}

int rtl8169_link_up(void)
{
    return g_ok && rtl8169_link_state();
}

/* ---- receive ---------------------------------------------------------- */
uint16_t rtl8169_recv(void *buf, uint16_t max)
{
    if (!g_ok || !buf)
        return 0;

    /* A burst of malformed descriptors must not hold the caller here
     * forever; after eight of them, let the next poll try again. */
    for (int guard = 0; guard < 8; guard++) {
        volatile rtl8169_desc_t *d = &g_rx[g_rx_next];
        uint32_t                 cmd = d->command;
        __sync_synchronize();

        if (cmd & RTL8169_DESC_OWN)
            return 0; /* the card has not filled this one */

        uint32_t length   = cmd & RTL8169_RX_LEN_MASK;
        int      complete = (cmd & RTL8169_DESC_FS) && (cmd & RTL8169_DESC_LS);
        int      good     = complete && !(cmd & RTL8169_RX_ERROR_MASK) &&
                       length > RTL8169_CRC_LEN && length <= RTL8169_BUFFER_SIZE &&
                       length - RTL8169_CRC_LEN <= RTL8169_MAX_FRAME;

        uint16_t n = 0;
        if (good) {
            n = (uint16_t)(length - RTL8169_CRC_LEN);
            if (n > max)
                n = max;
            memcpy(buf, g_rxbuf[g_rx_next], n);
            g_rx_frames++;
        } else {
            g_rx_errors++;
        }

        /* Hand the descriptor back to the card. */
        uint32_t rearm = RTL8169_DESC_OWN | RTL8169_BUFFER_SIZE;
        if (g_rx_next == RTL8169_RX_COUNT - 1)
            rearm |= RTL8169_DESC_EOR;
        d->command = rearm;
        __sync_synchronize();

        g_rx_next = (g_rx_next + 1) % RTL8169_RX_COUNT;
        if (good)
            return n;
    }
    return 0;
}

/* ---- transmit --------------------------------------------------------- */
int rtl8169_send(const void *frame, uint16_t len)
{
    if (!g_ok || !frame || len == 0 || len > RTL8169_MAX_FRAME)
        return 0;

    unsigned                 i = g_tx_next;
    volatile rtl8169_desc_t *d = &g_tx[i];

    /* OWN == 0 means the slot came back.  A synchronous sender should
     * almost never see a busy slot, but a previous timeout can leave one
     * live, so wait rather than scribbling over a DMA in progress. */
    for (int n = 0; n < RTL8169_TX_POLL && (d->command & RTL8169_DESC_OWN); n++)
        io_delay();
    if (d->command & RTL8169_DESC_OWN)
        return 0;

    memcpy(g_txbuf[i], frame, len);

    /* The chip does not pad short frames for us; an ARP request is 42 bytes
     * and must go out as 60. */
    if (len < RTL8169_ETH_ZLEN) {
        memset(g_txbuf[i] + len, 0, RTL8169_ETH_ZLEN - len);
        len = RTL8169_ETH_ZLEN;
    }

    uint32_t cmd = RTL8169_DESC_FS | RTL8169_DESC_LS | (uint32_t)len;
    if (i == RTL8169_TX_COUNT - 1)
        cmd |= RTL8169_DESC_EOR;

    __sync_synchronize(); /* buffer contents before the descriptor */
    d->command = cmd;     /* OWN still clear */
    __sync_synchronize();
    d->command = cmd | RTL8169_DESC_OWN;
    __sync_synchronize();

    w8(RTL8169_REG_TPPOLL, RTL8169_TPPOLL_NPQ);

    /* Wait for the card to come back with OWN clear. */
    for (int n = 0; n < RTL8169_TX_POLL; n++) {
        if (!(d->command & RTL8169_DESC_OWN)) {
            g_tx_next = (i + 1) % RTL8169_TX_COUNT;
            g_tx_frames++;
            return 1;
        }
        io_delay();
    }
    return 0;
}

/* ---- interrupts ------------------------------------------------------- */
static void rtl8169_irq(regs_t *r)
{
    (void)r;

    uint16_t st = r16(RTL8169_REG_ISR);
    if (!st)
        return; /* not us: PCI interrupt lines are shared */

    w16(RTL8169_REG_ISR, st); /* W1C */
    g_irqs++;

    if (st & RTL8169_ISR_LINKCHG) {
        dbg_puts("RTL8169: link ");
        dbg_puts(rtl8169_link_state() ? "up\r\n" : "down\r\n");
    }
    if (st & (RTL8169_ISR_RER | RTL8169_ISR_RDU | RTL8169_ISR_FOVW))
        g_rx_errors++;
    if (st & RTL8169_RX_INTS) {
        net_poll();
        sched_wake_reason(WAIT_NET);
    }
}

/* ---- public counters / identity --------------------------------------- */
int rtl8169_present(void)
{
    return g_ok;
}

const uint8_t *rtl8169_mac(void)
{
    return g_mac;
}

void rtl8169_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs)
{
    if (tx_frames)
        *tx_frames = g_tx_frames;
    if (rx_frames)
        *rx_frames = g_rx_frames;
    if (irqs)
        *irqs = g_irqs;
}

/* ---- bring-up --------------------------------------------------------- */
int rtl8169_init(void)
{
    const pci_dev_t *d = 0;

    for (int i = 0; i < g_pci_count; i++) {
        if (rtl8169_match(g_pci_devs[i].vendor, g_pci_devs[i].device)) {
            d = &g_pci_devs[i];
            break;
        }
    }
    if (!d) {
        dbg_puts("RTL8169: no 10EC:8169 on the bus, skipping\r\n");
        return 0;
    }

    pci_enable(d);
    uint64_t base = pci_map_bar(d, 0);
    if (!base) {
        dbg_puts("RTL8169: BAR0 is not a mappable memory region\r\n");
        return 0;
    }
    g_mmio = (volatile uint8_t *)base;
    g_irq  = d->irq_line;

    rtl8169_reset();
    timer_delay_ms(10); /* let the PHY come out of reset */
    rtl8169_read_mac();

    if (!rtl8169_alloc_dma())
        return 0;

    rtl8169_program_hw();
    g_ok = 1;

    /* Only now that a ring exists is it safe to let the card interrupt us. */
    if (g_irq < 16) {
        irq_install(g_irq, rtl8169_irq, "rtl8169");
        w16(RTL8169_REG_IMR, RTL8169_INT_MASK);
        write_flush();
    }

    (void)nic_register(&rtl8169_nic);

    dbg_puts("RTL8169: ");
    dbg_puts_hexn(d->vendor, 4);
    dbg_puts(":");
    dbg_puts_hexn(d->device, 4);
    dbg_puts(" up, mac ");
    for (int i = 0; i < 6; i++) {
        dbg_puts_hexn(g_mac[i], 2);
        if (i != 5)
            dbg_puts(":");
    }
    dbg_puts(", irq ");
    dbg_puts_dec(g_irq);
    dbg_puts(", link ");
    dbg_puts(rtl8169_link_state() ? "up" : "down");
    dbg_puts("\r\n");
    return 1;
}

/* ---- self-test -------------------------------------------------------- */
/*
 * Nothing here is a protocol stack: the frame is 42 bytes written by hand.
 * Broadcast "who has 10.0.2.2", and QEMU's user-mode network stack -- which
 * owns that address -- answers with its MAC.  A pass proves the transmit
 * descriptor kicked, the wire is connected, and the receive ring's length
 * arithmetic is right; a failure is a driver bug, not a stack bug.
 */
#define ETHERTYPE_ARP 0x0806

static const uint8_t IP_SELF[4] = {10, 0, 2, 15}; /* QEMU hands us .15 */
static const uint8_t IP_GW[4]   = {10, 0, 2, 2};  /* the gateway/host */

static uint16_t recv_wait(void *buf, uint16_t max, int tries)
{
    for (int n = 0; n < tries; n++) {
        uint16_t len = rtl8169_recv(buf, max);
        if (len)
            return len;
        io_delay();
    }
    return 0;
}

static void rtl8169_dump_state(void)
{
    dbg_puts("       CR=0x");
    dbg_puts_hexn(r8(RTL8169_REG_CR), 2);
    dbg_puts(" ISR=0x");
    dbg_puts_hexn(r16(RTL8169_REG_ISR), 4);
    dbg_puts(" IMR=0x");
    dbg_puts_hexn(r16(RTL8169_REG_IMR), 4);
    dbg_puts(" RCR=0x");
    dbg_puts_hexn(r32(RTL8169_REG_RCR), 8);
    dbg_puts(" TCR=0x");
    dbg_puts_hexn(r32(RTL8169_REG_TCR), 8);
    dbg_puts(" PHY=0x");
    dbg_puts_hexn(r8(RTL8169_REG_PHYSTATUS), 2);
    dbg_puts(" rx0=0x");
    dbg_puts_hexn(g_rx[0].command, 8);
    dbg_puts(" tx0=0x");
    dbg_puts_hexn(g_tx[0].command, 8);
    dbg_puts("\r\n");
}

static int test_arp(void)
{
    static uint8_t got[RTL8169_BUFFER_SIZE];
    uint8_t        f[42];

    if (!rtl8169_link_state()) {
        dbg_puts("RTL8169: ARP skipped (no link)\r\n");
        return 1; /* not a driver bug */
    }

    memset(f, 0, sizeof f);
    memset(f + 0, 0xFF, 6);  /* dst: broadcast */
    memcpy(f + 6, g_mac, 6); /* src: us */
    f[12] = ETHERTYPE_ARP >> 8;
    f[13] = ETHERTYPE_ARP & 0xFF;
    f[14] = 0x00;
    f[15] = 0x01; /* htype: ethernet */
    f[16] = 0x08;
    f[17] = 0x00; /* ptype: IPv4 */
    f[18] = 6;    /* hardware address length */
    f[19] = 4;    /* protocol address length */
    f[20] = 0x00;
    f[21] = 0x01;               /* oper: request */
    memcpy(f + 22, g_mac, 6);   /* sender hardware address */
    memcpy(f + 28, IP_SELF, 4); /* sender protocol address */
    /* target hardware address stays zero -- that is what we are asking for */
    memcpy(f + 38, IP_GW, 4); /* target protocol address */

    if (!rtl8169_send(f, sizeof f)) {
        dbg_puts("RTL8169: ARP FAIL (request never left the card)\r\n");
        rtl8169_dump_state();
        return 0;
    }

    /* The reply comes back through the host's event loop, so it takes far
     * longer than a loopback frame; be patient before declaring failure. */
    for (int round = 0; round < 40; round++) {
        uint16_t len = recv_wait(got, sizeof got, 20000);
        if (!len)
            continue;
        if (len < 42)
            continue;
        uint16_t et = (uint16_t)((got[12] << 8) | got[13]);
        if (et != ETHERTYPE_ARP)
            continue; /* something else on the wire */
        if (got[20] != 0x00 || got[21] != 0x02)
            continue; /* not a reply */
        if (memcmp(got + 28, IP_GW, 4) != 0)
            continue; /* not the gateway answering */

        dbg_puts("RTL8169: ARP PASS, 10.0.2.2 is at ");
        for (int i = 0; i < 6; i++) {
            dbg_puts_hexn(got[22 + i], 2);
            if (i != 5)
                dbg_puts(":");
        }
        dbg_puts("\r\n");
        return 1;
    }

    dbg_puts("RTL8169: ARP FAIL (no reply from 10.0.2.2)\r\n");
    rtl8169_dump_state();
    return 0;
}

int rtl8169_selftest(void)
{
    if (!g_ok)
        return 0;

    /* Give the link a moment to settle before the first frame. */
    timer_delay_ms(100);

    int ok = test_arp();

    dbg_puts("RTL8169: tx=");
    dbg_puts_dec((uint32_t)g_tx_frames);
    dbg_puts(" rx=");
    dbg_puts_dec((uint32_t)g_rx_frames);
    dbg_puts(" frames, rx_errors=");
    dbg_puts_dec((uint32_t)g_rx_errors);
    dbg_puts("\r\n");
    return ok;
}
