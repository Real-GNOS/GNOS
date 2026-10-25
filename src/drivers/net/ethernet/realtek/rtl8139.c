/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8139.c — Realtek RTL8139 "10/100" ethernet driver. (GPLv2)
 *
 * The register protocol and the bring-up order are the same;
 * what changed is everything around it:
 *
 *   - no worker kthread and no private IRQ slots.  The card's interrupt only
 *     acknowledges the cause and calls net_poll(), exactly like e1000.c.
 *   - no net_device_t.  The driver publishes a nic_ops_t (see nic.h) and the
 *     IP stack pulls frames with nic_recv().
 *   - transmit is synchronous, like e1000_send(): fill one of the four
 *     descriptors, kick it, and wait for OWN|TOK before returning.
 *
 * Bring-up order, and why each step exists:
 *
 *   1. Find the card and turn on I/O decoding + bus mastering.  The RTL8139
 *      is an I/O-port device, but it still DMAs frames in and out of main
 *      memory, so bus mastering is not optional.
 *   2. Software reset.  QEMU keeps the reset bit asserted on some versions,
 *      so the poll is bounded and we continue rather than fail.
 *   3. Read the MAC from IDR0-5 (the chip latched it from the serial EEPROM
 *      at power-on), substituting a locally administered address if the
 *      result is not a valid unicast.
 *   4. Allocate the 32 KiB + 16 byte receive ring and four transmit buffers
 *      from the physical allocator; the card sees physical addresses.
 *   5. Program RBSTART/CAPR, unlock the config registers to write IDR, then
 *      enable the transmitter and receiver and set TCR/RCR.
 *   6. Hook the PCI interrupt line.  The stack polls anyway; the interrupt
 *      exists so a process blocked in recvfrom() is woken as soon as a frame
 *      lands instead of waiting for the next timer tick.
 *
 * Receive is a circular byte buffer, not a descriptor ring.  Each frame the
 * card writes is preceded by a 4-byte header: status in the low half, total
 * length (including the 4-byte FCS) in the high half.  The next header starts
 * at (current + length + 4) rounded up to a multiple of four, wrapping at
 * 32 KiB.  CAPR tells the card how far software has consumed; getting that
 * arithmetic wrong is the classic way to make an RTL8139 driver receive
 * exactly one frame and then stop.
 */
#include "rtl8139.h"

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

#define RTL8139_VENDOR_REALTEK 0x10EC

