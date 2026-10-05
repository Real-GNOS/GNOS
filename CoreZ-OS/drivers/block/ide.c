#include "drivers/block/ide.h"

#include "drivers/block/block.h"
#include "drivers/driver_ops.h"
#include "drivers/pci/pci.h"

#include <stdint.h>

#include "kernel/asm_func.h"
#include "kernel/assert.h"
#include "drivers/char/console/io.h"
#include "arch/cpu.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"
#include "kernel/mm/pool/pool.h"

#define reg_data(channel) ((channel)->port_base + 0)
#define reg_sect_cnt(channel) ((channel)->port_base + 2)
#define reg_lba_l(channel) ((channel)->port_base + 3)
#define reg_lba_m(channel) ((channel)->port_base + 4)
#define reg_lba_h(channel) ((channel)->port_base + 5)
#define reg_dev(channel) ((channel)->port_base + 6)
#define reg_status(channel) ((channel)->port_base + 7)
#define reg_cmd(channel) (reg_status(channel))

#define BIT_ALT_STAT_BSY 0x80
#define BIT_ALT_STAT_DRQ 0x8

#define BIT_DEV_MBS 0xa0
#define BIT_DEV_LBA 0x40
#define BIT_DEV_DEV 0x10

#define CMD_IDENTIFY 0xec
#define CMD_READ_SECTOR 0x20
#define CMD_WRITE_SECTOR 0x30
#define CMD_READ_DMA 0xc8
#define CMD_WRITE_DMA 0xca

#define IDE_BM_CMD 0x00u
#define IDE_BM_STATUS 0x02u
#define IDE_BM_PRDT 0x04u
#define IDE_BM_START 0x01u
#define IDE_BM_READ (1u << 3)
#define IDE_BM_ACTIVE 0x01u
#define IDE_BM_DMA_ERR 0x02u
#define IDE_BM_RW_CLEAR 0x06u

#define IDE_DMA_SECTORS 128u
#define IDE_DMA_PAGES (IDE_DMA_SECTORS * 512u / 4096u)

#define MAX_LBA_DEFAULT ((80 * 1024 * 1024 / 512) - 1)
#define MAX_LBA28 (0x0FFFFFFF)

uint8_t channel_cnt;
struct IDE_CHANNEL channels[2];

static uint16_t s_bm_io[2];
static uint8_t *s_dma_buf;
static uint32_t s_dma_buf_phys;
static uint32_t *s_prdt;
static uint32_t s_prdt_phys;
static int s_dma_ready;

static void ide_panic(const char *msg) {
    set_text_color(12);
    kprintf("IDE PANIC: %s\n", msg);
    asm_cli();
    for (;;) {
        asm_hlt();
    }
}

static void select_disk(struct DISK *hd) {
    uint8_t reg_device = BIT_DEV_MBS | BIT_DEV_LBA;
    if (hd->dev_no == 1) {
        reg_device |= BIT_DEV_DEV;
    }
    outb(reg_dev(hd->my_channel), reg_device);
}

static void select_sector(struct DISK *hd, uint32_t lba, uint8_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    struct IDE_CHANNEL *channel = hd->my_channel;
    outb(reg_sect_cnt(channel), sec_cnt);
    outb(reg_lba_l(channel), lba);
    outb(reg_lba_m(channel), lba >> 8);
    outb(reg_lba_h(channel), lba >> 16);

    outb(reg_dev(channel), BIT_DEV_MBS | BIT_DEV_LBA |
                               (hd->dev_no == 1 ? BIT_DEV_DEV : 0) |
                               (uint8_t)(lba >> 24));
}

static void wait_bsy_clear(struct IDE_CHANNEL *channel) {
    while (inb(reg_status(channel)) & BIT_ALT_STAT_BSY) {
    }
}

static void channel_send_cmd(struct IDE_CHANNEL *channel, uint8_t cmd) {
    wait_bsy_clear(channel);
    channel->expecting_intr = 1;
    outb(reg_cmd(channel), cmd);
}

