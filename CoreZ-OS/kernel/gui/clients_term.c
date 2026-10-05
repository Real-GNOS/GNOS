#include "kernel/gui/clients_internal.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/char/console/io.h"
#include "flanterm/src/flanterm.h"
#include "flanterm/src/flanterm_backends/fb.h"

#define FT_POOL_BYTES (1u << 20)

static struct flanterm_context *ft_ctx;
static uint32_t *ft_fb;
static size_t ft_w, ft_h, ft_pitch;
static int ft_ready;
static uint8_t *ft_pool;
static size_t ft_pool_used;

static void *ft_malloc(size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (ft_pool == NULL || ft_pool_used + n > FT_POOL_BYTES)
        return NULL;
    void *p = ft_pool + ft_pool_used;
    ft_pool_used += n;
    return p;
}

static void ft_free(void *p, size_t n) {
    (void)p;
    (void)n;
}

static void term_ensure_init(int w, int h) {
    uint32_t fb_pages;
    kprintf("[ft] init enter w=%d h=%d\n", w, h);
    if (ft_ready || w < 160 || h < 80)
        return;
    ft_w = (size_t)(w < 1024 ? w : 1024);
    ft_h = (size_t)(h < 640 ? h : 640);
    ft_pitch = ft_w * 4u;
    ft_pool = get_kernel_pages(FT_POOL_BYTES / PAGE_SIZE);
    kprintf("[ft] pool=%x\n", (uint32_t)(uintptr_t)ft_pool);
    if (ft_pool == NULL)
        return;
    fb_pages = (uint32_t)((ft_pitch * ft_h + PAGE_SIZE - 1) / PAGE_SIZE);
    ft_fb = get_kernel_pages(fb_pages);
    kprintf("[ft] fb=%x pages=%u\n", (uint32_t)(uintptr_t)ft_fb, fb_pages);
    if (ft_fb == NULL) {
        ft_pool = NULL;
        ft_pool_used = 0;
        return;
    }
    static uint32_t ft_ansi[8], ft_ansi_bright[8];
    static uint32_t ft_default_bg, ft_default_fg;
    for (int i = 0; i < 8; i++) {
        ft_ansi[i] = io_ansi_color(i);
        ft_ansi_bright[i] = io_ansi_color(i + 8);
    }
    ft_default_bg = io_ansi_color(0);
    ft_default_fg = io_ansi_color(7);
    ft_ctx = flanterm_fb_init(
        ft_malloc, ft_free, ft_fb, ft_w, ft_h, ft_pitch,
        8, 16, 8, 8, 8, 0,
        NULL, ft_ansi, ft_ansi_bright, &ft_default_bg, &ft_default_fg,
        NULL, NULL,
        NULL, 0, 0, 0, 1, 1, 0, 0, true);
    kprintf("[ft] ctx=%x used=%u\n", (uint32_t)(uintptr_t)ft_ctx,
            (uint32_t)ft_pool_used);
    if (ft_ctx == NULL)
        return;
    ft_ready = 1;
    flanterm_full_refresh(ft_ctx);
    kprintf("[ft] ready\n");
}

static void term_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    static int rn;
    canvas_of(&cv, dc);
    if (rn < 3) {
        rn++;
        kprintf("[ft] render %d dc=%dx%d\n", rn, dc->w, dc->h);
    }
    term_ensure_init(dc->w, dc->h);
    if (!ft_ready) {
        gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
        return;
    }
    int vw = dc->w < (int)ft_w ? dc->w : (int)ft_w;
    int vh = dc->h < (int)ft_h ? dc->h : (int)ft_h;
    for (int y = 0; y < vh; y++) {
        uint32_t *dst = gfx_row(&cv, y);
        const uint32_t *src = (const uint32_t *)(const void *)(
            (const uint8_t *)ft_fb + (size_t)y * ft_pitch);
        for (int x = 0; x < vw; x++)
            dst[x] = src[x] | 0xFF000000u;
    }
    if (vh < dc->h)
        gfx_fill(&cv, 0, vh, dc->w, dc->h - vh, theme()->content);
    if (vw < dc->w)
        gfx_fill(&cv, vw, 0, dc->w - vw, dc->h, theme()->content);
    if (rn < 6) {
        kprintf("[ft] blit vw=%d vh=%d cv0=%x cvmid=%x fb0=%x\n", vw, vh,
                cv.pixels[0], cv.pixels[150 * 240], ft_fb[0]);
    }
}

static void term_log_hook(const char *s) {
    size_t n;
    if (!ft_ready)
        return;
    n = strlen(s);
    flanterm_write(ft_ctx, s, n);
    if (n == 0 || s[n - 1] != '\n')
        flanterm_write(ft_ctx, "\n", 1);
}

static void term_on_key(struct COMP_DEMO_CLIENT *dc, int scancode, int mods) {
    char ch;
    (void)mods;
    ch = keyboard_translate((uint8_t)scancode, mods & MOD_SHIFT);
    if (!ch || !ft_ready)
        return;
    flanterm_write(ft_ctx, &ch, 1);
    client_repaint(dc);
}

static const struct CLIENT_DESC term_desc = {
    "term", "term - wayland-ish client", term_render, term_on_key, 30};

void clients_term_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
    memset(&dc, 0, sizeof(dc));
    if (client_begin(&dc, &term_desc) != 0) {
        thread_exit_current();
        return;
    }
    log_hook = term_log_hook;
    client_main(&dc);
    log_hook = 0;
}
