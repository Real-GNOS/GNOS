/*
 * nvme.c — NVMe 1.x block transport over PCI MMIO. (GPLv2)
 *
 * The protocol, in one paragraph.  A controller exposes a register block
 * (BAR0): capability, configuration, status, and the queue doorbells.  You
 * set up one admin submission/completion queue pair, enable the device with
 * CC.EN, identify it (controller, then each namespace), create one I/O
 * queue pair, and then move data with READ/WRITE commands whose data
 * buffer is named by two "physical region pages" (PRP) entries -- the NVMe
 * answer to scatter/gather.  Completion is signalled by the controller
 * flipping a phase bit in the completion queue entry; we poll for it, the
 * same trade ata.c makes, and for the same reason: no interrupt routing
 * to get wrong.
 *
 * Everything is bounded.  A controller that never sets CSTS.RDY fails
 * init after its own timeout rather than hanging the boot; a namespace
 * whose LBA format is not 512/no-metadata is skipped with a log line,
 * not driven blind.
 *
 * The block-device surface is the same one ata.c publishes: whole
 * namespaces and their partition windows share one vfs_ops_t, and the
 * partition-table scan is the shared MBR/GPT reader.  QEMU exposes an
 * NVMe controller with -drive file=disk.img,if=none,id=nvme -device nvme,drive=nvme.
 */
#include <stdint.h>

#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "vfs.h"
#include "subsys.h"
#include "kstring.h"
#include "debugcon.h"

/* Forward declaration: the partition scanner sits above the ops it uses. */
static uint64_t alloc_pages(uint64_t n);

/* Queue and scratch memory must be physically contiguous, which the
 * general-purpose heap does not promise: take a run straight from the PMM
 * and reach it through the HHDM direct map. */
static uint64_t alloc_pages(uint64_t n)
{
    return pmm_alloc_contiguous(n);
}

/* ---- register map ------------------------------------------------------ */
#define NVME_REG_CAP     0x0000    /* capabilities */
#define NVME_REG_VS      0x0008    /* version */
#define NVME_REG_INTMS   0x000C
#define NVME_REG_INTMC   0x0010
#define NVME_REG_CC      0x0014    /* configuration */
#define NVME_REG_CSTS    0x001C    /* status */
#define NVME_REG_AQA     0x0024    /* admin queue attributes */
#define NVME_REG_ASQ     0x0028    /* admin submission queue base */
#define NVME_REG_ACQ     0x0030    /* admin completion queue base */
#define NVME_REG_DBS     0x1000    /* doorbell stride 0 */

/* CAP bits */
#define CAP_TO_SHIFT     24        /* timeout unit: 500 ms */
#define CAP_TO_MASK      0xFFu
#define CAP_DSTRD_SHIFT  32
#define CAP_DSTRD_MASK   0xFu
#define CAP_MPSMIN_SHIFT 48
#define CAP_MPSMIN_MASK  0xFu

/* CC bits */
#define CC_EN            0x1u
#define CC_CSS_NVM       0x0u
#define CC_MPS_MIN       (0x0u << 7)   /* 4 KiB host page size */
#define CC_AMS_RR        (0x0u << 11)
#define CC_SHN_NONE      (0x0u << 14)
#define CC_IOSQES_64     (6u << 16)
#define CC_IOCQES_16     (4u << 20)

/* CSTS bits */
#define CSTS_RDY         0x1u
#define CSTS_CFS         0x2u

/* Admin commands */
#define AC_DELETE_SQ     0x00
#define AC_CREATE_SQ     0x01
#define AC_GET_LOG_PAGE  0x02
#define AC_DELETE_CQ     0x04
#define AC_CREATE_CQ     0x05
#define AC_IDENTIFY      0x06
#define AC_ABORT         0x08
#define AC_SET_FEATURES  0x09

/* I/O commands */
#define IOC_FLUSH        0x00
#define IOC_WRITE        0x01
#define IOC_READ         0x02

/* Identify CNS values */
#define CNS_NS           0x00
#define CNS_CTRL         0x01
#define CNS_ACTIVE_NS    0x02

/* Command dword 0 flag bits */
#define CMD_FUSE_MASK    0xC0000000u
#define CMD_PSDT         0x00400000u   /* PRPs, no offset */

/* Completion status */
#define CQE_PHASE        0x1u
#define CQE_SC_MASK      0xFFu

/* PRP entries per 4 KiB page: one page covers 2 MiB of data, which the
 * 128 KiB worst-case transfer here never exceeds, so PRP2 suffices with
 * a single-page PRP list -- no chained lists to walk. */
