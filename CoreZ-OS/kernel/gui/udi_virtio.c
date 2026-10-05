#include "kernel/gui/udi.h"
#include "kernel/gui/vgpu_pci.h"

#include "drivers/char/console/io.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

#define VIRTIO_GPU_VENDOR 0x1AF4
#define VIRTIO_GPU_DEVICE 0x1050

#define VGPU_CTRL_VQ 0
#define VGPU_CURSOR_VQ 1

enum {
    VGPU_RESP_OK_NODATA = 0x1100,
    VGPU_RESP_OK_DATA = 0x1101,
    VGPU_RESP_ERR_UNSPEC = 0x1200,
    VGPU_RESP_ERR_OUT_OF_MEMORY = 0x1201,
    VGPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VGPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VGPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VGPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
};

enum {
    VGPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VGPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VGPU_CMD_RESOURCE_UNREF = 0x0102,
    VGPU_CMD_SET_SCANOUT = 0x0103,
    VGPU_CMD_RESOURCE_FLUSH = 0x0104,
    VGPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VGPU_CMD_ATTACH_BACKING = 0x0106,
    VGPU_CMD_DETACH_BACKING = 0x0107,
    VGPU_CMD_UPDATE_CURSOR = 0x0300,
    VGPU_CMD_MOVE_CURSOR = 0x0301,
};

#define VGPU_CURSOR_DIM 64

enum {
    VGPU_FORMAT_B8G8R8A8_UNORM = 1,
};

struct VGPU_RECT {
    uint32_t x, y, w, h;
};

struct VGPU_CTRL_HDR {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed));

struct VGPU_CTRL_RESP {
    struct VGPU_CTRL_HDR hdr;
} __attribute__((packed));

struct VGPU_RESOURCE_CREATE_2D {
    struct VGPU_CTRL_HDR hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct VGPU_RESOURCE_UNREF {
    struct VGPU_CTRL_HDR hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_SET_SCANOUT {
    struct VGPU_CTRL_HDR hdr;
    struct VGPU_RECT r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct VGPU_RESOURCE_FLUSH {
    struct VGPU_CTRL_HDR hdr;
    struct VGPU_RECT r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_TRANSFER_TO_HOST_2D {
    struct VGPU_CTRL_HDR hdr;
    struct VGPU_RECT r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_ATTACH_BACKING {
    struct VGPU_CTRL_HDR hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct VGPU_MEM_ENTRY {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_CURSOR_POS {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_UPDATE_CURSOR {
    struct VGPU_CTRL_HDR hdr;
    struct VGPU_CURSOR_POS pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} __attribute__((packed));

struct VGPU_VQ {
    volatile struct {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } *desc;
    volatile struct {
        uint16_t flags;
        uint16_t idx;
        uint16_t ring[];
    } *avail;
    volatile struct {
        uint16_t flags;
        uint16_t idx;
        volatile struct {
            uint32_t idx;
            uint32_t len;
        } ring[];
    } *used;
    uint16_t size;
    uint16_t last_used;
    uint16_t free_head;
    uint16_t free_count;
};

static struct VGPU_DEV {
    volatile uint8_t *common;
    volatile uint8_t *notify;
    uint32_t notify_mult;
    uint16_t notify_off;
    uint32_t queue_size;
    struct VGPU_VQ ctrlq;
    struct VGPU_VQ cursorq;
    uint64_t qdesc_phys;
    uint64_t cursor_qdesc_phys;
    uint32_t ready;
    uint32_t next_resource;
} vg;

static void vg_cfg_write32(uint32_t off, uint32_t v) {
    vg_mmio_write32(vg.common, off, v);
}

static uint32_t vg_cfg_read32(uint32_t off) {
    return vg_mmio_read32(vg.common, off);
}

static void vg_cfg_write16(uint32_t off, uint16_t v) {
    vg_mmio_write16(vg.common, off, v);
}

static void vg_cfg_write8(uint32_t off, uint8_t v) {
    vg_mmio_write8(vg.common, off, v);
}

static int vg_parse_caps(uint8_t bus, uint8_t dev) {
    uint32_t status = pci_read32(bus, dev, 0x06);
    if (!(status & 0x00100000u))
        return -1;
    uint8_t cap_ptr = (uint8_t)(pci_read32(bus, dev, 0x34) & 0xFF);
    int found = 0;
    uint32_t common_off = 0, notify_off = 0, notify_mult = 0;
    uint32_t common_len = 0, notify_len = 0;
    uint8_t common_bar = 0, notify_bar = 0;
    uint64_t bars[6] = {0};

    for (int guard = 0; guard < 16 && cap_ptr; guard++) {
        uint32_t c0 = pci_read32(bus, dev, cap_ptr);
        uint8_t cap_vndr = c0 & 0xFF;
        uint8_t cap_next = (c0 >> 8) & 0xFF;
        uint8_t cap_len = (c0 >> 16) & 0xFF;
        uint8_t cfg_type = (c0 >> 24) & 0xFF;
        if (cap_vndr != 0x09) {
            cap_ptr = cap_next;
            continue;
        }
        uint32_t c1 = pci_read32(bus, dev, (uint32_t)(cap_ptr + 4));
        uint8_t bar = (c1 & 0xFF);
        uint32_t offset = pci_read32(bus, dev, (uint32_t)(cap_ptr + 8));
        uint32_t length = pci_read32(bus, dev, (uint32_t)(cap_ptr + 12));
        if (bar < 6) {
            if (bars[bar] == 0) {
                uint32_t lo = pci_read32(bus, dev, 0x10 + bar * 4);
                uint32_t hi = pci_read32(bus, dev, 0x10 + bar * 4 + 4);
                uint64_t base = (lo & ~0xFu) | ((uint64_t)hi << 32);
                if (lo & 1)
                    base = lo & ~0x3u;
                bars[bar] = base;
            }
            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                common_off = offset;
                common_len = length;
                common_bar = bar;
                found |= 1;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                notify_off = offset;
                notify_len = length ? length : 4096;
                notify_mult = (cap_len >= 20)
                                  ? pci_read32(bus, dev,
                                               (uint32_t)(cap_ptr + 16))
                                  : 4;
                notify_bar = bar;
                found |= 2;
            }
        }
        (void)cap_len;
        cap_ptr = cap_next;
    }
    if (found != 3 || bars[common_bar] == 0 || bars[notify_bar] == 0)
        return -1;