static void ide_dma_init(void) {
    uint32_t bdf = PCI_BDF(0, 1, 1);
    uint32_t vendor = pci_config_read32(bdf, PCI_CFG_VENDOR);
    uint32_t bar4;
    uint16_t base;

    if ((vendor & 0xFFFFu) == 0xFFFFu) {
        return;
    }
    uint32_t class = pci_config_read32(bdf, PCI_CFG_CLASS);
    if (((class >> 24) & 0xFFu) != PCI_CLASS_MASS_STORAGE) {
        return;
    }
    bar4 = pci_config_read32(bdf, 0x20u);
    if (!(bar4 & 1u) || (bar4 & ~3u) == 0) {
        return;
    }
    base = (uint16_t)(bar4 & ~3u);
    pci_cmd_set(bdf, PCI_CMD_IO | PCI_CMD_BUS_MASTER);
    for (uint32_t c = 0; c < 2; c++) {
        s_bm_io[c] = base + 8u * c;
    }

    s_dma_buf = get_kernel_pages(IDE_DMA_PAGES);
    s_prdt = get_kernel_pages(1);
    if (s_dma_buf == NULL || s_prdt == NULL) {
        return;
    }
    s_dma_buf_phys = PHY_OF((uint32_t)s_dma_buf);
    s_prdt_phys = PHY_OF((uint32_t)s_prdt);
    s_dma_ready = 1;
    kprintf("  ide dma: bmdma=0x%x buf=0x%x(%uKB)\n", (unsigned)base,
            (unsigned)s_dma_buf_phys, (unsigned)(IDE_DMA_SECTORS / 2u));
}

/**
 * 用总线主控 DMA 搬运一段连续扇区。
 *
 * @param hd       目标盘
 * @param lba      起始扇区
 * @param buf      数据缓冲，物理连续性由 bounce 缓冲保证
 * @param sec_cnt  扇区数，不得超过 IDE_DMA_SECTORS
 * @param is_write 1 为写
 * @returns 成功返回 0；DMA 引擎报错或超时返回 -1
*/
static int ide_dma_transfer(struct DISK *hd, uint32_t lba, void *buf,
                            uint32_t sec_cnt, int is_write) {
    struct IDE_CHANNEL *channel = hd->my_channel;
    uint16_t bm = s_bm_io[(uint32_t)(channel - channels)];
    uint32_t bytes = sec_cnt * 512u;
    uint32_t count_field = (bytes & 0xFFFFu) ? bytes : 0u;
    uint32_t spins = 0;
    uint8_t st;
    uint8_t ata_st;

    if (is_write) {
        memcpy(s_dma_buf, buf, bytes);
    }
    s_prdt[0] = s_dma_buf_phys;
    s_prdt[1] = count_field | (1u << 31);

    outb(bm + IDE_BM_CMD, 0);
    outb(bm + IDE_BM_STATUS, IDE_BM_RW_CLEAR);
    outl(bm + IDE_BM_PRDT, s_prdt_phys);
    select_disk(hd);
    select_sector(hd, lba, (uint8_t)sec_cnt);
    channel_send_cmd(channel, is_write ? CMD_WRITE_DMA : CMD_READ_DMA);
    outb(bm + IDE_BM_CMD,
         (is_write ? 0u : IDE_BM_READ) | IDE_BM_START);

    for (;;) {
        st = inb(bm + IDE_BM_STATUS);
        if (!(st & IDE_BM_ACTIVE)) {
            break;
        }
        asm_pause();
        if (++spins > 100000000u) {
            break;
        }
    }
    outb(bm + IDE_BM_CMD, 0);
    ata_st = inb(reg_status(channel));
    if ((st & IDE_BM_DMA_ERR) || (ata_st & 1u) || (st & IDE_BM_ACTIVE)) {
        return -1;
    }
    if (!is_write) {
        memcpy(buf, s_dma_buf, bytes);
    }
    return 0;
}

