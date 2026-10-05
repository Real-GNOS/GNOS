#include "kernel/gui/wm_internal.h"

#include "drivers/char/rtc.h"

static int bar_dirty = 1;
static uint32_t bar_clock = 0;
static int hover_btn = -1;
static int hover_pill = -1;

void wm_bar_reset(void) {
    bar_dirty = 1;
    bar_clock = 0;
    hover_btn = -1;
    hover_pill = -1;
}

void wm_bar_invalidate(void) {
    bar_dirty = 1;
}

int wm_bar_check_dirty(void) {
    comp_lock_acquire();
    if (tick / PIT_HZ != bar_clock) {
        bar_clock = tick / PIT_HZ;
        bar_dirty = 1;
    }
    int d = bar_dirty;
    bar_dirty = 0;
    comp_lock_release();
    return d;
}

static int taskbar_btn_w(struct WL_SURFACE *s) {
    int w = font_text_width(s->title, BAR_FONT_PX) + 22;
    if (w < TASKBAR_BTN_MIN)
        w = TASKBAR_BTN_MIN;
    if (w > TASKBAR_BTN_MAXW)
        w = TASKBAR_BTN_MAXW;
    return w;
}

static int taskbar_pills_x(void) {
    return comp_screen_w() - 8 - (WL_MAX_WS * (BAR_PILL_W + 4) - 4);
}

static int taskbar_layout(int *xs, int *bws, int max) {
    struct WL_SURFACE *list[WL_MAX_SURFACES];
    int wn = wm_collect_visible(list, WL_MAX_SURFACES);
    int limit =
        taskbar_pills_x() - 12 - font_text_width("00:00", BAR_FONT_PX) - 8;
    int x = 8;
    int n = 0;
    for (int i = 0; i < wn && n < max; i++) {
        int bw = taskbar_btn_w(list[i]);
        if (x + bw > limit)
            break;
        xs[n] = x;
        bws[n] = bw;
        x += bw + 4;
        n++;
    }
    return n;
}

static int taskbar_hover_btn(int x) {
    int xs[WL_MAX_SURFACES];
    int bws[WL_MAX_SURFACES];
    int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
    for (int i = 0; i < n; i++)
        if (x >= xs[i] && x < xs[i] + bws[i])
            return i;
    return -1;
}

static int taskbar_hover_pill(int x) {
    int px = taskbar_pills_x();
    for (int i = 0; i < WL_MAX_WS; i++) {
        int bx = px + i * (BAR_PILL_W + 4);
        if (x >= bx && x < bx + BAR_PILL_W)
            return i;
    }
    return -1;
}

void wm_bar_hover(int x, int y) {
    int nbtn = -1;
    int npill = -1;
    if (y >= TASKBAR_TOP) {
        npill = taskbar_hover_pill(x);
        nbtn = taskbar_hover_btn(x);
    }
    if (nbtn != hover_btn || npill != hover_pill) {
        hover_btn = nbtn;
        hover_pill = npill;
        wm_bar_invalidate();
    }
}

void wm_bar_click(int x, int y) {
    if (y < TASKBAR_TOP + 1 || y >= comp_screen_h() - 1)
        return;
    int px = taskbar_pills_x();
    for (int i = 0; i < WL_MAX_WS; i++) {
        int bx = px + i * (BAR_PILL_W + 4);
        if (x >= bx && x < bx + BAR_PILL_W) {
            wm_switch_ws(i);
            return;
        }
    }
    int xs[WL_MAX_SURFACES];
    int bws[WL_MAX_SURFACES];
    int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
    for (int i = 0; i < n; i++) {
        if (x >= xs[i] && x < xs[i] + bws[i]) {
            wm_focus_index(i);
            return;
        }
    }
}

void wm_draw_bar(struct GFX_CANVAS *c, struct GFX_RECT *clip) {
    int sw = comp_screen_w();
    struct GFX_RECT bar = {0, TASKBAR_TOP, sw, COMP_BAR_H}, v;
    if (!gfx_rect_intersect(bar, *clip, &v))
        return;
    const struct GUI_THEME *t = theme();

    gfx_fill(c, v.x, v.y, v.w, v.h, t->bar);
    gfx_hline(c, 0, TASKBAR_TOP, sw, t->bar_line);

    int by = TASKBAR_TOP + 5;
    int ty = by + (TASKBAR_H - font_ascent(BAR_FONT_PX)) / 2;
    struct WL_SURFACE *f = wm_focused_surface();
    struct WL_SURFACE *list[WL_MAX_SURFACES];
    int wn = wm_collect_visible(list, WL_MAX_SURFACES);

    int xs[WL_MAX_SURFACES], bws[WL_MAX_SURFACES];
    int n = taskbar_layout(xs, bws, WL_MAX_SURFACES);
    for (int i = 0; i < n && i < wn; i++) {
        struct WL_SURFACE *s = list[i];
        int focused = (s == f);
        gfx_fill_round(c, xs[i], by, bws[i], TASKBAR_H, 6,
                       focused ? t->accent
                               : (i == hover_btn ? t->frame_foc
                                                 : t->frame_unf));
        struct GFX_RECT btn = {xs[i], by, bws[i], TASKBAR_H}, bv;
        if (!gfx_rect_intersect(btn, v, &bv))
            continue;
        font_draw_clip(c, xs[i] + 9, ty, s->title, BAR_FONT_PX,
                       focused ? t->title_fg_foc : t->text, &bv);
    }

    int px = taskbar_pills_x();
    int cur_ws = wm_current_ws();
    for (int i = 0; i < WL_MAX_WS; i++) {
        int cur = (i == cur_ws);
        int occ = wm_ws_occupied(i);
        int bx = px + i * (BAR_PILL_W + 4);
        gfx_color pill = cur ? t->accent
                             : (i == hover_pill ? t->frame_foc
                                                : (occ ? t->frame_unf
                                                       : t->dim));
        gfx_fill_round(c, bx, by, BAR_PILL_W, TASKBAR_H, 6, pill);
        char label[4] = {' ', (char)('1' + i), ' ', 0};
        gfx_color fg = cur ? t->title_fg_foc : (occ ? t->text : t->muted);
        int lw = font_text_width(label, BAR_FONT_PX);
        font_draw_clip(c, bx + (BAR_PILL_W - lw) / 2, ty, label, BAR_FONT_PX,
                       fg, &v);
    }

    uint8_t hh;
    uint8_t mm;
    uint8_t ss;
    rtc_read_time(&hh, &mm, &ss);
    char up[8];
    up[0] = (char)('0' + hh / 10);
    up[1] = (char)('0' + hh % 10);
    up[2] = ':';
    up[3] = (char)('0' + mm / 10);
    up[4] = (char)('0' + mm % 10);
    up[5] = 0;
    int rw = font_text_width(up, BAR_FONT_PX);
    font_draw_clip(c, px - 12 - rw, ty, up, BAR_FONT_PX, t->muted, &v);
    (void)ss;
}