    vg.common = (volatile uint8_t *)ioremap(
        (uint32_t)(bars[common_bar] + common_off),
        (common_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    vg.notify = (volatile uint8_t *)ioremap(
        (uint32_t)(bars[notify_bar] + notify_off),
        (notify_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    if (vg.common == 0 || vg.notify == 0)
        return -1;
    vg.notify_mult = notify_mult;
    vg.notify_off = (uint16_t)notify_off;
    return 0;
}

/*
 * KASLR-safe virtual -> physical translation for DMA.
 * Walks the current CR3 page tables (kernel mappings are present in
 * every address space), handling 4KB and 2MB leaf entries.
 */
static uint64_t vg_v2p(const void *v) {
    uint64_t va = (uint64_t)(uintptr_t)v;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t e = ((uint64_t *)phys_to_virt(cr3 & ~0xFFFull))[(va >> 39) & 0x1FF];
    if (!(e & 1))
        return 0;
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 30) & 0x1FF];
    if (!(e & 1))
        return 0;
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 21) & 0x1FF];
    if (!(e & 1))
        return 0;
    if (e & 0x80)
        return PTE_PHYS(e) | (va & 0x1FFFFFull);
    e = ((uint64_t *)phys_to_virt(PTE_PHYS(e)))[(va >> 12) & 0x1FF];
    if (!(e & 1))
        return 0;
    return PTE_PHYS(e) | (va & 0xFFFull);
}

static int vg_alloc_vq(struct VGPU_VQ *q, uint16_t size, uint64_t *phys_out) {
    uint32_t bytes = 16 * size + 6 + 2 * size;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (mem == 0)
        return -1;
    memset(mem, 0, pages * PAGE_SIZE);
    uint64_t phys = vg_v2p(mem);
    q->desc = (volatile void *)mem;
    q->avail = (volatile void *)(mem + 16 * size);
    q->used = (volatile void *)(mem + PAGE_SIZE);
    q->size = size;
    q->last_used = 0;
    q->free_head = 0;
    q->free_count = size;
    for (uint16_t i = 0; i < size; i++)
        q->desc[i].next = (uint16_t)(i + 1);
    *phys_out = phys;
    return 0;
}

static void vg_setup_vq(uint16_t index, uint16_t qsize, uint64_t phys) {
    uint32_t qbase = 0x20;
    vg_cfg_write16(0x16, index);
    vg_cfg_write32(qbase, (uint32_t)(phys & 0xFFFFFFFFu));
    vg_cfg_write32(qbase + 4, (uint32_t)(phys >> 32));
    vg_cfg_write32(qbase + 8, (uint32_t)((phys + 16 * qsize) & 0xFFFFFFFFu));
    vg_cfg_write32(qbase + 12, (uint32_t)((phys + 16 * qsize) >> 32));
    uint32_t used_off = (16 * qsize + 6 + 2 * qsize + 0xFFF) & ~0xFFFu;
    vg_cfg_write32(qbase + 16, (uint32_t)((phys + used_off) & 0xFFFFFFFFu));
    vg_cfg_write32(qbase + 20, (uint32_t)((phys + used_off) >> 32));
    vg_cfg_write16(0x1C, 1);
}

static int vg_kick_and_wait(void) {
    volatile uint8_t *nbase =
        vg.notify + vg.notify_mult * VGPU_CTRL_VQ;
    uint16_t before = vg.ctrlq.used->idx;
    vg_mmio_write32((volatile uint8_t *)nbase, 0, VGPU_CTRL_VQ);
    for (uint32_t spins = 0; spins < 50000000u; spins++) {
        if (vg.ctrlq.used->idx != before)
            return 0;
    }
    return -1;
}

static uint8_t vg_req_stage[128];

static int vg_cmd(const void *req, uint32_t reqlen, uint32_t cmd_type) {
    struct VGPU_VQ *q = &vg.ctrlq;
    if (q->free_count < 2 || reqlen > sizeof(vg_req_stage))
        return -1;
    memcpy(vg_req_stage, req, reqlen);
    uint16_t d0 = q->free_head;
    uint16_t d1 = q->desc[d0].next;
    q->free_head = q->desc[d1].next;
    q->free_count -= 2;

    q->desc[d0].addr = vg_v2p(vg_req_stage);
    q->desc[d0].len = reqlen;
    q->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[d0].next = d1;
    (void)cmd_type;

    static struct VGPU_CTRL_RESP resp;
    q->desc[d1].addr = vg_v2p(&resp);
    q->desc[d1].len = sizeof(resp);
    q->desc[d1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d1].next = 0xFFFFu;

    uint16_t avail_idx = q->avail->idx;
    q->avail->ring[avail_idx % q->size] = d0;
    __asm__ volatile("sfence" ::: "memory");
    q->avail->idx = (uint16_t)(avail_idx + 1);
    q->last_used = q->used->idx;

    int rc = vg_kick_and_wait();
    uint32_t got = q->used->ring[q->last_used % q->size].idx;
    (void)got;
    q->last_used++;

    q->desc[d1].next = q->free_head;
    __asm__ volatile("sfence" ::: "memory");
    q->free_head = d0;
    q->free_count += 2;

    if (rc != 0)
        return -1;
    return (resp.hdr.type == VGPU_RESP_OK_NODATA) ? 0 : -1;
}

static uint32_t vg_next_resource(void) {
    return ++vg.next_resource;
}

static int vg_probe(void) {
    uint8_t bus = 0, dev = 0;
    if (!pci_find_device(VIRTIO_GPU_VENDOR, VIRTIO_GPU_DEVICE, &bus, &dev)) {
        return -1;
    }
    pci_enable_bus_master(bus, dev, 0x07);
    if (vg_parse_caps(bus, dev) != 0) {
        return -1;
    }
    return 0;
}

static int vg_init(uint32_t *w, uint32_t *h, uint32_t bpp) {
    (void)bpp;
    if (vg.ready)
        return 0;

    vg_cfg_write8(0x14, 0);
    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK);
    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    uint64_t want = VIRTIO_F_VERSION_1;
    vg_mmio_write32(vg.common, 0x00, (uint32_t)(want >> 32));
    uint32_t hi = vg_mmio_read32(vg.common, 0x04);
    vg_mmio_write32(vg.common, 0x00, 0);
    uint32_t lo = vg_mmio_read32(vg.common, 0x04);
    if (!(hi & (uint32_t)(want >> 32))) {
        return -1;
    }
    vg_mmio_write32(vg.common, 0x08, (uint32_t)(want >> 32));
    vg_mmio_write32(vg.common, 0x0C, hi);
    vg_mmio_write32(vg.common, 0x08, 0);
    vg_mmio_write32(vg.common, 0x0C, lo);

    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK);
    if (!(vg_cfg_read32(0x14) & 0xFF & VIRTIO_STATUS_FEATURES_OK)) {
        return -1;
    }

