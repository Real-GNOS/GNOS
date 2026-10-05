#include "kernel/gui/udi.h"

#include "drivers/char/console/io.h"
#include "kernel/gui/vgpu_pci.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"

extern struct GUI_UDI_OPS udi_virtio_ops;
extern struct GUI_UDI_OPS udi_vmware_ops;

#define UDI_MAX_BACKENDS 4

static struct GUI_UDI_OPS *backends[UDI_MAX_BACKENDS];
static int backend_count;
static struct GUI_UDI_OPS *active;

void udi_register(struct GUI_UDI_OPS *ops) {
    if (ops == 0 || backend_count >= UDI_MAX_BACKENDS)
        return;
    backends[backend_count++] = ops;
}

struct GUI_UDI_OPS *udi_active(void) {
    return active;
}

int udi_init(uint32_t *w, uint32_t *h, uint32_t bpp) {
    if (active)
        return 0;

    extern struct GUI_UDI_OPS udi_sw_ops;
    udi_register(&udi_virtio_ops);
    udi_register(&udi_vmware_ops);
    udi_register(&udi_sw_ops);

    for (int i = 0; i < backend_count; i++) {
        struct GUI_UDI_OPS *ops = backends[i];
        if (ops->probe && ops->probe() != 0)
            continue;
        if (ops->init(w, h, bpp) != 0)
            continue;
        active = ops;
        return 0;
    }
    return -1;
}

static struct GFX_CANVAS sw_front;
static struct GFX_CANVAS sw_back;
static uint64_t sw_handle;

static int sw_probe(void) {
    return (io_get_vram() != 0) ? 0 : -1;
}

static int sw_init(uint32_t *w, uint32_t *h, uint32_t bpp) {
    *w = (uint32_t)io_get_scrnx();
    *h = (uint32_t)io_get_scrny();
    uint32_t wloc = *w;
    uint32_t hloc = *h;
    int pitch = io_get_pitch();
    if (pitch < (int)(wloc * 4))
        pitch = (int)(wloc * 4);
    sw_front.pixels = (gfx_color *)io_get_vram();
    sw_front.pitch = pitch;
    sw_front.w = wloc;
    sw_front.h = hloc;
    sw_front.bytes = io_get_vram_bytes();

    size_t bsz = (size_t)wloc * (size_t)hloc * 4u;
    uint8_t *bp = (uint8_t *)get_kernel_pages(
        (uint32_t)((bsz + (size_t)PAGE_SIZE - 1) / (size_t)PAGE_SIZE));
    if (bp == 0)
        return -1;
    sw_back.pixels = (gfx_color *)bp;
    sw_back.pitch = (int)(wloc * 4);
    sw_back.w = wloc;
    sw_back.h = hloc;
    sw_back.bytes = bsz;
    sw_handle = 1;
    return 0;
}

static int sw_alloc_buffer(uint32_t w, uint32_t h, uint32_t bpp,
                           struct GUI_UDI_BUFFER *out) {
    out->handle = sw_handle;
    out->vmem = sw_back.pixels;
    out->w = w;
    out->h = h;
    out->bpp = bpp;
    out->size = w * h * (bpp / 8);
    return 0;
}

static void sw_free_buffer(uint64_t handle) {
    (void)handle;
}

static int sw_commit(uint64_t handle, struct GFX_RECT *rects, int n) {
    (void)handle;
    if (n <= 0)
        return 0;
    if (n > 4) {
        int x0 = rects[0].x;
        int y0 = rects[0].y;
        int x1 = rects[0].x + rects[0].w;
        int y1 = rects[0].y + rects[0].h;
        uint64_t area = 0;
        for (int i = 0; i < n; i++) {
            area += (uint64_t)rects[i].w * (uint64_t)rects[i].h;
            if (rects[i].x < x0)
                x0 = rects[i].x;
            if (rects[i].y < y0)
                y0 = rects[i].y;
            if (rects[i].x + rects[i].w > x1)
                x1 = rects[i].x + rects[i].w;
            if (rects[i].y + rects[i].h > y1)
                y1 = rects[i].y + rects[i].h;
        }
        uint64_t uarea = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
        if (area * 2 >= uarea * 3) {
            gfx_present(&sw_front, x0, y0, &sw_back, x0, y0, x1 - x0,
                        y1 - y0);
            return 0;
        }
    }
    for (int i = 0; i < n; i++) {
        struct GFX_RECT *r = &rects[i];
        gfx_present(&sw_front, r->x, r->y, &sw_back, r->x, r->y, r->w, r->h);
    }
    return 0;
}

static void sw_wait_vblank(void) {
    mtime_sleep(16);
}

int udi_cursor_set(int w, int h, const void *argb, int hot_x, int hot_y) {
    struct GUI_UDI_OPS *ops = udi_active();
    if (ops == 0 || ops->cursor_set == 0)
        return -1;
    return ops->cursor_set(w, h, argb, hot_x, hot_y);
}

int udi_cursor_move(int x, int y) {
    struct GUI_UDI_OPS *ops = udi_active();
    if (ops == 0 || ops->cursor_move == 0)
        return -1;
    return ops->cursor_move(x, y);
}

struct GUI_UDI_OPS udi_sw_ops = {
    "software", sw_probe,  sw_init,      sw_alloc_buffer,
    sw_free_buffer, sw_commit, sw_wait_vblank,
};
