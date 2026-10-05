#include "drivers/block/nvme.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "drivers/char/console/io.h"
#include "drivers/driver_ops.h"
#include "drivers/pci/pci.h"
#include "kernel/asm_func.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"
#include "libc/user/stdio.h"

#define NVME_CAP 0x00u
#define NVME_CC 0x14u
#define NVME_CSTS 0x1Cu
#define NVME_AQA 0x24u
#define NVME_ASQ 0x28u
#define NVME_ACQ 0x30u
#define NVME_DB_BASE 0x1000u

#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_NVM (0u << 4)
#define NVME_CC_IOSQES_64 (6u << 16)
#define NVME_CC_IOCQES_16 (4u << 20)
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

#define NVME_CAP_MQES(v) ((v)&0xFFFFu)
#define NVME_CAP_DSTRD(v) (((v) >> 0) & 0xFu)
#define NVME_CAP_CSS(v) (((v) >> 5) & 0xFFu)
#define NVME_CAP_MPSMIN(v) (((v) >> 16) & 0xFu)

#define NVME_OPC_CREATE_IOSQ 0x01u
#define NVME_OPC_READ 0x02u
#define NVME_OPC_CREATE_IOCQ 0x05u
#define NVME_OPC_IDENTIFY 0x06u
#define NVME_OPC_WRITE 0x01u

#define NVME_IDENT_CNS_CTRL 1u
#define NVME_IDENT_CNS_NS 0u

#define NVME_AQ_NR 16u
#define NVME_IOQ_NR 16u
#define NVME_BOUNCE_PAGES 16u

#define NVME_SPIN_LIMIT 100000000u

static struct NVME_CTRL s_ctrl;
static struct DISK s_disk;
static int s_nvme_ready;

static uint32_t nvme_reg_read(uint32_t off) {
    return s_ctrl.regs[off / 4];
}

static void nvme_reg_write(uint32_t off, uint32_t v) {
    s_ctrl.regs[off / 4] = v;
}

static uint32_t nvme_sq_db(uint32_t qid) {
    return NVME_DB_BASE + 2u * qid * (4u << s_ctrl.doorbell_stride);
}

static uint32_t nvme_cq_db(uint32_t qid) {
    return NVME_DB_BASE + (2u * qid + 1u) * (4u << s_ctrl.doorbell_stride);
}

static void queue_init(struct NVME_QUEUE *q, void *virt, uint32_t phys,
                       uint32_t nr) {
    q->phys = phys;
    q->virt = (uint32_t *)virt;
    q->nr = nr;
    q->head = 0;
    q->tail = 0;
    q->phase = 1;
}

static void nvme_submit(struct NVME_QUEUE *q, uint32_t qid, const uint32_t *cmd) {
    uint32_t *slot = q->virt + q->tail * 16;

    memcpy(slot, cmd, 64);
    q->tail = (q->tail + 1) % q->nr;
    nvme_reg_write(nvme_sq_db(qid), q->tail);
}

/**
 * 轮询一条完成队列项并回敲门铃。
 *
 * @param cq     完成队列
 * @param qid    队列号，决定头门铃寄存器偏移
 * @param result 输出，命令特定结果值，可为 NULL
 * @returns 成功返回 0；超时返回 -1；控制器报错返回 -2
 */
static int nvme_poll(struct NVME_QUEUE *cq, uint32_t qid, uint32_t *result) {
    volatile uint32_t *e = cq->virt + cq->head * 4;
    uint32_t spins = 0;

    while ((((e[3] >> 16) & 1u) != cq->phase)) {
        asm_pause();
        if (++spins > NVME_SPIN_LIMIT) {
            return -1;
        }
    }
    uint32_t dw2 = e[2];
    uint32_t dw3 = e[3];
    if (result) {
        *result = e[0];
    }
    cq->head = dw2 & 0xFFFFu;
    if (cq->head == 0) {
        cq->phase ^= 1u;
    }
    nvme_reg_write(nvme_cq_db(qid), cq->head);
    return ((dw3 >> 16) & 0xFFFEu) == 0 ? 0 : -2;
}

