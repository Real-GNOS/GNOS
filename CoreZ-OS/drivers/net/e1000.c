#include "drivers/net/e1000.h"

#include "kernel/asm_func.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/net/netif.h"
#include "drivers/net/pci.h"
#include "drivers/char/console/io.h"

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E
#define E1000_BAR0 0x10

#define REG_CTRL 0x0000
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RCTL 0x0100
#define REG_IMC 0x00D8
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_RA 0x5400

#define TX_DESC_N 8
#define RX_DESC_N 16
#define RX_BUF_LEN 2048
#define E1000_TX_WAIT_SPINS 100000u

#define V2P(x)                                                                 \
    ((uint32_t)(x) >= 0xC0000000u                                              \
         ? ((uint32_t)(x) - 0xC0000000u)                                       \
         : ((*pte_ptr((uint32_t)(x)) & 0xfffff000) | ((uint32_t)(x) & 0xfff)))

struct E1000_TX_DESC {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

struct E1000_RX_DESC {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *s_regs;
static struct E1000_TX_DESC *s_tx;
static struct E1000_RX_DESC *s_rx;
static uint8_t *s_tx_buf;
static uint8_t *s_rx_buf;
static uint32_t s_tx_cur;
static uint32_t s_rx_cur;

static inline uint32_t e1000_reg_read(uint32_t off) {
    return *(volatile uint32_t *)(s_regs + off);
}

static inline void e1000_reg_write(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(s_regs + off) = v;
}

static void e1000_reset(void) {
    e1000_reg_write(REG_CTRL, e1000_reg_read(REG_CTRL) | (1 << 26));
    for (uint32_t i = 0; i < 10000; i++) {
        if (!(e1000_reg_read(REG_CTRL) & (1 << 26)))
            break;
    }
}

int e1000_init(NETIF *ifp) {
    uint8_t bus, dev;
    if (!pci_find_device(E1000_VENDOR, E1000_DEVICE, &bus, &dev))
        return -1;
    uint32_t bar = pci_read32(bus, dev, E1000_BAR0);
    uint32_t phy = bar & ~0xFu;
    pci_enable_bus_master(bus, dev, 0x6);

    s_regs = (volatile uint8_t *)ioremap(phy, 0x20000);
    if (!s_regs)
        return -1;
    kprintf("[e1000] resetting\n");
    e1000_reset();
    kprintf("[e1000] reset done\n");

    uint32_t ral = e1000_reg_read(REG_RA);
    uint32_t rah = e1000_reg_read(REG_RA + 4);
    ifp->mac[0] = (uint8_t)(ral & 0xFF);
    ifp->mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    ifp->mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    ifp->mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    ifp->mac[4] = (uint8_t)(rah & 0xFF);
    ifp->mac[5] = (uint8_t)((rah >> 8) & 0xFF);

    e1000_reg_write(REG_IMC, 0xFFFFFFFF);

    s_tx = (struct E1000_TX_DESC *)get_kernel_pages(1);
    s_tx_buf = (uint8_t *)get_kernel_pages(TX_DESC_N * RX_BUF_LEN / PAGE_SIZE);
    for (uint32_t i = 0; i < TX_DESC_N; i++) {
        s_tx[i].addr = V2P(s_tx_buf) + (uint64_t)i * RX_BUF_LEN;
        s_tx[i].cmd = 0;
        s_tx[i].status = 0x01;
    }
    e1000_reg_write(REG_TDBAL, V2P(s_tx));
    e1000_reg_write(REG_TDBAH, 0);
    e1000_reg_write(REG_TDLEN, TX_DESC_N * 16);
    e1000_reg_write(REG_TDH, 0);
    e1000_reg_write(REG_TDT, 0);
    e1000_reg_write(REG_TCTL, 0x10000 | 0x400 | 0x8 | 0x2);
    e1000_reg_write(REG_TIPG, 0x18);

    s_rx = (struct E1000_RX_DESC *)get_kernel_pages(1);
    s_rx_buf = (uint8_t *)get_kernel_pages(RX_DESC_N * RX_BUF_LEN / PAGE_SIZE);
    for (uint32_t i = 0; i < RX_DESC_N; i++) {
        s_rx[i].addr = V2P(s_rx_buf) + (uint64_t)i * RX_BUF_LEN;
        s_rx[i].status = 0;
    }
    e1000_reg_write(REG_RDBAL, V2P(s_rx));
    e1000_reg_write(REG_RDBAH, 0);
    e1000_reg_write(REG_RDLEN, RX_DESC_N * 16);
    e1000_reg_write(REG_RDH, 0);
    e1000_reg_write(REG_RDT, RX_DESC_N - 1);
    e1000_reg_write(REG_RCTL, 0x2 | 0x8000 | 0x04000000);

    s_tx_cur = s_rx_cur = 0;
    return 0;
}

static int e1000_tx_wait(volatile uint8_t *status) {
    for (uint32_t i = 0; i < E1000_TX_WAIT_SPINS; i++) {
        if (*status & 0x01)
            return 1;
        asm_pause();
    }
    return 0;
}

int e1000_tx(NETIF *ifp, const void *frame, uint32_t len) {
    (void)ifp;
    uint32_t slot = s_tx_cur % TX_DESC_N;
    if (!e1000_tx_wait(&s_tx[slot].status))
        return -1;
    memcpy(s_tx_buf + (size_t)slot * RX_BUF_LEN, frame, len);
    s_tx[slot].length = (uint16_t)len;
    s_tx[slot].cso = 0;
    s_tx[slot].css = 0;
    s_tx[slot].cmd = 0x01 | 0x02 | 0x08;
    s_tx[slot].status = 0;
    s_tx_cur++;
    e1000_reg_write(REG_TDT, s_tx_cur % TX_DESC_N);
    if (!e1000_tx_wait(&s_tx[slot].status))
        return -1;
    return (int)len;
}

int e1000_rx(NETIF *ifp, void *buf, uint32_t maxlen) {
    (void)ifp;
    if (!(s_rx[s_rx_cur].status & 0x01))
        return 0;
    uint32_t got = 0;
    if (s_rx[s_rx_cur].errors == 0) {
        uint16_t plen = s_rx[s_rx_cur].length;
        if (plen > maxlen)
            plen = (uint16_t)maxlen;
        memcpy(buf, s_rx_buf + (size_t)s_rx_cur * RX_BUF_LEN, plen);
        got = plen;
    }
    uint32_t done = s_rx_cur;
    s_rx[s_rx_cur].status = 0;
    s_rx_cur = (s_rx_cur + 1) % RX_DESC_N;
    e1000_reg_write(REG_RDT, done);
    return (int)got;
}
