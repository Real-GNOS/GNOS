#include "drivers/net/rtl8139.h"

#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"


#define REG_CMD 0x37
#define REG_CAPR 0x38
#define REG_RBSTART 0x30
#define REG_IMR 0x3C
#define REG_RCR 0x44
#define REG_TSD0 0x10
#define REG_TSAD0 0x20

#define RTL8139_TX_WAIT_SPINS 100000u

#define V2P(x)                                                                 \
    ((uint32_t)(x) >= 0xC0000000u                                              \
         ? ((uint32_t)(x) - 0xC0000000u)                                       \
         : ((*pte_ptr((uint32_t)(x)) & 0xfffff000) | ((uint32_t)(x) & 0xfff)))

static uint16_t ioaddr;
static uint8_t *rx_buffer;
static uint32_t rx_cur;
static uint8_t tx_slot;

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t reg) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t dev, uint8_t reg, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static int rtl8139_find_dev(uint8_t *bus, uint8_t *dev) {
    for (uint16_t d = 0; d < 32; d++) {
        uint32_t id = pci_read32(0, (uint8_t)d, 0);
        if ((id & 0xFFFF) == RTL8139_VENDOR &&
            ((id >> 16) & 0xFFFF) == RTL8139_DEVICE) {
            *bus = 0;
            *dev = (uint8_t)d;
            return 1;
        }
    }
    return 0;
}

int rtl8139_init(NETIF *ifp) {
    uint8_t bus, dev;
    if (!rtl8139_find_dev(&bus, &dev))
        return -1;
    uint32_t bar = pci_read32(bus, dev, 0x10);
    ioaddr = (uint16_t)(bar & 0xFFFFFFFC);

    uint32_t cmd = pci_read32(bus, dev, 0x04);
    pci_write32(bus, dev, 0x04, cmd | 0x7);

    outb(ioaddr + 0x52, 0x00);
    outb(ioaddr + REG_CMD, 0x10);
    while (inb(ioaddr + REG_CMD) & 0x10) {
    }

    for (int i = 0; i < 6; i++)
        ifp->mac[i] = inb(ioaddr + i);

    rx_buffer = (uint8_t *)get_kernel_pages(3);
    outl(ioaddr + REG_RBSTART, V2P(rx_buffer));

    outw(ioaddr + REG_IMR, 0x0000);
    outl(ioaddr + REG_RCR, 0xF | (1 << 7));
    outb(ioaddr + REG_CMD, 0x0C);
    rx_cur = 0;
    tx_slot = 0;
    kprintf("[NET] rtl8139 up\n");
    return 0;
}

static int rtl8139_tx_wait(uint32_t slot) {
    for (uint32_t i = 0; i < RTL8139_TX_WAIT_SPINS; i++) {
        if (inl(ioaddr + REG_TSD0 + slot * 4) & (1u << 13))
            return 1;
        asm_pause();
    }
    return 0;
}

int rtl8139_tx(NETIF *ifp, const void *frame, uint32_t len) {
    (void)ifp;
    outl(ioaddr + REG_TSAD0 + tx_slot * 4, V2P(frame));
    outl(ioaddr + REG_TSD0 + tx_slot * 4, len);

    int done = rtl8139_tx_wait(tx_slot);
    tx_slot = (tx_slot + 1) & 3;
    return done ? (int)len : -1;
}

int rtl8139_rx(NETIF *ifp, void *buf, uint32_t maxlen) {
    (void)ifp;
    uint16_t st = *(uint16_t *)(rx_buffer + rx_cur);
    if (!(st & 0x0001))
        return 0;
    uint16_t pkt_len = *(uint16_t *)(rx_buffer + rx_cur + 2);
    if (pkt_len > maxlen)
        pkt_len = (uint16_t)maxlen;
    memcpy(buf, rx_buffer + rx_cur + 4, pkt_len);
    rx_cur = (rx_cur + 4 + pkt_len + 3) & ~3u;
    if (rx_cur >= 8192)
        rx_cur = 0;
    if (rx_cur >= 16)
        outw(ioaddr + REG_CAPR, rx_cur - 16);
    else
        outw(ioaddr + REG_CAPR, rx_cur + 8192 - 16);
    return pkt_len;
}