static int nvme_admin(const uint32_t *cmd, uint32_t *result) {
    nvme_submit(&s_ctrl.asq, 0, cmd);
    int rc = nvme_poll(&s_ctrl.acq, 0, result);
    if (rc != 0) {
        volatile uint32_t *e0 = s_ctrl.acq.virt;
        kprintf("  nvme: admin opc=%u rc=%d csts=%x cq=%x.%x\n",
                (unsigned)(cmd[0] & 0xFFu), rc,
                (unsigned)nvme_reg_read(NVME_CSTS), (unsigned)e0[3],
                (unsigned)e0[0]);
    }
    return rc;
}

/**
 * 组装一条 64 字节提交队列项。
 *
 * @param e      目标，16 个双字
 * @param opcode 命令操作码
 * @param cid    命令标识
 * @param nsid   命名空间，仅数据命令使用
 * @param prp1   第一页物理地址，非数据命令填 0
 * @param prp2   第二页或 PRP 列表页物理地址，可填 0
 * @param cdw10  命令双字 10
 * @param cdw11  命令双字 11
 */
static void nvme_build_cmd(uint32_t *e, uint8_t opcode, uint32_t cid,
                           uint32_t nsid, uint64_t prp1, uint64_t prp2,
                           uint32_t cdw10, uint32_t cdw11) {
    memset(e, 0, 64);
    e[0] = ((cid & 0xFFFFu) << 16) | opcode;
    e[1] = nsid;
    e[6] = (uint32_t)prp1;
    e[7] = (uint32_t)(prp1 >> 32);
    e[8] = (uint32_t)prp2;
    e[9] = (uint32_t)(prp2 >> 32);
    e[10] = cdw10;
    e[11] = cdw11;
}

static int nvme_wait_rdy(uint32_t expect, uint32_t spins) {
    for (uint32_t i = 0; i < spins; i++) {
        if ((nvme_reg_read(NVME_CSTS) & NVME_CSTS_RDY) == expect) {
            return 0;
        }
        asm_pause();
    }
    return -1;
}

static int nvme_create_ioq(void) {
    uint32_t cmd[16];
    uint32_t result = 0;

    nvme_build_cmd(cmd, NVME_OPC_CREATE_IOCQ, 1, 0, s_ctrl.iocq.phys, 0,
                   (uint32_t)(s_ctrl.iocq.nr - 1) << 16 | 1u, 1u);
    if (nvme_admin(cmd, &result) != 0) {
        return -1;
    }

    nvme_build_cmd(cmd, NVME_OPC_CREATE_IOSQ, 2, 0, s_ctrl.iosq.phys, 0,
                   (uint32_t)(s_ctrl.iosq.nr - 1) << 16 | 1u, (1u << 16) | 1u);
    if (nvme_admin(cmd, &result) != 0) {
        return -1;
    }
    return 0;
}

static int nvme_identify(void) {
    uint32_t cmd[16];
    uint64_t nsze;

    nvme_build_cmd(cmd, NVME_OPC_IDENTIFY, 3, 0, s_ctrl.identify_phys, 0,
                   NVME_IDENT_CNS_CTRL, 0);
    if (nvme_admin(cmd, NULL) != 0) {
        return -1;
    }

    nvme_build_cmd(cmd, NVME_OPC_IDENTIFY, 4, 1, s_ctrl.identify_phys + 4096u,
                   0, NVME_IDENT_CNS_NS, 0);
    if (nvme_admin(cmd, NULL) != 0) {
        return -1;
    }
    uint8_t *ns = s_ctrl.identify + 4096;
    nsze = *(uint64_t *)(void *)ns;
    uint32_t lbads = ns[130];
    if (nsze == 0) {
        kprintf("  nvme: namespace 1 inactive\n");
        return -1;
    }
    if (lbads != 9u) {
        kprintf("  nvme: unsupported logical block size 2^%u\n",
                (unsigned)lbads);
        return -1;
    }
    s_ctrl.nsid = 1;
    s_ctrl.lba_shift = lbads;
    s_disk.max_lba = (nsze > 1) ? (uint32_t)(nsze - 1) : 0;
    if (s_disk.max_lba > 0x0FFFFFFFu) {
        s_disk.max_lba = 0x0FFFFFFFu;
    }
    kprintf("  nvme: nsze=%u sectors lba=%uB\n", (unsigned)nsze,
            1u << lbads);
    return 0;
}

