#include "kernel/gui/clients_internal.h"

#define VIEWER_MAX_FILES 3
static const char *viewer_files[VIEWER_MAX_FILES] = {"/wallpaper.png",
                                                     "/pic1.png",
                                                     "/pic2.png"};

struct PNG_VIEWER {
    struct COMP_DEMO_CLIENT dc;
    struct PNG_IMAGE img;
    int idx;
};

static int viewer_load(struct PNG_VIEWER *v, int idx) {
    const char *path = viewer_files[idx];
    struct FS_STAT st;
    if (sys_stat(path, &st) != 0 || st.st_size == 0 ||
        st.st_size > (8u * 1024u * 1024u))
        return -1;
    int fd = open_file(path, O_RDONLY);
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
    if (got != st.st_size) {
        free_kernel_page((uint32_t)(uintptr_t)buf);
        return -1;
    }
    if (v->img.pixels)
        png_image_free(&v->img);
    int rc = png_decode(buf, got, &v->img);
    free_kernel_page((uint32_t)(uintptr_t)buf);
    if (rc != PNG_OK)
        return -1;
    v->idx = idx;
    return 0;
}

static void png_viewer_render(struct COMP_DEMO_CLIENT *dc) {
    struct PNG_VIEWER *v = (struct PNG_VIEWER *)dc;
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);
    int footer = 20;
    int avail = dc->h - footer;
    if (avail < 8)
        return;
    if (v->img.pixels && v->img.w > 0 && v->img.h > 0) {
        struct GFX_CANVAS src;
        src.pixels = v->img.pixels;
        src.pitch = v->img.w * 4;
        src.w = v->img.w;
        src.h = v->img.h;
        src.bytes = (size_t)v->img.w * (size_t)v->img.h * 4u;
        int dw = dc->w, dh = avail;
        if ((int64_t)src.w * dh > (int64_t)src.h * dw)
            dh = (int)((int64_t)src.h * dw / src.w);
        else
            dw = (int)((int64_t)src.w * dh / src.h);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        gfx_blit_scale(&cv, (dc->w - dw) / 2, (avail - dh) / 2, dw, dh, &src,
                       0, 0, src.w, src.h);
    } else {
        font_draw(&cv, 8, 8, "png view: no image", UI_FONT_PX,
                  theme()->muted);
    }
    char line[96];
    char num[12];
    const char *name = viewer_files[v->idx];
    while (*name == '/')
        name++;
    line[0] = 0;
    strcat(line, name);
    strcat(line, "  ");
    u32_to_dec((uint32_t)v->img.w, num);
    strcat(line, num);
    strcat(line, "x");
    u32_to_dec((uint32_t)v->img.h, num);
    strcat(line, num);
    strcat(line, "  [");
    u32_to_dec((uint32_t)(v->idx + 1), num);
    strcat(line, num);
    strcat(line, "/");
    u32_to_dec((uint32_t)VIEWER_MAX_FILES, num);
    strcat(line, num);
    strcat(line, "]  space/enter: next");
    gfx_fill(&cv, 0, avail, dc->w, footer, theme()->bar);
    font_draw(&cv, 6, avail + 4, line, UI_FONT_PX, theme()->text);
}

static void png_viewer_on_key(struct COMP_DEMO_CLIENT *dc, int scancode,
                              int mods) {
    struct PNG_VIEWER *v = (struct PNG_VIEWER *)dc;
    (void)mods;
    int next = v->idx;
    if (scancode == KBD_SC_SPACE || scancode == KBD_SC_ENTER || scancode == KBD_SC_RIGHT)
        next = (v->idx + 1) % VIEWER_MAX_FILES;
    else if (scancode == KBD_SC_LEFT)
        next = (v->idx + VIEWER_MAX_FILES - 1) % VIEWER_MAX_FILES;
    else
        return;
    if (viewer_load(v, next) != 0) {
        comp_log("pngview: load failed");
        return;
    }
    if (dc->pool) {
        client_repaint(dc);
    }
}

static const struct CLIENT_DESC png_desc = {"pngview", "pngview",
                                             png_viewer_render,
                                             png_viewer_on_key, 1000000};

void clients_png_thread(void *arg) {
    (void)arg;
    struct PNG_VIEWER viewer;
    struct PNG_VIEWER *v = &viewer;
    memset(v, 0, sizeof(*v));
    if (viewer_load(v, 0) != 0)
        comp_log("pngview: initial image load failed");
    else
        comp_log("pngview: image decoded");
    if (client_begin(&v->dc, &png_desc) != 0) {
        if (v->img.pixels)
            png_image_free(&v->img);
        thread_exit_current();
        return;
    }
    client_main(&v->dc);
    if (v->img.pixels)
        png_image_free(&v->img);
}