#define NVME_PAGE_SIZE   4096u
#define NVME_QDEPTH      16u
#define NVME_WAIT_SPINS  20000000u   /* seconds of patience at 40 ns/iter */

/* ---- devices ----------------------------------------------------------- */
#define NVME_MAX_CTRL    2
#define NVME_MAX_NS      4       /* namespaces per controller we publish */
#define NVME_MAX_PARTS   16

/* One 64-byte submission command.  Field order is fixed by the spec. */
typedef struct {
    uint32_t cdw0;       /* opcode + flags */
    uint32_t nsid;
    uint64_t reserved[2];
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_cmd_t;

/* One 16-byte completion entry. */
typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;     /* low bit = phase tag */
} nvme_cqe_t;

/* ---- the block-device ops ----------------------------------------------
 * Defined here so the partition scanner below can hand nodes to the VFS;
 * the read/write/ioctl bodies live further down with the device structs
 * they need. */
static int32_t bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len);
static int32_t bdev_write(vfs_node_t *n, uint64_t off, const void *buf,
                          uint32_t len);
static int32_t bdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg);

static const vfs_ops_t g_bdev_ops = {
    .read  = bdev_read,
    .write = bdev_write,
    .ioctl = bdev_ioctl,
    .mmap  = 0,
};

typedef struct {
    volatile uint8_t *mmio;
    uint32_t          dbs_stride;    /* doorbell stride in bytes */

    /* Physically contiguous queue memory: 4 pages, see nvme_queues_phys. */
    uint64_t          q_phys;
    nvme_cmd_t       *asq;
    nvme_cqe_t       *acq;
    nvme_cmd_t       *iosq;
    nvme_cqe_t       *iocq;

    /* Scratch: identify payloads (2 x 4 KiB) and the I/O staging page. */
    uint64_t          scr_phys;
    uint8_t          *id_ctrl;       /* 4096 bytes */
    uint8_t          *id_ns;         /* 4096 bytes */
    uint8_t          *io_page;       /* 4096 bytes, staging for split I/O */

    uint16_t          qdepth;
    uint16_t          asq_tail, acq_head;
    uint16_t          iosq_tail, iocq_head;
    uint16_t          next_cid;
    uint8_t           acq_phase, iocq_phase;
    uint8_t           ready;
    uint32_t          nn;            /* reported namespace count */
    char              model[41];
} nvme_ctrl_t;

typedef struct {
    nvme_ctrl_t *ctrl;
    uint32_t     nsid;
    uint64_t     nsect;              /* capacity in 512-byte sectors */
    uint8_t      is_ns;              /* 1 = whole namespace, 0 = partition */
    uint64_t     lba0;               /* partition window start */
    uint64_t     nsect_win;          /* partition window length */
    char         name[16];           /* "nvme0n1", "nvme0n1p3" */
    uint8_t      used;
} nvme_bdev_t;

/* The partition scanner (below the ops) publishes nodes through this. */
static nvme_bdev_t *ns_slot(int ctrl, int ns);

static nvme_ctrl_t g_ctrl[NVME_MAX_CTRL];
static int         g_nctrl;

/* Slot 0..NVME_MAX_NS*NVME_MAX_CTRL-1 are whole namespaces, the rest are
 * partition windows -- the same layout ata.c uses. */
#define NS_SLOT_MAX (NVME_MAX_CTRL * NVME_MAX_NS)
static nvme_bdev_t g_bdev[NS_SLOT_MAX + NVME_MAX_CTRL * NVME_MAX_PARTS];

static nvme_bdev_t *part_slot(int ctrl, int ns, int idx)
{
    return &g_bdev[NS_SLOT_MAX + (ctrl * NVME_MAX_NS + ns) * NVME_MAX_PARTS + idx];
}

/* ---- register access --------------------------------------------------- */

static inline uint32_t rd32(nvme_ctrl_t *c, uint32_t off)
{
    return *(volatile uint32_t *)(c->mmio + off);
}