static int nvme_ctrl_setup(void) {
    uint32_t cap0 = nvme_reg_read(NVME_CAP);
    uint32_t cap1 = nvme_reg_read(NVME_CAP + 4u);

    if (!(NVME_CAP_CSS(cap1) & 1u)) {
        kprintf("  nvme: NVM command set not supported\n");
        return -1;
    }
    if (NVME_CAP_MQES(cap0) < NVME_IOQ_NR - 1u) {
        kprintf("  nvme: max queue depth %u below required %u\n",
                (unsigned)(NVME_CAP_MQES(cap0) + 1u), (unsigned)NVME_IOQ_NR);
        return -1;
    }
    if (NVME_CAP_MPSMIN(cap1) != 0u) {
        kprintf("  nvme: minimum page size 2^%u exceeds 4KB\n",
                (unsigned)NVME_CAP_MPSMIN(cap1));
        return -1;
    }
    s_ctrl.doorbell_stride = NVME_CAP_DSTRD(cap1);

    nvme_reg_write(NVME_CC, 0);
    if (nvme_wait_rdy(0, NVME_SPIN_LIMIT) != 0) {
        kprintf("  nvme: controller stuck ready=1\n");
        return -1;
    }

    nvme_reg_write(NVME_AQA,
                   (uint32_t)(s_ctrl.acq.nr - 1) << 16 |
                       (s_ctrl.asq.nr - 1));
    nvme_reg_write(NVME_ASQ, (uint32_t)s_ctrl.asq.phys);
    nvme_reg_write(NVME_ASQ + 4u, (uint32_t)(s_ctrl.asq.phys >> 32));
    nvme_reg_write(NVME_ACQ, (uint32_t)s_ctrl.acq.phys);
    nvme_reg_write(NVME_ACQ + 4u, (uint32_t)(s_ctrl.acq.phys >> 32));

    nvme_reg_write(NVME_CC, NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_IOSQES_64 |
                                NVME_CC_IOCQES_16);
    if (nvme_wait_rdy(NVME_CSTS_RDY, NVME_SPIN_LIMIT) != 0 ||
        (nvme_reg_read(NVME_CSTS) & NVME_CSTS_CFS)) {
        kprintf("  nvme: controller failed to enable\n");
        return -1;
    }
    return 0;
}