static void read_from_sector(struct DISK *hd, void *buf, uint8_t sec_cnt) {
    uint32_t dwords = sec_cnt ? (uint32_t)sec_cnt << 7 : 256u << 7;
    cpu_ins(reg_data(hd->my_channel), buf, (int)dwords, 4);
}

static void write_to_sector(struct DISK *hd, void *buf, uint8_t sec_cnt) {
    uint32_t words = sec_cnt ? (uint32_t)sec_cnt << 8 : 256 << 8;
    outsw(reg_data(hd->my_channel), buf, words);
}

static int busy_wait(struct DISK *hd) {
    struct IDE_CHANNEL *channel = hd->my_channel;
    uint32_t timeout = 30 * 1000 * 100;
    while (timeout--) {
        uint8_t st = inb(reg_status(channel));
        if (!(st & BIT_ALT_STAT_BSY)) {
            return (st & BIT_ALT_STAT_DRQ) != 0;
        }
    }
    return 0;
}

void ide_read(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);

    if (s_dma_ready) {
        uint32_t done = 0;
        while (done < sec_cnt) {
            uint32_t chunk = sec_cnt - done;
            if (chunk > IDE_DMA_SECTORS) {
                chunk = IDE_DMA_SECTORS;
            }
            if (ide_dma_transfer(hd, lba + done, (char *)buf + done * 512,
                                 chunk, 0) != 0) {
                ide_panic("ide dma read failed");
            }
            done += chunk;
        }
        lock_release(&hd->my_channel->lock);
        return;
    }

    select_disk(hd);

    uint32_t secs_done = 0;
    while (secs_done < sec_cnt) {
        uint32_t chunk = sec_cnt - secs_done;
        if (chunk > 255) {
            chunk = 255;
        }
        select_sector(hd, lba + secs_done, (uint8_t)chunk);
        channel_send_cmd(hd->my_channel, CMD_READ_SECTOR);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!busy_wait(hd)) {
                char error[64];
                sprintf(error, "%s read sector %d failed!!!!!!", hd->name,
                        (int)(lba + secs_done + i));
                ide_panic(error);
            }
            read_from_sector(hd, (char *)buf + (secs_done + i) * 512, 1);
        }
        secs_done += chunk;
    }
    lock_release(&hd->my_channel->lock);
}

void ide_write(struct DISK *hd, uint32_t lba, const void *buf,
               uint32_t sec_cnt) {
    ASSERT(lba <= hd->max_lba);
    ASSERT(sec_cnt > 0);
    lock_acquire(&hd->my_channel->lock);

    if (s_dma_ready) {
        uint32_t done = 0;
        while (done < sec_cnt) {
            uint32_t chunk = sec_cnt - done;
            if (chunk > IDE_DMA_SECTORS) {
                chunk = IDE_DMA_SECTORS;
            }
            if (ide_dma_transfer(hd, lba + done, (char *)buf + done * 512,
                                 chunk, 1) != 0) {
                ide_panic("ide dma write failed");
            }
            done += chunk;
        }
        lock_release(&hd->my_channel->lock);
        return;
    }

    select_disk(hd);

    uint32_t secs_done = 0;
    while (secs_done < sec_cnt) {
        uint32_t chunk = sec_cnt - secs_done;
        if (chunk > 255) {
            chunk = 255;
        }
        select_sector(hd, lba + secs_done, (uint8_t)chunk);
        channel_send_cmd(hd->my_channel, CMD_WRITE_SECTOR);
        for (uint32_t i = 0; i < chunk; i++) {
            if (!busy_wait(hd)) {
                char error[64];
                sprintf(error, "%s write sector %d failed!!!!!!", hd->name,
                        (int)(lba + secs_done + i));
                ide_panic(error);
            }
            write_to_sector(hd, (char *)buf + (secs_done + i) * 512, 1);
        }
        wait_bsy_clear(hd->my_channel);
        secs_done += chunk;
    }
    lock_release(&hd->my_channel->lock);
}