#define RTL8139_RX_BUF_IDX 2 /* RCR receive ring length: 32 KiB + 16 bytes */
#define RTL8139_RX_BUF_SIZE (8192u << RTL8139_RX_BUF_IDX)
#define RTL8139_RX_RING_PAGES \
    ((RTL8139_RX_BUF_SIZE + 16 + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE)

#define RTL8139_TX_COUNT 4    /* four hardware transmit descriptors */
#define RTL8139_TX_MAX   2048 /* one page per transmit buffer */
#define RTL8139_ETH_ZLEN 60   /* minimum frame without the FCS */
#define RTL8139_CRC_LEN  4    /* the length in the RX header includes the FCS */
#define RTL8139_MAX_FRAME 1518

#define RTL8139_RESET_POLL 1000
#define RTL8139_TX_POLL    200000

#define RTL8139_RX_ERRORS \
    (RTL8139_RX_FAE | RTL8139_RX_CRC | RTL8139_RX_LONG | RTL8139_RX_RUNT | RTL8139_RX_ISE)
#define RTL8139_RX_INTS (RTL8139_ISR_ROK | RTL8139_ISR_RER | RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)
#define RTL8139_INT_MASK (RTL8139_RX_INTS | RTL8139_ISR_PUN)

/* Device IDs that use the classic RTL8139 register layout. */
typedef struct {
    uint16_t vendor;
    uint16_t device;
} rtl8139_id_t;

static const rtl8139_id_t rtl8139_ids[] = {
    {RTL8139_VENDOR_REALTEK, 0x8129}, /* RTL8129 */
    {RTL8139_VENDOR_REALTEK, 0x8139}, /* RTL8139 / RTL8139D */
    {RTL8139_VENDOR_REALTEK, 0x8138}, /* RTL8139B */
    {0x1113, 0x1211},                 /* Accton EN-1207D / SMC1211TX */
    {0x1186, 0x1300},                 /* D-Link DFE-538TX */
    {0x018A, 0x0106},                 /* LevelOne FPC-0106Tx */
    {0x021B, 0x8139},                 /* Compaq HNE-300 */
};

static int      g_ok;
static uint16_t g_io;  /* I/O port BAR */
static uint8_t  g_irq; /* PCI interrupt line */
static uint8_t  g_mac[6];

/* Receive ring.  g_cur_rx grows without bound; the ring offset is the
 * remainder modulo 32 KiB, exactly as Linux's 8139too does it. */
static volatile uint8_t *g_rx;
static uint64_t          g_rx_phys;
static uint32_t          g_cur_rx;

/* Transmit buffers, one page each.  The descriptor registers hold physical
 * addresses, so the virtual mapping is kept only for memcpy(). */
static uint64_t g_tx_phys[RTL8139_TX_COUNT];
static uint8_t *g_tx[RTL8139_TX_COUNT];
static unsigned g_tx_next;

static uint64_t g_tx_frames, g_rx_frames, g_irqs;

/* Published once, after the card is fully programmed. */
static const nic_ops_t rtl8139_nic = {
    .name     = "rtl8139",
    .mac      = rtl8139_mac,
    .link_up  = rtl8139_link_up,
    .send     = rtl8139_send,
    .recv     = rtl8139_recv,
    .selftest = rtl8139_selftest,
};

/* ---- register access -------------------------------------------------- */
static inline uint8_t r8(uint32_t reg)
{
    return inb((uint16_t)(g_io + reg));
}

static inline uint16_t r16(uint32_t reg)
{
    return inw((uint16_t)(g_io + reg));
}

static inline uint32_t r32(uint32_t reg)
{
    return inl((uint16_t)(g_io + reg));
}

static inline void w8(uint32_t reg, uint8_t v)
{
    outb((uint16_t)(g_io + reg), v);
}

static inline void w16(uint32_t reg, uint16_t v)
{
    outw((uint16_t)(g_io + reg), v);
}

static inline void w32(uint32_t reg, uint32_t v)
{
    outl((uint16_t)(g_io + reg), v);
}

/* Force a previous I/O write to complete by reading back the command
 * register.  The RTL8139 has posted writes; ordering matters when the next
 * access is a DMA kick. */
static inline void write_flush(void)
{
    (void)r8(RTL8139_REG_CR);
}

/* ---- probing ---------------------------------------------------------- */
static const rtl8139_id_t *rtl8139_match(uint16_t vendor, uint16_t device)
{
    for (unsigned i = 0; i < sizeof(rtl8139_ids) / sizeof(rtl8139_ids[0]); i++)
        if (rtl8139_ids[i].vendor == vendor && rtl8139_ids[i].device == device)
            return &rtl8139_ids[i];
    return 0;
}

static int rtl8139_valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    uint8_t all = 0xFF;

    for (int i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    /* Reject the all-zero and all-ones addresses, and multicast/group bits. */
    return any != 0 && all != 0xFF && !(mac[0] & 1);
}

static void rtl8139_reset(void)
{
    w8(RTL8139_REG_CR, RTL8139_CR_RESET);
    write_flush();

    /* The datasheet says the bit clears when the reset finishes.  QEMU
     * clears it; some clone implementations do not, so bound the wait and
     * carry on rather than turning a cosmetic difference into a dead NIC. */
    for (int i = 0; i < RTL8139_RESET_POLL; i++)
        if (!(r8(RTL8139_REG_CR) & RTL8139_CR_RESET))
            return;
    dbg_puts("RTL8139: reset bit did not clear, continuing\r\n");
}

static void rtl8139_read_mac(void)
{
    for (int i = 0; i < 6; i++)
        g_mac[i] = r8(RTL8139_REG_IDR0 + (uint32_t)i);

    if (!rtl8139_valid_mac(g_mac)) {
        dbg_puts("RTL8139: invalid MAC, using a locally administered one\r\n");
        g_mac[0] = 0x02;
        g_mac[1] = 0x00;
        g_mac[2] = 0x00;
        g_mac[3] = 0x00;
        g_mac[4] = 0x00;
        g_mac[5] = 0x01;
    }
}

/* ---- DMA memory ------------------------------------------------------- */
static void rtl8139_free_dma(void)
{
    for (int i = 0; i < RTL8139_TX_COUNT; i++) {
        if (g_tx_phys[i])
            pmm_free(g_tx_phys[i]);
        g_tx_phys[i] = 0;
        g_tx[i]      = 0;
    }
    if (g_rx_phys) {
        for (uint32_t i = 0; i < RTL8139_RX_RING_PAGES; i++)
            pmm_free(g_rx_phys + (uint64_t)i * PAGE_SIZE);
        g_rx_phys = 0;
        g_rx      = 0;
    }
}

static int rtl8139_alloc_dma(void)
{
    g_rx_phys = pmm_alloc_contiguous(RTL8139_RX_RING_PAGES);
    if (!g_rx_phys) {
        dbg_puts("RTL8139: out of memory for the receive ring\r\n");
        return 0;
    }
    g_rx = (volatile uint8_t *)pmm_virt(g_rx_phys);
    memset((void *)g_rx, 0, (size_t)RTL8139_RX_RING_PAGES * PAGE_SIZE);

    for (int i = 0; i < RTL8139_TX_COUNT; i++) {
        g_tx_phys[i] = pmm_alloc_zeroed();
        if (!g_tx_phys[i]) {
            dbg_puts("RTL8139: out of memory for a transmit buffer\r\n");
            rtl8139_free_dma();
            return 0;
        }
        g_tx[i] = (uint8_t *)pmm_virt(g_tx_phys[i]);
    }
    __sync_synchronize();
    return 1;
}

/* ---- hardware programming --------------------------------------------- */
static void rtl8139_program_hw(void)
{
    w16(RTL8139_REG_IMR, 0);
    w16(RTL8139_REG_ISR, 0xFFFF); /* W1C: clear anything left from reset */
    write_flush();

    /* Unlock the config registers so IDR and RBSTART can be written. */
    w8(RTL8139_REG_9346CR, RTL8139_9346_UNLOCK);

    /* IDR0-5 are byte registers, but the datasheet's 32-bit write sequence
     * is what the chip expects (and what Linux uses). */
    w32(RTL8139_REG_IDR0, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8) |
                              ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    w32(RTL8139_REG_IDR0 + 4, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8));

    /* Receive ring base, dword aligned by the page allocator. */
    w32(RTL8139_REG_RBSTART, (uint32_t)g_rx_phys);

    w8(RTL8139_REG_9346CR, RTL8139_9346_LOCK);

    g_cur_rx = 0;
    w16(RTL8139_REG_CAPR, (uint16_t)(g_cur_rx - 16));

    /* The transmitter must be enabled before TCR is written (the datasheet
     * calls this out and QEMU enforces it). */
    w8(RTL8139_REG_CR, RTL8139_CR_TE | RTL8139_CR_RE);

    /* Standard interframe gap, 1024-byte DMA bursts. */
    w32(RTL8139_REG_TCR, RTL8139_TCR_IFG_NORMAL | ((uint32_t)RTL8139_TCR_MXDMA << 8));

    /* Physical/multicast/broadcast, 256-byte FIFO threshold, 32 KiB ring. */
    w32(RTL8139_REG_RCR, ((uint32_t)RTL8139_RCR_RXFTH << 13) |
                             ((uint32_t)RTL8139_RX_BUF_IDX << 11) |
                             ((uint32_t)RTL8139_RCR_MXDMA << 8) | RTL8139_RCR_APM |
                             RTL8139_RCR_AM | RTL8139_RCR_AB);

    w8(RTL8139_REG_CR, RTL8139_CR_TE | RTL8139_CR_RE);
    write_flush();
}