    vg_cfg_write16(0x16, VGPU_CTRL_VQ);
    uint16_t qsize = vg_cfg_read32(0x18) & 0xFFFF;
    if (qsize == 0 || qsize > 256)
        qsize = 128;
    vg.queue_size = qsize;
    if (vg_alloc_vq(&vg.ctrlq, qsize, &vg.qdesc_phys) != 0) {
        return -1;
    }
    vg_setup_vq(VGPU_CTRL_VQ, qsize, vg.qdesc_phys);

    vg_cfg_write16(0x16, VGPU_CURSOR_VQ);
    uint16_t cqsize = vg_cfg_read32(0x18) & 0xFFFF;
    if (cqsize > 0) {
        if (cqsize > 64)
            cqsize = 64;
        if (vg_alloc_vq(&vg.cursorq, cqsize, &vg.cursor_qdesc_phys) == 0)
            vg_setup_vq(VGPU_CURSOR_VQ, cqsize, vg.cursor_qdesc_phys);
    }
    vg_cfg_write16(0x16, VGPU_CTRL_VQ);

    vg_cfg_write8(0x14, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK |
                             VIRTIO_STATUS_DRIVER_OK);
    vg.ready = 1;
    return 0;
}

struct VGPU_FB {
    uint64_t rid;
    uint8_t *mem;
    uint32_t w, h, size;
};

static struct VGPU_FB vfb;

static int vg_alloc_buffer(uint32_t w, uint32_t h, uint32_t bpp,
                           struct GUI_UDI_BUFFER *out) {
    if (vg.ready == 0) {
        return -1;
    }
    uint32_t size = w * h * (bpp / 8);
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (mem == 0) {
        return -1;
    }
    memset(mem, 0, pages * PAGE_SIZE);

