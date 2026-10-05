#include "kernel/gui/display.h"

#include "drivers/char/console/io.h"
#include "kernel/gui/udi.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"

static struct GFX_CANVAS back;
static uint64_t back_handle;
static int back_ok;

static struct GUI_DISPLAY_OPS *active;

void display_register(struct GUI_DISPLAY_OPS *ops) {
    active = ops;
}

struct GUI_DISPLAY_OPS *display_get(void) {
    return active;
}

#define DISP_TGT_W 1024
#define DISP_TGT_H 768

static int udi_disp_init(void) {
    uint32_t w = DISP_TGT_W;
    uint32_t h = DISP_TGT_H;
    if (udi_init(&w, &h, 32) != 0)
        return -1;
    struct GUI_UDI_OPS *ops = udi_active();
    struct GUI_UDI_BUFFER buf;
    if (ops->alloc_buffer(w, h, 32, &buf) != 0)
        return -1;
    back.pixels = buf.vmem;
    back.pitch = (int)(w * 4);
    back.w = w;
    back.h = h;
    back.bytes = buf.size;
    back_handle = buf.handle;
    back_ok = 1;
    io_init(buf.vmem, (int)w, (int)h, (uint32_t)buf.size, (int)(w * 4), 32);
    kprintf("display: udi framebuffer %ux%u\n", w, h);
    return 0;
}

static struct GFX_CANVAS *udi_disp_surface(int which) {
    (void)which;
    return &back;
}

static int udi_disp_flip(struct GFX_RECT *rects, int n) {
    struct GUI_UDI_OPS *ops = udi_active();
    if (ops == 0 || back_ok == 0)
        return -1;
    return ops->commit(back_handle, rects, n);
}

static void udi_disp_wait_vblank(void) {
    struct GUI_UDI_OPS *ops = udi_active();
    if (ops && ops->wait_vblank)
        ops->wait_vblank();
}

static int udi_disp_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    (void)w;
    (void)h;
    (void)bpp;
    return -1;
}

static int udi_disp_cursor_set(int w, int h, const void *argb, int hot_x,
                               int hot_y) {
    return udi_cursor_set(w, h, argb, hot_x, hot_y);
}

static int udi_disp_cursor_move(int x, int y) {
    return udi_cursor_move(x, y);
}

struct GUI_DISPLAY_OPS udi_display_ops = {
    "udi", udi_disp_init, udi_disp_surface, udi_disp_flip,
    udi_disp_wait_vblank, udi_disp_set_mode,
    udi_disp_cursor_set, udi_disp_cursor_move,
};

void display_init(void) {
    display_register(&udi_display_ops);
}
