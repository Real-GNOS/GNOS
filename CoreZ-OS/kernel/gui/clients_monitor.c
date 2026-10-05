#include "kernel/gui/clients_internal.h"

static void clock_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);

    uint32_t secs = tick / 100;
    uint32_t hh = (secs / 3600) % 24;
    uint32_t mm = (secs / 60) % 60;
    uint32_t ss = secs % 60;
    char tbuf[12];
    tbuf[0] = (char)('0' + hh / 10);
    tbuf[1] = (char)('0' + hh % 10);
    tbuf[2] = ':';
    tbuf[3] = (char)('0' + mm / 10);
    tbuf[4] = (char)('0' + mm % 10);
    tbuf[5] = ':';
    tbuf[6] = (char)('0' + ss / 10);
    tbuf[7] = (char)('0' + ss % 10);
    tbuf[8] = 0;

    int px = dc->h / 5;
    if (px < 16)
        px = 16;
    if (px > 96)
        px = 96;
    int tw = font_text_width(tbuf, px);
    int th = font_line_height(px);
    int x = (dc->w - tw) / 2;
    int y = (dc->h - th) / 2;
    font_draw(&cv, x, y, tbuf, px, theme()->accent);

    font_draw(&cv, 8, 8, "frame-callback driven clock", UI_FONT_PX,
              theme()->muted);
    font_draw(&cv, 8, dc->h - font_line_height(UI_FONT_PX) - 6,
              "uptime since boot", UI_FONT_PX, theme()->muted);
}

static const struct CLIENT_DESC clock_desc = {"clock", "clock",
                                               clock_render, 0, 33};

void clients_clock_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
    memset(&dc, 0, sizeof(dc));
    if (client_begin(&dc, &clock_desc) != 0) {
        thread_exit_current();
        return;
    }
    client_main(&dc);
}

static void sysmon_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_fill(&cv, 0, 0, dc->w, dc->h, theme()->content);

    font_draw(&cv, 8, 8, "sysmon - compositor stats", UI_FONT_PX, theme()->text);

    int nsurf = 0;
    comp_surfaces(&nsurf);
    char line[64];
    char num[12];

    line[0] = 0;
    strcat(line, "surfaces: ");
    u32_to_dec((uint32_t)nsurf, num);
    strcat(line, num);
    font_draw(&cv, 8, 30, line, UI_FONT_PX, theme()->muted);

    line[0] = 0;
    strcat(line, "workspace: ");
    u32_to_dec((uint32_t)(wm_current_ws() + 1), num);
    strcat(line, num);
    strcat(line, " / 4");
    font_draw(&cv, 8, 48, line, UI_FONT_PX, theme()->muted);

    line[0] = 0;
    strcat(line, "tick: ");
    u32_to_dec(tick, num);
    strcat(line, num);
    font_draw(&cv, 8, 66, line, UI_FONT_PX, theme()->muted);

    int gx = 8, gy = 90, gw = dc->w - 16, gh = dc->h - 104;
    if (gw > 8 && gh > 8) {
        gfx_rect(&cv, gx, gy, gw, gh, theme()->dim);
        int bars = (gw - 4) / 6;
        for (int i = 0; i < bars; i++) {
            uint32_t v = (tick / 4 + (uint32_t)i * 7) % 40;
            int bh = (int)(v * (uint32_t)(gh - 6) / 40);
            gfx_color col = GFX_RGB(40 + (int)(v % 4) * 30,
                                    120 + (int)(v % 4) * 30, 220);
            gfx_fill(&cv, gx + 2 + i * 6, gy + gh - 2 - bh, 4, bh, col);
        }
    }
}

static const struct CLIENT_DESC sysmon_desc = {"sysmon", "sysmon",
                                                sysmon_render, 0, 30};

void clients_sysmon_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
    memset(&dc, 0, sizeof(dc));
    if (client_begin(&dc, &sysmon_desc) != 0) {
        thread_exit_current();
        return;
    }
    client_main(&dc);
}

static void plasma_render(struct COMP_DEMO_CLIENT *dc) {
    struct GFX_CANVAS cv;
    canvas_of(&cv, dc);
    gfx_color *buf = cv.pixels;
    int phase = (int)(tick / 3);
    for (int y = 0; y < dc->h; y += 2) {
        gfx_color *row0 = buf + (size_t)y * (size_t)dc->w;
        gfx_color *row1 = (y + 1 < dc->h)
                              ? buf + (size_t)(y + 1) * (size_t)dc->w
                              : 0;
        for (int x = 0; x < dc->w; x += 2) {
            int rr = ((x + phase) / 6) & 0xFF;
            int gg = ((y + phase) / 5) & 0xFF;
            int bb = ((x + y + phase) / 4) & 0xFF;
            gfx_color col = GFX_RGB(rr, gg, bb);
            row0[x] = col;
            if (x + 1 < dc->w)
                row0[x + 1] = col;
            if (row1) {
                row1[x] = col;
                if (x + 1 < dc->w)
                    row1[x + 1] = col;
            }
        }
    }
    font_draw(&cv, 8, 8, "plasma - shm client rendering", UI_FONT_PX,
              GFX_RGB(255, 255, 255));
}

static const struct CLIENT_DESC plasma_desc = {"plasma", "plasma",
                                                plasma_render, 0, 20};

void clients_plasma_thread(void *arg) {
    (void)arg;
    struct COMP_DEMO_CLIENT dc;
    memset(&dc, 0, sizeof(dc));
    if (client_begin(&dc, &plasma_desc) != 0) {
        thread_exit_current();
        return;
    }
    client_main(&dc);
}