/* ---- link ------------------------------------------------------------- */
static int rtl8139_link_state(void)
{
    (void)r16(RTL8139_REG_BMSR); /* the datasheet asks for two reads */
    return !!(r16(RTL8139_REG_BMSR) & RTL8139_BMSR_LINK);
}

int rtl8139_link_up(void)
{
    return g_ok && rtl8139_link_state();
}

/* ---- receive ---------------------------------------------------------- */
/*
 * After an overflow the ring contents are no longer trustworthy: the card
 * wrapped and overwrote the frame we were about to read.  The only safe
 * recovery is to jump to the card's own write pointer (CBR) and tell it we
 * have consumed everything up to there.
 */
static void rtl8139_rx_resync(void)
{
    g_cur_rx = (uint32_t)r16(RTL8139_REG_CBR) % RTL8139_RX_BUF_SIZE;
    w16(RTL8139_REG_CAPR, (uint16_t)(g_cur_rx - 16));
}

uint16_t rtl8139_recv(void *buf, uint16_t max)
{
    if (!g_ok || !buf)
        return 0;

    /* The IRQ handler usually clears these, but polling is the fallback when
     * no interrupt line is available.  An overflow that nobody reports would
     * otherwise leave the ring permanently misaligned. */
    uint16_t isr = r16(RTL8139_REG_ISR);
    if (isr & (RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)) {
        w16(RTL8139_REG_ISR, (uint16_t)(isr & (RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)));
        rtl8139_rx_resync();
    }

    /* A burst of malformed headers must not hold the caller here forever;
     * after eight of them, let the next poll try again. */
    for (int guard = 0; guard < 8; guard++) {
        if (r8(RTL8139_REG_CR) & RTL8139_CR_BUFE)
            return 0;

        uint32_t off = g_cur_rx % RTL8139_RX_BUF_SIZE;
        uint32_t hdr = *(volatile uint32_t *)(g_rx + off);
        __sync_synchronize(); /* header before payload */

        uint16_t status = (uint16_t)(hdr & 0xFFFF);
        uint32_t length = hdr >> 16; /* includes the 4-byte FCS */

        int good = (status & RTL8139_RX_ROK) && !(status & RTL8139_RX_ERRORS) &&
                   length > RTL8139_CRC_LEN &&
                   length <= RTL8139_MAX_FRAME + RTL8139_CRC_LEN;

        if (!good) {
            if (length <= RTL8139_CRC_LEN || length > RTL8139_RX_BUF_SIZE)
                rtl8139_rx_resync(); /* unusable length: trust CBR instead */
            else {
                g_cur_rx = (g_cur_rx + length + RTL8139_CRC_LEN + 3) & ~3u;
                w16(RTL8139_REG_CAPR, (uint16_t)(g_cur_rx - 16));
            }
            continue;
        }

        uint32_t n    = length - RTL8139_CRC_LEN;
        if (n > max)
            n = max;
        uint32_t data = (off + RTL8139_CRC_LEN) % RTL8139_RX_BUF_SIZE;

        if (data + n <= RTL8139_RX_BUF_SIZE) {
            memcpy(buf, (const void *)(g_rx + data), n);
        } else {
            /* The frame straddles the end of the ring. */
            uint32_t first = RTL8139_RX_BUF_SIZE - data;
            memcpy(buf, (const void *)(g_rx + data), first);
            memcpy((uint8_t *)buf + first, (const void *)g_rx, n - first);
        }

        g_cur_rx = (g_cur_rx + length + RTL8139_CRC_LEN + 3) & ~3u;
        w16(RTL8139_REG_CAPR, (uint16_t)(g_cur_rx - 16));
        g_rx_frames++;
        return (uint16_t)n;
    }
    return 0;
}

