#include "kernel/gui/gui.h"

#include "drivers/char/keyboard.h"
#include "drivers/char/mouse.h"
#include "kernel/asm_func.h"
#include "drivers/char/console/io.h"
#include "kernel/fs/fs.h"
#include "kernel/init/pit/pit.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/gui/clients.h"
#include "kernel/gui/font.h"
#include "kernel/gui/gfx.h"
#include "kernel/gui/server.h"
#include "kernel/gui/udi.h"
#include "kernel/gui/wm.h"
#include "kernel/gui/x11.h"
#include "lib/png/png.h"

extern const unsigned char _binary_font_kernel_ttf_start[];
extern const unsigned char _binary_font_kernel_ttf_end[];
extern const unsigned char _binary_wallpaper_png_start[];
extern const unsigned char _binary_wallpaper_png_end[];
#define FONT_DISK_MAX (6u * 1024u * 1024u)
#define WALLPAPER_DISK_MAX (16u * 1024u * 1024u)
static int running = 0;
static int ready = 0;

int gui_session_ready(void) {
    return ready;
}

static void load_embedded_font(void) {
    int len = (int)(_binary_font_kernel_ttf_end -
                    _binary_font_kernel_ttf_start);
    if (len <= 0) {
        comp_log("font: embedded blob missing");
        return;
    }
    if (font_init(_binary_font_kernel_ttf_start, len))
        comp_log("font: embedded latin subset ready");
    else
        comp_log("font: embedded font init failed");
}

static void load_disk_font(void) {
    struct FS_STAT st;
    if (sys_stat("/font_subset.ttf", &st) != 0 || st.st_size == 0 ||
        st.st_size > FONT_DISK_MAX)
        return;
    int fd = open_file("/font_subset.ttf", O_RDONLY);
    if (fd < 0)
        return;
    uint32_t pages = (st.st_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = (uint8_t *)get_kernel_pages(pages);
    if (!buf) {
        close_file(fd);
        return;
    }
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    if (got < 32) {
        comp_log("font: disk font read failed, keeping embedded");
        return;
    }
    if (font_init(buf, (int)got))
        comp_log("font: disk CJK subset loaded (中文可用)");
    else
        comp_log("font: disk font invalid, keeping embedded");
}

static int apply_wallpaper_image(const void *data, uint32_t len,
                                 const char *tag) {
    struct PNG_IMAGE img;
    if (png_decode(data, len, &img) != PNG_OK)
        return -1;
    comp_set_wallpaper(img.pixels, img.w, img.h);
    kprintf("wallpaper: %s %dx%d\n", tag, img.w, img.h);
    png_image_free(&img);
    return 0;
}

static int load_disk_wallpaper(void) {
    struct FS_STAT st;
    if (sys_stat("/wallpaper.png", &st) != 0 || st.st_size == 0 ||
        st.st_size > WALLPAPER_DISK_MAX)
        return -1;
    int fd = open_file("/wallpaper.png", O_RDONLY);
    if (fd < 0)
        return -1;
    uint32_t pages = (st.st_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = (uint8_t *)get_kernel_pages(pages);
    if (!buf) {
        close_file(fd);
        return -1;
    }
    uint32_t got = read_file(fd, buf, st.st_size);
    close_file(fd);
    int rc = -1;
    if (got >= 64)
        rc = apply_wallpaper_image(buf, got, "/wallpaper.png");
    free_kernel_page((uint32_t)(uintptr_t)buf);
    return rc;
}

static void load_wallpaper(void) {
    if (load_disk_wallpaper() == 0)
        return;
    comp_log("wallpaper: disk image unavailable, using embedded");
    uint32_t len =
        (uint32_t)(_binary_wallpaper_png_end - _binary_wallpaper_png_start);
    if ((int)len < 64) {
        comp_log("wallpaper: embedded blob missing, keeping gradient");
        return;
    }
    apply_wallpaper_image(_binary_wallpaper_png_start, len, "embedded");
}

int gui_session_run(void) {
    if (running)
        return -1;
    running = 1;
    ready = 0;

    asm_sti();

    io_set_gui_active(1);
    comp_init();
    kprintf("gui: active=%s %ux%u@%ux%u\n",
            udi_active() ? udi_active()->name : "(none)",
            io_get_scrnx(), io_get_scrny(),
            (int)comp_screen_w(), (int)comp_screen_h());
    if (gfx_fb_bpp() != 32) {
        kprintf("gui: 真彩 GUI 需要 32bpp 线性帧缓冲, 当前 %d bpp\n",
                gfx_fb_bpp());
        io_set_gui_active(0);
        running = 0;
        return -1;
    }
    load_embedded_font();
    load_disk_font();
    load_wallpaper();
    wm_init_state();
    x11_gateway_init();

    keyboard_set_gui_hook(comp_post_key);
    mouse_set_hook(comp_post_mouse);

    comp_log("compositor: session started (32bpp RGBA)");
    x11_server_start();
    clients_spawn_initial();

    ready = 1;
    comp_run();
    kprintf("gui: session ended\n");
    mtime_sleep(150);

    keyboard_set_gui_hook(0);
    mouse_set_hook(0);
    io_set_gui_active(0);
    io_clear_screen();

    ready = 0;
    running = 0;
    return 0;
}