static inline uint64_t rd64(nvme_ctrl_t *c, uint32_t off)
{
    /* rd64 through the MMIO window: split into two 32-bit reads, because
     * some toolchains emit an unlocked 8-byte mov that crosses a 4-byte
     * MMIO boundary and older hardware answers with a bus error. */
    uint32_t lo = rd32(c, off);
    uint32_t hi = rd32(c, off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static inline void wr32(nvme_ctrl_t *c, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(c->mmio + off) = v;
}

static inline uint32_t dbs_off(nvme_ctrl_t *c, uint16_t qid, int completion)
{
    return NVME_REG_DBS + ((uint32_t)qid * 2u + (completion ? 1u : 0u))
                         * c->dbs_stride;
}

static inline void doorbell(nvme_ctrl_t *c, uint16_t qid, int completion,
                            uint16_t value)
{
    wr32(c, dbs_off(c, qid, completion), value);
}

/* ---- waits ------------------------------------------------------------- */

static int wait_csts(nvme_ctrl_t *c, int want_ready)
{
    for (uint32_t i = 0; i < NVME_WAIT_SPINS; i++) {
        uint32_t s = rd32(c, NVME_REG_CSTS);
        if (s & CSTS_CFS)
            return -1;                       /* controller fatal status */
        if (((s & CSTS_RDY) != 0) == (want_ready != 0))
            return 0;
    }
    return -1;
}

/* ---- command submission ------------------------------------------------ */

/* Submit one command on the admin (admin=1) or I/O queue and poll for its
 * completion.  Returns the 32-bit completion result, or a negative errno.
 * Single-threaded by construction (kernel init and VFS calls), but the
 * queue indices are still maintained properly so a future caller from a
 * worker thread only has to take the lock it wants. */
static int64_t nvme_submit(nvme_ctrl_t *c, int admin, nvme_cmd_t *cmd)
{
    nvme_cmd_t *sq  = admin ? c->asq : c->iosq;
    nvme_cqe_t *cq  = admin ? c->acq : c->iocq;
    uint16_t   *tail = admin ? &c->asq_tail : &c->iosq_tail;
    uint16_t   *head = admin ? &c->acq_head : &c->iocq_head;
    uint8_t    *phase = admin ? &c->acq_phase : &c->iocq_phase;
    uint16_t    qid   = admin ? 0u : 1u;

    if (!c->ready)
        return -E_NODEV;

    uint16_t cid = c->next_cid++;
    /* cdw0: opcode in [7:0], FUSE [14:9], CID [31:16]. */
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);

    sq[*tail] = *cmd;
    if (++*tail >= c->qdepth)
        *tail = 0;
    doorbell(c, qid, 0, *tail);

    for (uint32_t i = 0; i < NVME_WAIT_SPINS; i++) {
        nvme_cqe_t *e = &cq[*head];
        if ((e->status & CQE_PHASE) != *phase)
            continue;                        /* slot not yet written */

        uint16_t st = e->status;
        uint16_t ecid = e->cid;

        /* Consume the entry before acting on it: advance head, ring the
         * completion doorbell, then flip the phase when wrapped. */
        if (++*head >= c->qdepth) {
            *head = 0;
            *phase ^= 1u;
        }
        doorbell(c, qid, 1, *head);

        if (ecid != cid)
            return -E_IO;                    /* completion for another cid */
        /* Status field: bit 0 = phase, bits 15:1 = SC.  SC != 0 is an
         * error (0 is "successful completion"). */
        if (st & ~CQE_PHASE) {
            uint8_t sc = (uint8_t)((st >> 1) & 0xFF);
            dbg_puts("NVMe: command failed sc=");
            dbg_puts_hex(sc);
            dbg_puts("\n");
            return -E_IO;
        }
        return (int64_t)e->result;
    }
    dbg_puts("NVMe: command timeout\n");
    return -E_IO;
}

/* Issue IDENTIFY into the controller's 4 KiB scratch page.  CNS 0
 * (namespace) lands in the id_ns page, CNS 1/2 (controller / active-ns
 * list) in the id_ctrl page -- the caller reads whichever it asked for. */
static int nvme_identify(nvme_ctrl_t *c, uint32_t cns, uint32_t nsid)
{
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = AC_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = (cns == CNS_NS) ? c->scr_phys + NVME_PAGE_SIZE : c->scr_phys;
    cmd.cdw10 = cns;
    return nvme_submit(c, 1, &cmd) < 0 ? -1 : 0;
}

static int nvme_create_queue(nvme_ctrl_t *c)
{
    nvme_cmd_t cmd;

    /* I/O completion queue (qid 1) first, then its submission queue: the
     * spec requires the CQ to exist before an SQ is bound to it. */
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = AC_CREATE_CQ;
    cmd.prp1  = c->q_phys + NVME_PAGE_SIZE * 3;   /* iocq page */
    cmd.cdw10 = 1u | ((uint32_t)(c->qdepth - 1) << 16);
    /* cdw11 for CREATE_CQ: IRQ vector [31:16] (unused, polled), flags
     * [0] = physically contiguous.  Completion entry size comes from
     * CC.IOCQES, not from this command. */
    cmd.cdw11 = 0x1u;
    if (nvme_submit(c, 1, &cmd) < 0)
        return -1;

    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0  = AC_CREATE_SQ;
    cmd.prp1  = c->q_phys + NVME_PAGE_SIZE * 2;   /* iosq page */
    cmd.cdw10 = 1u | ((uint32_t)(c->qdepth - 1) << 16);
    /* cdw11 for CREATE_SQ: CQID it is bound to [31:16], flags [0] =
     * physically contiguous.  Submission entry size comes from CC.IOSQES. */
    cmd.cdw11 = (1u << 16) | 0x1u;
    if (nvme_submit(c, 1, &cmd) < 0)
        return -1;

    return 0;
}

/* ---- controller bring-up ---------------------------------------------- */

static int nvme_init_ctrl(nvme_ctrl_t *c)
{
    uint64_t cap = rd64(c, NVME_REG_CAP);

    uint32_t timeout_unit = (uint32_t)((cap >> CAP_TO_SHIFT) & CAP_TO_MASK);
    uint32_t mpsmin       = (uint32_t)((cap >> CAP_MPSMIN_SHIFT) & CAP_MPSMIN_MASK);
    c->dbs_stride = 4u << ((cap >> CAP_DSTRD_SHIFT) & CAP_DSTRD_MASK);

    /* MPSMIN is the minimum host page size as log2(2*(N+1)) KiB... in
     * practice QEMU reports 0 (= 4 KiB).  Anything else and our single-page
     * PRP arithmetic would lie about the hardware's view; refuse cleanly. */
    if (mpsmin != 0) {
        dbg_puts("NVMe: unsupported MPSMIN, skipping\n");
        return -1;
    }

    /* Allow the controller's own timeout (500 ms units) plus slack. */
    (void)timeout_unit;

    if (rd32(c, NVME_REG_CC) & CC_EN) {
        wr32(c, NVME_REG_CC, 0);             /* disable to reconfigure */
        if (wait_csts(c, 0) < 0)
            return -1;
    }

    /* One contiguous arena: [0]=asq, [1]=acq, [2]=iosq, [3]=iocq pages. */
    c->q_phys = 0;
    nvme_cmd_t   *qmem = 0;
    uint64_t qframes = 4;
    uint64_t qphys = 0;
    /* The queues need 4 KiB each and must be physically contiguous per
     * queue (CC.ASQ/ACQ take one base each, no PRP list) -- a 4-page run
     * gives each queue its own aligned page. */
    qphys = alloc_pages(qframes);
    if (!qphys)
        return -1;
    qmem = (nvme_cmd_t *)pmm_virt(qphys);
    memset(qmem, 0, (size_t)(qframes * NVME_PAGE_SIZE));
    c->q_phys = qphys;
    c->asq    = (nvme_cmd_t *)(qmem);
    c->acq    = (nvme_cqe_t *)((uint8_t *)qmem + NVME_PAGE_SIZE);
    c->iosq   = (nvme_cmd_t *)((uint8_t *)qmem + NVME_PAGE_SIZE * 2);
    c->iocq   = (nvme_cqe_t *)((uint8_t *)qmem + NVME_PAGE_SIZE * 3);

    /* Scratch: identify ctrl (4 KiB) + identify ns (4 KiB) + I/O page. */
    uint64_t sphys = alloc_pages(3);
    if (!sphys)
        return -1;
    uint8_t *smem = (uint8_t *)pmm_virt(sphys);
    memset(smem, 0, (size_t)(3 * NVME_PAGE_SIZE));
    c->scr_phys = sphys;
    c->id_ctrl  = smem;
    c->id_ns    = smem + NVME_PAGE_SIZE;
    c->io_page  = smem + NVME_PAGE_SIZE * 2;

    c->qdepth    = NVME_QDEPTH;
    c->asq_tail  = c->acq_head = 0;
    c->iosq_tail = c->iocq_head = 0;
    c->next_cid  = 0;
    c->acq_phase = c->iocq_phase = 1;

    wr32(c, NVME_REG_AQA,
         (uint32_t)(c->qdepth - 1) | ((uint32_t)(c->qdepth - 1) << 16));
    wr32(c, NVME_REG_ASQ, (uint32_t)c->q_phys);
    wr32(c, NVME_REG_ASQ + 4, (uint32_t)(c->q_phys >> 32));
    wr32(c, NVME_REG_ACQ, (uint32_t)(c->q_phys + NVME_PAGE_SIZE));
    wr32(c, NVME_REG_ACQ + 4, (uint32_t)((c->q_phys + NVME_PAGE_SIZE) >> 32));

    wr32(c, NVME_REG_CC, CC_EN | CC_MPS_MIN | CC_AMS_RR |
                         CC_IOSQES_64 | CC_IOCQES_16);
    if (wait_csts(c, 1) < 0) {
        dbg_puts("NVMe: controller never became ready\n");
        return -1;
    }
    c->ready = 1;

    if (nvme_identify(c, CNS_CTRL, 0) < 0)
        return -1;

    /* Model string: bytes 24..64 of the identify-controller structure,
     * space-padded.  Trim to a NUL-terminated name for the log. */
    memcpy(c->model, c->id_ctrl + 24, 40);
    c->model[40] = 0;
    for (int i = 39; i >= 0 && (c->model[i] == ' ' || c->model[i] == 0); i--)
        c->model[i] = 0;

    c->nn = *(volatile uint32_t *)(c->id_ctrl + 516);  /* NN at byte 516 */

    if (nvme_create_queue(c) < 0)
        return -1;

    return 0;
}

/* ---- data transfer ----------------------------------------------------- */

/*
 * Move `nsect` 512-byte sectors starting at `lba`.  Every transfer stages
 * through the controller's own I/O page: the caller's buffer has no
 * physical continuity we could name with PRPs (kernel heap blocks span
 * pages arbitrarily), and a one-page bounce buffer keeps the PRP math
 * trivial -- PRP1 is always the staging page, no lists, no chains.  Whole
 * pages go one command each; sub-sector tails are impossible because the
 * VFS block layer only ever asks for whole sectors.
 */
static int nvme_rw(nvme_ctrl_t *c, uint32_t nsid, int write,
                   uint64_t lba, uint32_t nsect, void *buf)
{
    if (!c->ready || !nsect)
        return -1;

    /* One command moves at most one staging page (8 sectors at 512 B). */
    const uint32_t per_cmd = NVME_PAGE_SIZE / NVME_SECTOR;

    uint8_t *p = (uint8_t *)buf;
    for (uint32_t done = 0; done < nsect; ) {
        uint32_t take = nsect - done;
        if (take > per_cmd)
            take = per_cmd;

        if (write) {
            memcpy(c->io_page, p + (uint64_t)done * NVME_SECTOR,
                   (size_t)take * NVME_SECTOR);
        }

        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0  = write ? IOC_WRITE : IOC_READ;
        cmd.nsid  = nsid;
        cmd.prp1  = c->scr_phys + NVME_PAGE_SIZE * 2;   /* the I/O page */
        cmd.cdw10 = (uint32_t)(lba + done);
        cmd.cdw11 = (uint32_t)((lba + done) >> 32);
        cmd.cdw12 = take - 1;                            /* 0-based count */

        if (nvme_submit(c, 0, &cmd) < 0)
            return -1;

        if (!write) {
            memcpy(p + (uint64_t)done * NVME_SECTOR, c->io_page,
                   (size_t)take * NVME_SECTOR);
        }
        done += take;
    }
    return 0;
}

/* ---- partition scanning (shared MBR/GPT reader) ------------------------ */
/* Same layout ata.c parses; duplicated here rather than factored out so
 * each driver stays a single self-contained file. */

#define MBR_SIG_OFF   510
#define MBR_PART_OFF  446
#define GPT_PROTECTIVE 0xEE
#define GPT_ENTRY_LBA 72
#define GPT_ENTRY_NUM 80
#define GPT_ENTRY_SZ  84
#define GPT_ENTRY_MIN 128
#define GPT_ENTRY_FIRST 32

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static void part_name(char *dst, const char *ns, int idx)
{
    int i = 0;
    while (ns[i] && i < 12)
        dst[i] = ns[i], i++;
    if (idx < 9)
        dst[i++] = (char)('1' + idx);
    else {
        dst[i++] = (char)('1' + idx / 10);
        dst[i++] = (char)('0' + idx % 10);
    }
    dst[i] = 0;
}

static int ns_scan_partitions(int ctrl_idx, nvme_bdev_t *ns)
{
    nvme_ctrl_t *c = ns->ctrl;

    /* GPT first: protective MBR entry type 0xEE announces it. */
    uint8_t mbr[NVME_SECTOR];
    if (nvme_rw(c, ns->nsid, 0, ns->lba0, 1, mbr) < 0)
        return -1;
    if (mbr[MBR_SIG_OFF] != 0x55 || mbr[MBR_SIG_OFF + 1] != 0xAA)
        return 0;

    int idx = 0;
    if (mbr[MBR_PART_OFF + 4] == GPT_PROTECTIVE) {
        uint8_t hdr[NVME_SECTOR];
        if (nvme_rw(c, ns->nsid, 0, ns->lba0 + 1, 1, hdr) < 0)
            return -1;

        /* The entry array can be anywhere; read the first sectors' worth
         * that covers 80 entries (80 * 128 = 10240 bytes = 20 sectors). */
        uint64_t ent_lba  = rd64le(hdr + GPT_ENTRY_LBA);
        uint32_t ent_num  = rd32le(hdr + GPT_ENTRY_NUM);
        uint32_t ent_sz   = rd32le(hdr + GPT_ENTRY_SZ);
        if (ent_num > GPT_ENTRY_NUM)
            ent_num = GPT_ENTRY_NUM;
        if (ent_sz < GPT_ENTRY_MIN)
            ent_sz = GPT_ENTRY_MIN;

        static uint8_t ents[NVME_PAGE_SIZE];   /* 4 KiB: 32 entries of 128 B */
        uint32_t per_read = NVME_PAGE_SIZE / ent_sz;
        for (uint32_t base = 0; base < ent_num && idx < NVME_MAX_PARTS;
             base += per_read) {
            uint32_t take = ent_num - base;
            if (take > per_read)
                take = per_read;
            if (nvme_rw(c, ns->nsid, 0, ns->lba0 + ent_lba + base,
                        (take * ent_sz + NVME_SECTOR - 1) / NVME_SECTOR,
                        ents) < 0)
                return -1;
            for (uint32_t i = 0; i < take && idx < NVME_MAX_PARTS; i++) {
                const uint8_t *e = ents + i * ent_sz;
                static const uint8_t zero[16];
                if (!memcmp(e, zero, 16))
                    continue;                /* empty slot */
                uint64_t first = rd64le(e + GPT_ENTRY_FIRST);
                uint64_t last  = rd64le(e + 40);
                if (first < ns->nsect && last >= first && last < ns->nsect) {
                    nvme_bdev_t *p = part_slot(ctrl_idx,
                                               (int)(ns - ns_slot(ctrl_idx, 0)),
                                               idx);
                    p->used = 1;
                    p->is_ns = 0;
                    p->nsid  = ns->nsid;
                    p->lba0  = ns->lba0 + first;
                    p->nsect_win = last - first + 1;
                    part_name(p->name, ns->name, idx);
                    if (vfs_register_blkdev(p->name, &g_bdev_ops, p,
                                            p->nsect_win * NVME_SECTOR) == 0) {
                        idx++;
                    } else {
                        p->used = 0;
                    }
                }
            }
        }
        return idx;
    }

    /* MBR: four entries, type 0x00 = unused. */
    for (int i = 0; i < 4 && idx < NVME_MAX_PARTS; i++) {
        const uint8_t *e = mbr + MBR_PART_OFF + i * 16;
        if (!e[4])
            continue;
        uint64_t first = rd32le(e + 8);
        uint64_t count = rd32le(e + 12);
        if (first < ns->nsect && count && first + count <= ns->nsect) {
            nvme_bdev_t *p = part_slot(ctrl_idx,
                                       (int)(ns - ns_slot(ctrl_idx, 0)), idx);
            p->used = 1;
            p->is_ns = 0;
            p->nsid  = ns->nsid;
            p->lba0  = ns->lba0 + first;
            p->nsect_win = count;
            part_name(p->name, ns->name, idx);
            if (vfs_register_blkdev(p->name, &g_bdev_ops, p,
                                    p->nsect_win * NVME_SECTOR) == 0) {
                idx++;
            } else {
                p->used = 0;
            }
        }
    }
    return idx;
}

/* ---- the block-device ops --------------------------------------------- */

/*
 * Whole-window read/write.  `off` and `len` are arbitrary byte ranges (the
 * VFS lets user space read one byte at a time), so sub-sector heads and
 * tails go through a bounce sector -- read-modify-write on the tail, the
 * same discipline ata.c applies.  The middle is handed to nvme_rw whole;
 * nvme_rw further splits it into staging-page commands.
 */
static int32_t bdev_rw_window(nvme_bdev_t *b, uint64_t off, void *buf,
                              uint32_t len, int write)
{
    uint64_t cap = (b->is_ns ? b->nsect : b->nsect_win) * NVME_SECTOR;
    if (off >= cap)
        return 0;                          /* EOF */
    if (off + len > cap)
        len = (uint32_t)(cap - off);
    if (!len)
        return 0;

    uint64_t lba0 = b->is_ns ? 0 : b->lba0;
    uint8_t  *p   = (uint8_t *)buf;
    uint64_t  pos = off;
    int32_t   rc  = 0;

    uint8_t sec[NVME_SECTOR];

    /* Head: partial first sector. */
    uint32_t skip = (uint32_t)(pos % NVME_SECTOR);
    if (skip && len) {
        uint32_t chunk = NVME_SECTOR - skip;
        if (chunk > len)
            chunk = len;
        uint64_t lba = lba0 + pos / NVME_SECTOR;
        if (nvme_rw(b->ctrl, b->nsid, 0, lba, 1, sec) < 0)
            return -E_IO;
        if (write) {
            memcpy(sec + skip, p, chunk);
            if (nvme_rw(b->ctrl, b->nsid, 1, lba, 1, sec) < 0)
                return -E_IO;
        } else {
            memcpy(p, sec + skip, chunk);
        }
        p    += chunk;
        pos  += chunk;
        len  -= chunk;
        rc   += chunk;
    }

    /* Middle: whole sectors straight through. */
    if (len >= NVME_SECTOR) {
        uint32_t whole = len / NVME_SECTOR;
        if (nvme_rw(b->ctrl, b->nsid, write,
                    lba0 + pos / NVME_SECTOR, whole, p) < 0)
            return rc ? rc : -E_IO;
        p   += (uint64_t)whole * NVME_SECTOR;
        pos += (uint64_t)whole * NVME_SECTOR;
        len -= whole * NVME_SECTOR;
        rc  += (int32_t)whole * NVME_SECTOR;
    }

    /* Tail: partial last sector, read-modify-write. */
    if (len) {
        uint64_t lba = lba0 + pos / NVME_SECTOR;
        if (nvme_rw(b->ctrl, b->nsid, 0, lba, 1, sec) < 0)
            return rc ? rc : -E_IO;
        if (write) {
            memcpy(sec, p, len);
            if (nvme_rw(b->ctrl, b->nsid, 1, lba, 1, sec) < 0)
                return rc ? rc : -E_IO;
        } else {
            memcpy(p, sec, len);
        }
        rc += (int32_t)len;
    }
    return rc;
}

static int32_t bdev_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    nvme_bdev_t *b = (nvme_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;
    return bdev_rw_window(b, off, buf, len, 0);
}

static int32_t bdev_write(vfs_node_t *n, uint64_t off, const void *buf,
                          uint32_t len)
{
    nvme_bdev_t *b = (nvme_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;
    /* const-qualified upstream, the driver does not write through it
     * beyond handing the bytes to the controller. */
    return bdev_rw_window(b, off, (void *)(uintptr_t)buf, len, 1);
}

static int32_t bdev_ioctl(vfs_node_t *n, uint64_t cmd, uint64_t arg)
{
    nvme_bdev_t *b = (nvme_bdev_t *)n->priv;
    if (!b || !b->used)
        return -E_IO;

    switch (cmd) {
    case 0x1260:                          /* BLKGETSIZE: sector count */
        if (!arg) return -E_FAULT;
        *(unsigned long *)(uintptr_t)arg =
            (unsigned long)(b->is_ns ? b->nsect : b->nsect_win);
        return 0;
    case 0x80081272:                      /* BLKGETSIZE64 */
        if (!arg) return -E_FAULT;
        *(uint64_t *)(uintptr_t)arg =
            (b->is_ns ? b->nsect : b->nsect_win) * NVME_SECTOR;
        return 0;
    case 0x1268:                          /* BLKSSZGET */
        if (!arg) return -E_FAULT;
        *(int *)(uintptr_t)arg = NVME_SECTOR;
        return 0;
    default:
        return -E_NOTTY;
    }
}

/* The forward-declared ops table lives above; ns_slot() was likewise
 * forward-declared for the partition scanner. */
static nvme_bdev_t *ns_slot(int ctrl, int ns)
{
    return &g_bdev[ctrl * NVME_MAX_NS + ns];
}

/* ---- namespace publication -------------------------------------------- */

static int ns_register(nvme_ctrl_t *c, int ctrl_idx, uint32_t nsid)
{
    if (nvme_identify(c, CNS_NS, nsid) < 0)
        return -1;

    /* Identify-namespace: NSZE (size, sectors) at 0, FLBAS at byte 26
     * (low nibble = the LBA format the controller currently applies), and
     * the LBA format table at byte 128, 4 bytes per entry: MS [15:0],
     * LBADS [23:16].  Only 512-byte sectors without metadata are driven;
     * anything else is skipped with a note. */
    uint64_t nsze  = *(volatile uint64_t *)c->id_ns;
    uint32_t flbas = c->id_ns[26] & 0x0F;
    uint32_t lbo   = (c->id_ns[26] >> 4) & 0x0F;
    uint16_t ms    = *(volatile uint16_t *)(c->id_ns + 128 + flbas * 4);
    uint8_t  lbads = c->id_ns[128 + flbas * 4 + 2];

    if (!nsze || lbads != 9 || ms != 0 || lbo != 0) {
        dbg_puts("NVMe: namespace ");
        dbg_puts_dec(nsid);
        dbg_puts(" unsupported format (lbads=");
        dbg_puts_dec(lbads);
        dbg_puts(")\n");
        return -1;
    }

    for (int i = 0; i < NVME_MAX_NS; i++) {
        nvme_bdev_t *b = ns_slot(ctrl_idx, i);
        if (b->used)
            continue;
        b->used  = 1;
        b->is_ns = 1;
        b->ctrl  = c;
        b->nsid  = nsid;
        b->nsect = nsze;
        b->lba0  = 0;
        b->nsect_win = 0;

        /* Name: "nvme<ctrl>n<ns>". */
        char *n = b->name;
        *n++ = 'n'; *n++ = 'v'; *n++ = 'm'; *n++ = 'e';
        *n++ = (char)('0' + ctrl_idx);
        *n++ = 'n';
        if (nsid < 10)
            *n++ = (char)('0' + nsid);
        else {
            *n++ = (char)('0' + nsid / 10);
            *n++ = (char)('0' + nsid % 10);
        }
        *n = 0;

        if (vfs_register_blkdev(b->name, &g_bdev_ops, b,
                                nsze * NVME_SECTOR) != 0) {
            b->used = 0;
            return -1;
        }

        dbg_puts("NVMe: /dev/");
        dbg_puts(b->name);
        dbg_puts(" ");
        dbg_puts_dec((uint32_t)(nsze / 2048));
        dbg_puts(" MiB\n");

        ns_scan_partitions(ctrl_idx, b);
        return 0;
    }
    return -1;
}

/* ---- PCI probing ------------------------------------------------------- */

#define PCI_CLASS_MASS   0x01
#define PCI_SUB_NVM      0x08

int nvme_init(void)
{
    static int inited;
    if (inited)
        return g_nctrl;
    inited = 1;

    for (int i = 0; i < g_pci_count && g_nctrl < NVME_MAX_CTRL; i++) {
        const pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor == 0xFFFF)
            continue;
        if (d->class_code != PCI_CLASS_MASS || d->subclass != PCI_SUB_NVM)
            continue;

        uint64_t bar = pci_map_bar(d, 0);
        if (!bar) {
            dbg_puts("NVMe: no MMIO BAR\n");
            continue;
        }

        pci_enable(d);

        nvme_ctrl_t *c = &g_ctrl[g_nctrl];
        memset(c, 0, sizeof *c);
        c->mmio = (volatile uint8_t *)bar;

        if (nvme_init_ctrl(c) < 0) {
            dbg_puts("NVMe: controller init failed\n");
            continue;
        }
        int idx = g_nctrl++;

        dbg_puts("NVMe: ctrl ");
        dbg_puts_dec(idx);
        dbg_puts(" \"");
        dbg_puts(c->model);
        dbg_puts("\" nn=");
        dbg_puts_dec(c->nn);
        dbg_puts("\n");

        /* Enumerate active namespaces via the identify list; a controller
         * that rejects the list falls back to scanning 1..NN. */
        uint32_t list[NVME_PAGE_SIZE / 4];
        nvme_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.cdw0  = AC_IDENTIFY;
        cmd.prp1  = c->scr_phys + NVME_PAGE_SIZE;  /* id_ns page as scratch */
        cmd.cdw10 = CNS_ACTIVE_NS;
        /* The list lands in id_ns scratch; read through the mapping. */
        if (nvme_submit(c, 1, &cmd) >= 0) {
            memcpy(list, c->id_ns, sizeof list);
            int posted = 0;
            for (unsigned k = 0; k < sizeof list / sizeof list[0]; k++) {
                if (!list[k])
                    break;
                if (ns_register(c, idx, list[k]) == 0)
                    posted++;
                else
                    break;
            }
            if (posted)
                continue;
        }
        for (uint32_t nsid = 1; nsid <= c->nn && nsid <= NVME_MAX_NS; nsid++)
            ns_register(c, idx, nsid);
    }

    if (!g_nctrl) {
        dbg_puts("NVMe: no controllers\n");
        return 0;
    }

    int slot = subsys_register("nvme", "nvme0n1", SUBSYS_CLASS_BLOCK, 8, 1);
    subsys_set_state(slot, SUBSYS_STATE_LIVE);
    return g_nctrl;
}