/* ---- transmit --------------------------------------------------------- */
int rtl8139_send(const void *frame, uint16_t len)
{
    if (!g_ok || !frame || len == 0 || len > RTL8139_TX_MAX)
        return 0;

    unsigned i    = g_tx_next;
    uint32_t tsd  = r32(RTL8139_REG_TSD0 + i * 4);

    /* OWN == 0 means the card still owns the descriptor.  A synchronous
     * sender should almost never see this, but a previous timeout can leave
     * one busy, so wait for it rather than scribbling over a live DMA. */
    for (int n = 0; n < RTL8139_TX_POLL && !(tsd & RTL8139_TX_OWN); n++) {
        io_delay();
        tsd = r32(RTL8139_REG_TSD0 + i * 4);
    }
    if (!(tsd & RTL8139_TX_OWN))
        return 0;

    memcpy(g_tx[i], frame, len);

    /* The chip does not pad short frames for us; an ARP request is 42 bytes
     * and must go out as 60. */
    if (len < RTL8139_ETH_ZLEN) {
        memset(g_tx[i] + len, 0, RTL8139_ETH_ZLEN - len);
        len = RTL8139_ETH_ZLEN;
    }

    __sync_synchronize(); /* buffer contents before the descriptor kick */

    w32(RTL8139_REG_TSAD0 + i * 4, (uint32_t)g_tx_phys[i]);
    w32(RTL8139_REG_TSD0 + i * 4,
        ((uint32_t)RTL8139_TX_ERTXTH << 16) | (len & RTL8139_TX_LEN_MASK));

    /* Wait for the card to finish.  OWN is set when the DMA is complete;
     * TOK says it completed successfully.  This is the same "wait for the
     * descriptor to come back" contract as e1000_send(). */
    for (int n = 0; n < RTL8139_TX_POLL; n++) {
        tsd = r32(RTL8139_REG_TSD0 + i * 4);
        if (tsd & RTL8139_TX_OWN) {
            g_tx_next = (i + 1) % RTL8139_TX_COUNT;
            if (tsd & RTL8139_TX_TOK) {
                g_tx_frames++;
                return 1;
            }
            return 0;
        }
        io_delay();
    }
    return 0;
}