void intr_hd_handler(uint8_t irq_no) {
    ASSERT(irq_no == 0x2e || irq_no == 0x2f);
    uint32_t no = irq_no - 0x2e;
    struct IDE_CHANNEL *channel = &channels[no];
    ASSERT(channel->irq_no == irq_no);
    channel->expecting_intr = 0;
    inb(reg_status(channel));
}

static void swap_pairs_bytes(const char *dst, char *buf, uint32_t len) {
    uint8_t idx;
    for (idx = 0; idx < len; idx += 2) {
        buf[idx + 1] = *dst;
        dst++;
        buf[idx] = *dst;
        dst++;
    }
    buf[idx] = '\0';
}

/**
 * 读取 IDENTIFY DEVICE 数据并填充盘容量。
 *
 * @param hd 已选定设备号的盘
 * @returns 成功返回 0；该设备号上没有盘（IDENTIFY 无响应）返回 -1
 */
static int identify_disk(struct DISK *hd) {
    char id_info[512];
    select_disk(hd);
    channel_send_cmd(hd->my_channel, CMD_IDENTIFY);

    if (!busy_wait(hd)) {
        kprintf("  %s: no drive on this position, skip\n", hd->name);
        return -1;
    }
    read_from_sector(hd, id_info, 1);

    char buf[64];
    uint8_t sn_start = 10 * 2, sn_len = 20;
    uint8_t md_start = 27 * 2, md_len = 40;
    swap_pairs_bytes(&id_info[sn_start], buf, sn_len);
    kprintf("  disk %s info:\n", hd->name);
    kprintf("    SN: %s\n", buf);
    memset(buf, 0, sizeof(buf));
    swap_pairs_bytes(&id_info[md_start], buf, md_len);
    kprintf("    MODULE: %s\n", buf);
    uint32_t sector = *(uint32_t *)&id_info[60 * 2];
    kprintf("    SECTORS: %d\n", (int)sector);
    kprintf("    CAPACITY: %dMB\n", (int)(sector * 512 / 1024 / 1024));
    hd->max_lba = (sector > 1) ? sector - 1 : MAX_LBA_DEFAULT;
    if (hd->max_lba > MAX_LBA28) {
        hd->max_lba = MAX_LBA28;
    }
    return 0;
}

static int ide_drv_init(void) {
    ide_init();
    return 0;
}

DRIVER_REGISTER("ide", 20, ide_drv_init);

void ide_init(void) {
    kprintf("ide_init start\n");

    ide_dma_init();

    uint8_t hd_cnt = *((uint8_t *)(0x475));
    if (hd_cnt == 0) {
        kprintf("  BDA disk count is 0 (UEFI boot?), probe all positions\n");
        hd_cnt = 4;
    }

    channel_cnt = (uint8_t)DIV_ROUND_UP(hd_cnt, 2);
    struct IDE_CHANNEL *channel;
    uint8_t channel_no = 0, dev_no = 0;
    uint8_t global_dev = 0;

    while (channel_no < channel_cnt) {
        channel = &channels[channel_no];
        sprintf(channel->name, "ide%d", channel_no);
        if (channel_no == 0) {
            channel->port_base = 0x1f0;
            channel->irq_no = 0x20 + 14;
        } else {
            channel->port_base = 0x170;
            channel->irq_no = 0x20 + 15;
        }
        channel->expecting_intr = 0;
        lock_init(&channel->lock);
        sema_init(&channel->disk_done, 0);

        while (dev_no < 2 && global_dev < hd_cnt) {
            struct DISK *hd = &channel->devices[dev_no];
            hd->my_channel = channel;
            hd->dev_no = dev_no;
            hd->kind = DISK_KIND_IDE;
            hd->drv_priv = NULL;
            sprintf(hd->name, "sd%c", 'a' + channel_no * 2 + dev_no);
            if (identify_disk(hd) != 0) {
                dev_no++;
                global_dev++;
                continue;
            }
            block_scan_partitions(hd);
            dev_no++;
            global_dev++;
        }
        dev_no = 0;
        channel_no++;
    }
    kprintf("\n  all partition info\n");
    block_print_partitions();
    kprintf("ide_init done\n");
}