int nvme_init(void) {
    uint32_t bdf = 0;
    uint32_t bar_lo;
    uint32_t bar_hi;

    if (pci_find_class(PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVM, &bdf) != 0) {
        kprintf("  nvme: no controller on pci bus 0\n");
        return -1;
    }
    bar_lo = pci_config_read32(bdf, PCI_CFG_BAR0);
    bar_hi = pci_config_read32(bdf, 0x14u);
    if ((bar_lo & 0x6u) != 0x4u) {
        kprintf("  nvme: bar0 is not a 64-bit mem bar (0x%x)\n", bar_lo);
        return -1;
    }
    pci_cmd_set(bdf, PCI_CMD_MEM | PCI_CMD_BUS_MASTER);

    s_ctrl.regs = ioremap(bar_lo & ~0xFFFu, 0x4000u);
    if (s_ctrl.regs == NULL) {
        kprintf("  nvme: ioremap failed\n");
        return -1;
    }
    if (bar_hi != 0) {
        kprintf("  nvme: bar0 above 4G, truncated\n");
    }

    void *asq = get_kernel_pages(1);
    void *acq = get_kernel_pages(1);
    void *iosq = get_kernel_pages(1);
    void *iocq = get_kernel_pages(1);
    s_ctrl.identify = get_kernel_pages(2);
    s_ctrl.bounce = get_kernel_pages(NVME_BOUNCE_PAGES);
    s_ctrl.prp_list = get_kernel_pages(1);
    if (!asq || !acq || !iosq || !iocq || !s_ctrl.identify || !s_ctrl.bounce ||
        !s_ctrl.prp_list) {
        kprintf("  nvme: no dma memory\n");
        return -1;
    }
    s_ctrl.identify_phys = PHY_OF((uintptr_t)s_ctrl.identify);
    s_ctrl.bounce_phys = PHY_OF((uintptr_t)s_ctrl.bounce);
    s_ctrl.prp_list_phys = PHY_OF((uintptr_t)s_ctrl.prp_list);
    queue_init(&s_ctrl.asq, asq, PHY_OF((uintptr_t)asq), NVME_AQ_NR);
    queue_init(&s_ctrl.acq, acq, PHY_OF((uintptr_t)acq), NVME_AQ_NR);
    queue_init(&s_ctrl.iosq, iosq, PHY_OF((uintptr_t)iosq), NVME_IOQ_NR);
    queue_init(&s_ctrl.iocq, iocq, PHY_OF((uintptr_t)iocq), NVME_IOQ_NR);

    if (nvme_ctrl_setup() != 0 || nvme_identify() != 0 ||
        nvme_create_ioq() != 0) {
        return -1;
    }

    memset(&s_disk, 0, sizeof(s_disk));
    sprintf(s_disk.name, "nvm");
    s_disk.kind = DISK_KIND_NVME;
    s_disk.drv_priv = &s_ctrl;
    s_nvme_ready = 1;
    block_scan_partitions(&s_disk);

    return 0;
}

static int nvme_rw(struct DISK *hd, uint32_t lba, void *buf, uint32_t count,
                   int is_write) {
    struct NVME_CTRL *c = (struct NVME_CTRL *)hd->drv_priv;
    uint32_t max_sect = (NVME_BOUNCE_PAGES * 4096u) >> 9;
    uint32_t cmd[16];

    for (uint32_t done = 0; done < count;) {
        uint32_t chunk = count - done;
        uint32_t bytes;
        uint32_t pages;

        if (chunk > max_sect) {
            chunk = max_sect;
        }
        bytes = chunk * 512u;
        pages = (bytes + 4095u) / 4096u;
        if (is_write) {
            memcpy(c->bounce, (const uint8_t *)buf + done * 512u, bytes);
        }

        uint64_t prp1 = c->bounce_phys;
        uint64_t prp2 = 0;
        if (pages == 2) {
            prp2 = c->bounce_phys + 4096u;
        } else if (pages > 2) {
            for (uint32_t i = 1; i < pages; i++) {
                c->prp_list[i - 1] = (uint64_t)(c->bounce_phys + i * 4096u);
            }
            prp2 = c->prp_list_phys;
        }

        uint32_t cid = ++c->cid;
        nvme_build_cmd(cmd, is_write ? NVME_OPC_WRITE : NVME_OPC_READ, cid,
                       c->nsid, prp1, prp2, (uint32_t)(lba + done), 0);
        cmd[12] = chunk - 1;
        nvme_submit(&c->iosq, 1, cmd);
        if (nvme_poll(&c->iocq, 1, NULL) != 0) {
            kprintf("nvme: %s timeout at lba %u\n", is_write ? "write" : "read",
                    (unsigned)(lba + done));
            return -1;
        }
        if (!is_write) {
            memcpy((uint8_t *)buf + done * 512u, c->bounce, bytes);
        }
        done += chunk;
    }
    return 0;
}

int nvme_read_sectors(struct DISK *hd, uint32_t lba, void *buf, uint32_t count) {
    if (!s_nvme_ready) {
        return -1;
    }
    return nvme_rw(hd, lba, buf, count, 0);
}

int nvme_write_sectors(struct DISK *hd, uint32_t lba, const void *buf,
                       uint32_t count) {
    if (!s_nvme_ready) {
        return -1;
    }
    return nvme_rw(hd, lba, (void *)buf, count, 1);
}

static int nvme_drv_init(void) {
    return nvme_init();
}

DRIVER_REGISTER("nvme", 18, nvme_drv_init);