/* ---- interrupts ------------------------------------------------------- */
/*
 * ISR is write-one-to-clear.  Reading it without writing the bits back is
 * the second classic way to make this chip look dead: the line stays
 * asserted and the machine spends all its time in the stub.
 */
static void rtl8139_irq(regs_t *r)
{
    (void)r;

    uint16_t st = r16(RTL8139_REG_ISR);
    if (!st)
        return; /* not us: PCI interrupt lines are shared */

    w16(RTL8139_REG_ISR, st);
    g_irqs++;

    if (st & (RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW))
        rtl8139_rx_resync();

    if (st & RTL8139_RX_INTS) {
        net_poll();
        sched_wake_reason(WAIT_NET);
    }

    if (st & RTL8139_ISR_PUN) {
        int up = rtl8139_link_state();
        dbg_puts("RTL8139: link ");
        dbg_puts(up ? "up\r\n" : "down\r\n");
    }
}

/* ---- public counters / identity --------------------------------------- */
int rtl8139_present(void)
{
    return g_ok;
}

const uint8_t *rtl8139_mac(void)
{
    return g_mac;
}

void rtl8139_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs)
{
    if (tx_frames)
        *tx_frames = g_tx_frames;
    if (rx_frames)
        *rx_frames = g_rx_frames;
    if (irqs)
        *irqs = g_irqs;
}

/* ---- bring-up --------------------------------------------------------- */
int rtl8139_init(void)
{
    const pci_dev_t *d = 0;

    for (int i = 0; i < g_pci_count; i++) {
        if (rtl8139_match(g_pci_devs[i].vendor, g_pci_devs[i].device)) {
            d = &g_pci_devs[i];
            break;
        }
    }
    if (!d) {
        dbg_puts("RTL8139: no 10EC:8139 on the bus, skipping\r\n");
        return 0;
    }

    pci_enable(d); /* I/O space + memory space + BUS MASTER */
    uint16_t io = pci_bar_io(d, 0);
    if (!io) {
        dbg_puts("RTL8139: BAR0 is not an I/O port region\r\n");
        return 0;
    }

    g_io  = io;
    g_irq = d->irq_line;

    rtl8139_reset();
    rtl8139_read_mac();

    if (!rtl8139_alloc_dma())
        return 0;

    rtl8139_program_hw();

    g_ok = 1;

    /* Only now that a ring exists is it safe to let the card interrupt us. */
    if (g_irq < 16) {
        irq_install(g_irq, rtl8139_irq, "rtl8139");
        w16(RTL8139_REG_IMR, RTL8139_INT_MASK);
        write_flush();
    }

    (void)nic_register(&rtl8139_nic);

    dbg_puts("RTL8139: 10EC:8139 up, mac ");
    for (int i = 0; i < 6; i++) {
        dbg_puts_hexn(g_mac[i], 2);
        if (i != 5)
            dbg_puts(":");
    }
    dbg_puts(", irq ");
    dbg_puts_dec(g_irq);
    dbg_puts(", link ");
    dbg_puts(rtl8139_link_state() ? "up" : "down");
    dbg_puts("\r\n");
    return 1;
}