    uint32_t rid = vg_next_resource();
    struct VGPU_RESOURCE_CREATE_2D create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VGPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = rid;
    create.format = VGPU_FORMAT_B8G8R8A8_UNORM;
    create.width = w;
    create.height = h;
    if (vg_cmd(&create, sizeof(create), create.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    struct {
        struct VGPU_ATTACH_BACKING a;
        struct VGPU_MEM_ENTRY e;
    } attach;
    memset(&attach, 0, sizeof(attach));
    attach.a.hdr.type = VGPU_CMD_ATTACH_BACKING;
    attach.a.resource_id = rid;
    attach.a.nr_entries = 1;
    attach.e.addr = vg_v2p(mem);
    attach.e.length = pages * PAGE_SIZE;
    if (vg_cmd(&attach, sizeof(attach), attach.a.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    struct VGPU_SET_SCANOUT ss;
    memset(&ss, 0, sizeof(ss));
    ss.hdr.type = VGPU_CMD_SET_SCANOUT;
    ss.r.w = w;
    ss.r.h = h;
    ss.scanout_id = 0;
    ss.resource_id = rid;
    if (vg_cmd(&ss, sizeof(ss), ss.hdr.type) != 0) {
        free_kernel_page((uint32_t)mem);
        return -1;
    }

    vfb.rid = rid;
    vfb.mem = mem;
    vfb.w = w;
    vfb.h = h;
    vfb.size = size;

    out->handle = rid;
    out->vmem = mem;
    out->w = w;
    out->h = h;
    out->bpp = bpp;
    out->size = size;
    return 0;
}

static void vg_free_buffer(uint64_t handle) {
    struct VGPU_RESOURCE_UNREF un;
    memset(&un, 0, sizeof(un));
    un.hdr.type = VGPU_CMD_RESOURCE_UNREF;
    un.resource_id = (uint32_t)handle;
    vg_cmd(&un, sizeof(un), un.hdr.type);
}

static int vg_commit_cmds(struct GFX_RECT *rects, int n);
static int vg_commit(uint64_t handle, struct GFX_RECT *rects, int n) {
    (void)handle;
    if (vfb.rid == 0)
        return -1;
    int rc = vg_commit_cmds(rects, n);
    if (rc != 0) {
        static int once;
        if (!once) {
            once = 1;
            kprintf("virtio-gpu: commit failed\n");
        }
    }
    return rc;
}

static int vg_commit_cmds(struct GFX_RECT *rects, int n) {
    int count = (n > 0) ? n : 0;

    if (count > 4) {
        struct VGPU_TRANSFER_TO_HOST_2D t;
        memset(&t, 0, sizeof(t));
        t.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_2D;
        t.r.w = vfb.w;
        t.r.h = vfb.h;
        t.offset = 0;
        t.resource_id = vfb.rid;
        if (vg_cmd(&t, sizeof(t), t.hdr.type) != 0)
            return -1;
    } else {
        for (int i = 0; i < count; i++) {
            struct GFX_RECT *r = &rects[i];
            struct VGPU_TRANSFER_TO_HOST_2D t;
            memset(&t, 0, sizeof(t));
            t.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_2D;
            t.r.x = r->x;
            t.r.y = r->y;
            t.r.w = r->w;
            t.r.h = r->h;
            t.offset = (uint64_t)r->y * (uint64_t)vfb.w * 4u +
                       (uint64_t)r->x * 4u;
            t.resource_id = vfb.rid;
            if (vg_cmd(&t, sizeof(t), t.hdr.type) != 0)
                return -1;
        }
    }
    struct VGPU_RESOURCE_FLUSH f;
    memset(&f, 0, sizeof(f));
    f.hdr.type = VGPU_CMD_RESOURCE_FLUSH;
    f.r.w = vfb.w;
    f.r.h = vfb.h;
    f.resource_id = vfb.rid;
    return vg_cmd(&f, sizeof(f), f.hdr.type);
}

static void vg_wait_vblank(void) {
    mtime_sleep(16);
}

static struct {
    uint64_t rid;
    uint8_t *mem;
    uint32_t x, y;
    int ready;
} vcur;

static uint8_t vg_cur_stage[64];

static void vg_cursor_fail(const char *stage) {
    static int once;
    if (once)
        return;
    once = 1;
    kprintf("cursor: %s failed\n", stage);
}

static int vg_cursor_cmd(const void *req, uint32_t reqlen) {
    struct VGPU_VQ *q = &vg.cursorq;
    if (q->size == 0 || q->free_count < 1 || reqlen > sizeof(vg_cur_stage))
        return -1;
    memcpy(vg_cur_stage, req, reqlen);
    uint16_t d0 = q->free_head;
    q->free_head = q->desc[d0].next;
    q->free_count--;
    q->desc[d0].addr = vg_v2p(vg_cur_stage);
    q->desc[d0].len = reqlen;
    q->desc[d0].flags = 0;
    q->desc[d0].next = 0;
    uint16_t avail_idx = q->avail->idx;
    q->avail->ring[avail_idx % q->size] = d0;
    __asm__ volatile("sfence" ::: "memory");
    q->avail->idx = (uint16_t)(avail_idx + 1);
    uint16_t before = q->used->idx;
    vg_mmio_write32(vg.notify + vg.notify_mult * VGPU_CURSOR_VQ, 0,
                    VGPU_CURSOR_VQ);
    int rc = -1;
    for (uint32_t spins = 0; spins < 20000000u; spins++) {
        if (q->used->idx != before) {
            rc = 0;
            break;
        }
    }
    q->desc[d0].next = q->free_head;
    __asm__ volatile("sfence" ::: "memory");
    q->free_head = d0;
    q->free_count++;
    return rc;
}

static int vg_cursor_set(int w, int h, const void *argb, int hot_x, int hot_y) {
    if (vg.ready == 0 || argb == 0 || w <= 0 || h <= 0 ||
        w > VGPU_CURSOR_DIM || h > VGPU_CURSOR_DIM) {
        vg_cursor_fail("arg");
        return -1;
    }
    if (vcur.rid == 0) {
        uint32_t bytes = VGPU_CURSOR_DIM * VGPU_CURSOR_DIM * 4u;
        uint8_t *mem = (uint8_t *)get_kernel_pages(
            (bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        if (mem == 0)
            return -1;
        memset(mem, 0, (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        uint32_t rid = vg_next_resource();
        struct VGPU_RESOURCE_CREATE_2D create;
        memset(&create, 0, sizeof(create));
        create.hdr.type = VGPU_CMD_RESOURCE_CREATE_2D;
        create.resource_id = rid;
        create.format = VGPU_FORMAT_B8G8R8A8_UNORM;
        create.width = VGPU_CURSOR_DIM;
        create.height = VGPU_CURSOR_DIM;
        if (vg_cmd(&create, sizeof(create), create.hdr.type) != 0) {
            free_kernel_page((uint32_t)mem);
            vg_cursor_fail("create");
            return -1;
        }
        struct {
            struct VGPU_ATTACH_BACKING a;
            struct VGPU_MEM_ENTRY e;
        } attach;
        memset(&attach, 0, sizeof(attach));
        attach.a.hdr.type = VGPU_CMD_ATTACH_BACKING;
        attach.a.resource_id = rid;
        attach.a.nr_entries = 1;
        attach.e.addr = vg_v2p(mem);
        attach.e.length = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (vg_cmd(&attach, sizeof(attach), attach.a.hdr.type) != 0) {
            free_kernel_page((uint32_t)mem);
            vg_cursor_fail("attach");
            return -1;
        }
        vcur.rid = rid;
        vcur.mem = mem;
        vcur.ready = 0;
    }
    for (uint32_t y = 0; y < VGPU_CURSOR_DIM; y++) {
        uint32_t *row =
            (uint32_t *)(void *)(vcur.mem + (size_t)y * VGPU_CURSOR_DIM * 4u);
        if (y < (uint32_t)h) {
            const uint32_t *src =
                (const uint32_t *)(const void *)argb + (size_t)y * (size_t)w;
            for (uint32_t x = 0; x < VGPU_CURSOR_DIM; x++)
                row[x] = (x < (uint32_t)w) ? src[x] : 0;
        } else {
            for (uint32_t x = 0; x < VGPU_CURSOR_DIM; x++)
                row[x] = 0;
        }
    }
    struct VGPU_TRANSFER_TO_HOST_2D t;
    memset(&t, 0, sizeof(t));
    t.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_2D;
    t.r.w = VGPU_CURSOR_DIM;
    t.r.h = VGPU_CURSOR_DIM;
    t.offset = 0;
    t.resource_id = (uint32_t)vcur.rid;
    if (vg_cmd(&t, sizeof(t), t.hdr.type) != 0) {
        vg_cursor_fail("transfer");
        return -1;
    }
    struct VGPU_UPDATE_CURSOR uc;
    memset(&uc, 0, sizeof(uc));
    uc.hdr.type = VGPU_CMD_UPDATE_CURSOR;
    uc.pos.scanout_id = 0;
    uc.pos.x = vcur.x;
    uc.pos.y = vcur.y;
    uc.resource_id = (uint32_t)vcur.rid;
    uc.hot_x = (uint32_t)(hot_x < 0 ? 0 : hot_x);
    uc.hot_y = (uint32_t)(hot_y < 0 ? 0 : hot_y);
    if (vg_cursor_cmd(&uc, sizeof(uc)) != 0) {
        vg_cursor_fail("update");
        return -1;
    }
    vcur.ready = 1;
    return 0;
}

static int vg_cursor_move(int x, int y) {
    if (vg.ready == 0 || vcur.rid == 0 || vcur.ready == 0)
        return -1;
    vcur.x = (uint32_t)(x < 0 ? 0 : x);
    vcur.y = (uint32_t)(y < 0 ? 0 : y);
    struct VGPU_UPDATE_CURSOR mc;
    memset(&mc, 0, sizeof(mc));
    mc.hdr.type = VGPU_CMD_MOVE_CURSOR;
    mc.pos.scanout_id = 0;
    mc.pos.x = vcur.x;
    mc.pos.y = vcur.y;
    mc.resource_id = (uint32_t)vcur.rid;
    int rc = vg_cursor_cmd(&mc, sizeof(mc));
    if (rc != 0)
        vg_cursor_fail("move");
    return rc;
}

struct GUI_UDI_OPS udi_virtio_ops = {
    "virtio-gpu", vg_probe,  vg_init,       vg_alloc_buffer,
    vg_free_buffer, vg_commit, vg_wait_vblank,
    vg_cursor_set, vg_cursor_move,
};