/* ---- self-test -------------------------------------------------------- */
/*
 * Nothing here is a protocol stack: the frame is 42 bytes written by hand.
 * Broadcast "who has 10.0.2.2", and QEMU's user-mode network stack -- which
 * owns that address -- answers with its MAC.  A pass proves the transmit
 * descriptor kicked, the wire is connected, and the receive ring's header
 * arithmetic is right; a failure is a driver bug, not a stack bug.
 */
#define ETHERTYPE_ARP 0x0806

static const uint8_t IP_SELF[4] = {10, 0, 2, 15}; /* QEMU hands us .15 */
static const uint8_t IP_GW[4]   = {10, 0, 2, 2};  /* the gateway/host */

static uint16_t recv_wait(void *buf, uint16_t max, int tries)
{
    for (int n = 0; n < tries; n++) {
        uint16_t len = rtl8139_recv(buf, max);
        if (len)
            return len;
        io_delay();
    }
    return 0;
}

static void rtl8139_dump_state(void)
{
    dbg_puts("       CR=0x");
    dbg_puts_hexn(r8(RTL8139_REG_CR), 2);
    dbg_puts(" ISR=0x");
    dbg_puts_hexn(r16(RTL8139_REG_ISR), 4);
    dbg_puts(" IMR=0x");
    dbg_puts_hexn(r16(RTL8139_REG_IMR), 4);
    dbg_puts(" RCR=0x");
    dbg_puts_hexn(r32(RTL8139_REG_RCR), 8);
    dbg_puts(" TCR=0x");
    dbg_puts_hexn(r32(RTL8139_REG_TCR), 8);
    dbg_puts(" CAPR=0x");
    dbg_puts_hexn(r16(RTL8139_REG_CAPR), 4);
    dbg_puts(" CBR=0x");
    dbg_puts_hexn(r16(RTL8139_REG_CBR), 4);
    dbg_puts(" BMSR=0x");
    dbg_puts_hexn(r16(RTL8139_REG_BMSR), 4);
    dbg_puts("\r\n");
}

static int test_arp(void)
{
    static uint8_t got[RTL8139_TX_MAX];
    uint8_t        f[42];

    if (!rtl8139_link_state()) {
        dbg_puts("RTL8139: ARP skipped (no link)\r\n");
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

    if (!rtl8139_send(f, sizeof f)) {
        dbg_puts("RTL8139: ARP FAIL (request never left the card)\r\n");
        rtl8139_dump_state();
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

        dbg_puts("RTL8139: ARP PASS, 10.0.2.2 is at ");
        for (int i = 0; i < 6; i++) {
            dbg_puts_hexn(got[22 + i], 2);
            if (i != 5)
                dbg_puts(":");
        }
        dbg_puts("\r\n");
        return 1;
    }

    dbg_puts("RTL8139: ARP FAIL (no reply from 10.0.2.2)\r\n");
    rtl8139_dump_state();
    return 0;
}

int rtl8139_selftest(void)
{
    if (!g_ok)
        return 0;

    /* Give the link a moment to settle before the first frame. */
    timer_delay_ms(100);

    int ok = test_arp();

    dbg_puts("RTL8139: tx=");
    dbg_puts_dec((uint32_t)g_tx_frames);
    dbg_puts(" rx=");
    dbg_puts_dec((uint32_t)g_rx_frames);
    dbg_puts(" frames\r\n");
    return ok;
}
