#include "kernel/gui/x11.h"

#include <stddef.h>
#include <stdint.h>

#include "drivers/char/console/io.h"
#include "kernel/gui/gfx.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/gui/shm.h"
#include "kernel/gui/wm.h"
#include "lib/str/str.h"

#include "kernel/gui/x11_internal.h"

#define TRIG_ONE 1024

uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static const int16_t g_sin_q10[91] = {
    0,    17,   35,   52,   70,   87,   104,  121,  139,  156,  173,  190,
    207,  224,  241,  258,  275,  292,  309,  325,  342,  358,  374,  390,
    406,  422,  438,  453,  469,  484,  499,  514,  529,  543,  558,  572,
    586,  600,  613,  627,  640,  653,  666,  678,  691,  703,  715,  726,
    738,  749,  760,  771,  781,  791,  801,  811,  820,  829,  838,  846,
    855,  863,  870,  878,  885,  892,  898,  905,  911,  916,  922,  927,
    932,  936,  941,  945,  948,  952,  955,  958,  960,  962,  964,  966,
    967,  968,  969,  970,  970,  970,  970};
int trig_sin(int deg) {
    deg %= 360;
    if (deg < 0)
        deg += 360;
    if (deg <= 90)
        return g_sin_q10[deg];
    if (deg <= 180)
        return g_sin_q10[180 - deg];
    if (deg <= 270)
        return -g_sin_q10[deg - 180];
    return -g_sin_q10[360 - deg];
}
int trig_cos(int deg) {
    return trig_sin(deg + 90);
}

int gc_idx(struct X11_CONN *c, uint32_t gid) {
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (c->gc[i].used && c->gc[i].gid == gid)
            return i;
    return -1;
}
int pix_idx(struct X11_CONN *c, uint32_t pid) {
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (c->pix[i].used && c->pix[i].pid == pid)
            return i;
    return -1;
}

struct X11_DRAW {
    struct GFX_CANVAS cv;
    int ok;
};

struct X11_DRAW draw_get(struct X11_CONN *c, uint32_t id) {
    struct X11_DRAW d;
    memset(&d, 0, sizeof(d));
    int wi = win_idx(c, id);
    if (wi >= 0) {
        struct X11_WINDOW *w = &c->win[wi];
        if (!w->pool || w->w == 0 || w->h == 0)
            return d;
        d.cv.pixels = (gfx_color *)w->pool->data;
        d.cv.pitch = (int)(w->w * 4u);
        d.cv.w = (int)w->w;
        d.cv.h = (int)w->h;
        d.cv.bytes = (size_t)w->w * (size_t)w->h * 4u;
        d.ok = 1;
        return d;
    }
    int pi = pix_idx(c, id);
    if (pi >= 0) {
        struct X11_PIXMAP *p = &c->pix[pi];
        if (!p->data)
            return d;
        d.cv.pixels = (gfx_color *)p->data;
        d.cv.pitch = (int)(p->w * 4u);
        d.cv.w = (int)p->w;
        d.cv.h = (int)p->h;
        d.cv.bytes = p->size;
        d.ok = 1;
    }
    return d;
}
void draw_commit(struct X11_CONN *c, uint32_t id) {
    int wi = win_idx(c, id);
    if (wi >= 0 && c->win[wi].s)
        wl_surface_commit(c->win[wi].s);
}
gfx_color pix_to_color(uint32_t pixel) {
    return GFX_RGB((int)((pixel >> 16) & 0xFF), (int)((pixel >> 8) & 0xFF),
                   (int)(pixel & 0xFF));
}
int16_t clo16(int v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

void h_open_font(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t fid = rd32(p + 4);
    if (fid & 0x3)
        return post_error(c, X11_ERR_Value, X11_REQ_OpenFont, 0, fid);
    uint8_t head[32];
    reply_init(c, X11_REQ_OpenFont, 0, head);
    reply_finish(c, head, 0, 0);
}
void h_query_text_extents(struct X11_CONN *c, const uint8_t *p,
                                 uint32_t len) {
    (void)p;
    (void)len;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryTextExtents, 0, head);
    head[1] = 0;
    reply_finish(c, head, 0, 0);
}
void h_create_pixmap(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    (void)len;
    uint32_t pid = rd32(p + 4);
    uint32_t w = rd16(p + 12), h = rd16(p + 14);
    if (pix_idx(c, pid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreatePixmap, 0, pid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_PIXMAPS; i++)
        if (!c->pix[i].used) {
            slot = i;
            break;
        }
    if (slot < 0 || w == 0 || h == 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreatePixmap, 0, pid);
    uint32_t bytes = w * h * 4u;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (!mem)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreatePixmap, 0, pid);
    memset(mem, 0, pages * PAGE_SIZE);
    c->pix[slot].used = 1;
    c->pix[slot].pid = pid;
    c->pix[slot].w = w;
    c->pix[slot].h = h;
    c->pix[slot].depth = p[1];
    c->pix[slot].data = mem;
    c->pix[slot].size = bytes;
    (void)pid_next;
}
void h_free_pixmap(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t pid = rd32(p + 4);
    int i = pix_idx(c, pid);
    if (i < 0)
        return post_error(c, X11_ERR_Pixmap, X11_REQ_FreePixmap, 0, pid);
    if (c->pix[i].data)
        free_kernel_page((uint32_t)c->pix[i].data);
    memset(&c->pix[i], 0, sizeof(c->pix[i]));
}
void h_create_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_CreateGC, 0, 0);
    uint32_t gid = rd32(p + 4);
    if (gc_idx(c, gid) >= 0)
        return post_error(c, X11_ERR_Value, X11_REQ_CreateGC, 0, gid);
    int slot = -1;
    for (int i = 0; i < X11_MAX_GCS; i++)
        if (!c->gc[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return post_error(c, X11_ERR_Alloc, X11_REQ_CreateGC, 0, gid);
    memset(&c->gc[slot], 0, sizeof(c->gc[slot]));
    c->gc[slot].used = 1;
    c->gc[slot].gid = gid;
    c->gc[slot].fg = 0x000000u;
    c->gc[slot].bg = 0xFFFFFFu;
    c->gc[slot].line_width = 1;
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    for (int bit = 0; bit < 23 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        if ((1u << bit) == X11_GC_Foreground)
            c->gc[slot].fg = val;
        else if ((1u << bit) == X11_GC_Background)
            c->gc[slot].bg = val;
        else if ((1u << bit) == X11_GC_LineWidth)
            c->gc[slot].line_width = val;
    }
    (void)gid_next;
}
void h_change_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 12)
        return post_error(c, X11_ERR_Length, X11_REQ_ChangeGC, 0, 0);
    int gi = gc_idx(c, rd32(p + 4));
    if (gi < 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_ChangeGC, 0,
                          rd32(p + 4));
    uint32_t mask = rd32(p + 8);
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    for (int bit = 0; bit < 23 && remain >= 4; bit++) {
        if (!(mask & (1u << bit)))
            continue;
        uint32_t val = rd32(v);
        v += 4;
        remain -= 4;
        if ((1u << bit) == X11_GC_Foreground)
            c->gc[gi].fg = val;
        else if ((1u << bit) == X11_GC_Background)
            c->gc[gi].bg = val;
        else if ((1u << bit) == X11_GC_LineWidth)
            c->gc[gi].line_width = val;
    }
}
void h_free_gc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t gid = rd32(p + 4);
    int i = gc_idx(c, gid);
    if (i < 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_FreeGC, 0, gid);
    c->gc[i].used = 0;
}
void h_clear_area(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t wid = rd32(p + 4);
    int wi = win_idx(c, wid);
    if (wi < 0)
        return post_error(c, X11_ERR_Window, X11_REQ_ClearArea, 0, wid);
    struct X11_DRAW d = draw_get(c, wid);
    if (!d.ok)
        return;
    int16_t x = (int16_t)rd16(p + 8), y = (int16_t)rd16(p + 10);
    uint16_t w = rd16(p + 12), h = rd16(p + 14);
    if (p[1]) {
        uint8_t ev[32];
        memset(ev, 0, sizeof(ev));
        ev[0] = X11_EV_EXPOSE;
        wr16(ev + 2, (uint16_t)c->seq);
        wr32(ev + 4, wid);
        wr16(ev + 8, (uint16_t)x);
        wr16(ev + 10, (uint16_t)y);
        wr16(ev + 12, w);
        wr16(ev + 14, h);
        post_event_to(c, &c->win[wi], ev, 0x00008000u);
    }
    if (w == 0 || h == 0) {
        w = (uint16_t)d.cv.w;
        h = (uint16_t)d.cv.h;
        x = 0;
        y = 0;
    }
    gfx_fill(&d.cv, x, y, w, h, pix_to_color(c->win[wi].bg_pixel));
    draw_commit(c, wid);
}
void h_copy_area(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    struct X11_DRAW src = draw_get(c, rd32(p + 4));
    struct X11_DRAW dst = draw_get(c, rd32(p + 8));
    if (!src.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_CopyArea, 0,
                          rd32(p + 4));
    if (!dst.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_CopyArea, 0,
                          rd32(p + 8));
    int16_t sx = (int16_t)rd16(p + 12), sy = (int16_t)rd16(p + 14);
    int16_t dx = (int16_t)rd16(p + 16), dy = (int16_t)rd16(p + 18);
    uint16_t w = rd16(p + 20), h = rd16(p + 22);
    gfx_blit(&dst.cv, dx, dy, &src.cv, sx, sy, w, h);
    draw_commit(c, rd32(p + 8));
}
int gc_fg(struct X11_CONN *c, uint32_t gid, gfx_color *out) {
    int gi = gc_idx(c, gid);
    if (gi < 0)
        return -1;
    *out = pix_to_color(c->gc[gi].fg);
    return 0;
}
void h_poly_point(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyPoint, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyPoint, 0,
                          rd32(p + 8));
    uint8_t coord_mode = p[1];
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int cur_x = 0, cur_y = 0;
    while (remain >= 4) {
        int16_t px = (int16_t)rd16(v), py = (int16_t)rd16(v + 2);
        if (coord_mode == 0) {
            gfx_px(&d.cv, px, py, fg);
            cur_x = px;
            cur_y = py;
        } else {
            gfx_px(&d.cv, cur_x + px, cur_y + py, fg);
            cur_x += px;
            cur_y += py;
        }
        v += 4;
        remain -= 4;
    }
    draw_commit(c, did);
}
void draw_line(struct GFX_CANVAS *cv, int x0, int y0, int x1, int y1,
                      gfx_color col) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    int err = dx - dy;
    for (;;) {
        gfx_px(cv, x0, y0, col);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}
void h_poly_line(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyLine, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyLine, 0,
                          rd32(p + 8));
    uint8_t coord_mode = p[1];
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int cur_x = 0, cur_y = 0, first = 1;
    while (remain >= 4) {
        int16_t gx = (int16_t)rd16(v), gy = (int16_t)rd16(v + 2);
        int px = (coord_mode == 0) ? gx : cur_x + gx;
        int py = (coord_mode == 0) ? gy : cur_y + gy;
        if (!first)
            draw_line(&d.cv, cur_x, cur_y, px, py, fg);
        cur_x = px;
        cur_y = py;
        first = 0;
        v += 4;
        remain -= 4;
    }
    draw_commit(c, did);
}
void h_poly_segment(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolySegment, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolySegment, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        draw_line(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2),
                  (int16_t)rd16(v + 4), (int16_t)rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}
void h_poly_rectangle(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyRectangle, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyRectangle, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        gfx_rect(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2), rd16(v + 4),
                 rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}
void h_poly_fill_rect(struct X11_CONN *c, const uint8_t *p,
                             uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyFillRectangle, 0,
                          did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyFillRectangle, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 8) {
        gfx_fill(&d.cv, (int16_t)rd16(v), (int16_t)rd16(v + 2), rd16(v + 4),
                 rd16(v + 6), fg);
        v += 8;
        remain -= 8;
    }
    draw_commit(c, did);
}
void h_poly_arc(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyArc, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyArc, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 12) {
        int x = (int16_t)rd16(v), y = (int16_t)rd16(v + 2);
        int w = rd16(v + 4), h = rd16(v + 6);
        int a1 = (int16_t)rd16(v + 8), a2 = (int16_t)rd16(v + 10);
        int rx = w / 2, ry = h / 2;
        int cx = x + rx, cy = y + ry;
        if (a2 < a1)
            a2 += 360;
        for (int a = a1; a <= a2; a += 2) {
            int idx = a % 360;
            if (idx < 0)
                idx += 360;
            int px = cx + (int)((long)rx * trig_cos(idx) / TRIG_ONE);
            int py = cy + (int)((long)ry * trig_sin(idx) / TRIG_ONE);
            gfx_px(&d.cv, px, py, fg);
        }
        v += 12;
        remain -= 12;
    }
    draw_commit(c, did);
}
void h_poly_fill_arc(struct X11_CONN *c, const uint8_t *p,
                            uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PolyFillArc, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_PolyFillArc, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    while (remain >= 12) {
        int x = (int16_t)rd16(v), y = (int16_t)rd16(v + 2);
        int w = rd16(v + 4), h = rd16(v + 6);
        int rad = (w < h ? w : h) / 2;
        gfx_fill_round(&d.cv, x, y, w, h, rad, fg);
        v += 12;
        remain -= 12;
    }
    draw_commit(c, did);
}
void h_fill_poly(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_FillPoly, 0, did);
    gfx_color fg;
    if (gc_fg(c, rd32(p + 8), &fg) != 0)
        return post_error(c, X11_ERR_GContext, X11_REQ_FillPoly, 0,
                          rd32(p + 8));
    const uint8_t *v = p + 12;
    uint32_t remain = len - 12;
    int16_t xs[16], ys[16];
    int n = 0;
    while (remain >= 4 && n < 16) {
        xs[n] = (int16_t)rd16(v);
        ys[n] = (int16_t)rd16(v + 2);
        n++;
        v += 4;
        remain -= 4;
    }
    if (n < 3)
        return;
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < ymin)
            ymin = ys[i];
        if (ys[i] > ymax)
            ymax = ys[i];
    }
    for (int y = ymin; y <= ymax; y++) {
        int xints[16], cnt = 0;
        for (int i = 0; i < n && cnt < 15; i++) {
            int j = (i + 1) % n;
            if ((ys[i] <= y && ys[j] > y) || (ys[j] <= y && ys[i] > y)) {
                int num = (y - ys[i]) * (xs[j] - xs[i]);
                int den = ys[j] - ys[i];
                xints[cnt++] = xs[i] + (den != 0 ? num / den : 0);
            }
        }
        for (int i = 0; i < cnt; i++)
            for (int k = i + 1; k < cnt; k++)
                if (xints[k] < xints[i]) {
                    int t = xints[i];
                    xints[i] = xints[k];
                    xints[k] = t;
                }
        for (int i = 0; i + 1 < cnt; i += 2)
            gfx_hline(&d.cv, xints[i], y, xints[i + 1] - xints[i] + 1, fg);
    }
    draw_commit(c, did);
}
void h_put_image(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 28)
        return post_error(c, X11_ERR_Length, X11_REQ_PutImage, 0, 0);
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_PutImage, 0, did);
    uint16_t w = rd16(p + 12), h = rd16(p + 14);
    int16_t dx = (int16_t)rd16(p + 16), dy = (int16_t)rd16(p + 18);
    uint8_t depth = p[21];
    uint8_t format = p[1];
    if (format != 2 || (depth != 24 && depth != 32))
        return post_error(c, X11_ERR_Match, X11_REQ_PutImage, 0, did);
    const uint8_t *src = p + 24;
    uint32_t row_bytes = (depth == 32) ? (uint32_t)w * 4u
                                       : (((uint32_t)w * 3u + 3u) & ~3u);
    uint32_t avail = len - 24;
    uint32_t rows = (row_bytes > 0) ? (avail / row_bytes) : 0;
    if (rows > h)
        rows = h;
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *s = src + r * row_bytes;
        int py = dy + (int)r;
        if (py < 0 || py >= d.cv.h)
            continue;
        for (uint32_t x = 0; x < w; x++) {
            int px = dx + (int)x;
            if (px < 0 || px >= d.cv.w)
                continue;
            const uint8_t *pixp = s + (depth == 32 ? x * 4u : x * 3u);
            gfx_px(&d.cv, px, py,
                   GFX_RGB((int)pixp[2], (int)pixp[1], (int)pixp[0]));
        }
    }
    draw_commit(c, did);
}
void h_get_image(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    if (len < 20)
        return post_error(c, X11_ERR_Length, X11_REQ_GetImage, 0, 0);
    uint32_t did = rd32(p + 4);
    struct X11_DRAW d = draw_get(c, did);
    if (!d.ok)
        return post_error(c, X11_ERR_Drawable, X11_REQ_GetImage, 0, did);
    int16_t x = (int16_t)rd16(p + 12), y = (int16_t)rd16(p + 14);
    uint16_t w = rd16(p + 16), h = rd16(p + 18);
    uint8_t format = p[1];
    if (format != 2)
        return post_error(c, X11_ERR_Match, X11_REQ_GetImage, 0, did);
    uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t total = row_bytes * h;
    uint8_t head[32];
    reply_init(c, X11_REQ_GetImage, total, head);
    head[1] = 24;
    out_bytes(c, head, 32);
    for (uint32_t r = 0; r < h; r++) {
        uint8_t row[2048];
        uint32_t n = row_bytes < sizeof(row) ? row_bytes : (uint32_t)sizeof(row);
        memset(row, 0, n);
        for (uint32_t xx = 0; xx < w; xx++) {
            int px = x + (int)xx, py = y + (int)r;
            if (px < 0 || py < 0 || px >= d.cv.w || py >= d.cv.h)
                continue;
            gfx_color col = *gfx_px_at(&d.cv,
                                       (size_t)py * (size_t)d.cv.pitch +
                                           (size_t)px * 4u);
            if (xx * 3u + 2 < n) {
                row[xx * 3 + 0] = (uint8_t)GFX_B(col);
                row[xx * 3 + 1] = (uint8_t)GFX_G(col);
                row[xx * 3 + 2] = (uint8_t)GFX_R(col);
            }
        }
        out_bytes(c, row, n);
    }
}
void h_alloc_color(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint16_t r = rd16(p + 8), g = rd16(p + 10), b = rd16(p + 12);
    uint32_t pixel = ((uint32_t)(r >> 8) << 16) | ((uint32_t)(g >> 8) << 8) |
                     (uint32_t)(b >> 8);
    uint8_t head[32];
    reply_init(c, X11_REQ_AllocColor, 0, head);
    uint8_t *bb = head + 8;
    wr16(bb + 0, r);
    wr16(bb + 2, g);
    wr16(bb + 4, b);
    wr32(bb + 8, pixel);
    reply_finish(c, head, 0, 0);
}
void h_alloc_named_color(struct X11_CONN *c, const uint8_t *p,
                                uint32_t len) {
    (void)len;
    uint16_t nl = rd16(p + 8);
    char name[24];
    int n = nl < 23 ? nl : 23;
    memcpy(name, p + 12, (size_t)n);
    name[n] = 0;
    uint32_t pixel = 0xFFFFFFu;
    if (strncmp(name, "black", 5) == 0)
        pixel = 0x000000u;
    else if (strncmp(name, "red", 3) == 0)
        pixel = 0xFF0000u;
    else if (strncmp(name, "green", 5) == 0)
        pixel = 0x00FF00u;
    else if (strncmp(name, "blue", 4) == 0)
        pixel = 0x0000FFu;
    else if (strncmp(name, "gray", 4) == 0)
        pixel = 0x808080u;
    uint8_t head[32];
    reply_init(c, X11_REQ_AllocNamedColor, 0, head);
    uint8_t *b = head + 8;
    wr32(b + 0, pixel);
    uint16_t rv = (uint16_t)(((pixel >> 16) & 0xFF) * 257);
    uint16_t gv = (uint16_t)(((pixel >> 8) & 0xFF) * 257);
    uint16_t bv = (uint16_t)((pixel & 0xFF) * 257);
    wr16(b + 4, rv);
    wr16(b + 6, gv);
    wr16(b + 8, bv);
    wr16(b + 10, rv);
    wr16(b + 12, gv);
    wr16(b + 14, bv);
    reply_finish(c, head, 0, 0);
}
void h_query_colors(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint32_t n = len >= 12 ? (len - 8) / 4 : 0;
    if (n > 3)
        n = 3;
    uint8_t head[32];
    reply_init(c, X11_REQ_QueryColors, 0, head);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pix = rd32(p + 8 + i * 4);
        uint8_t *b = head + 8 + i * 8;
        wr16(b + 0, (uint16_t)(((pix >> 16) & 0xFF) * 257));
        wr16(b + 2, (uint16_t)(((pix >> 8) & 0xFF) * 257));
        wr16(b + 4, (uint16_t)((pix & 0xFF) * 257));
    }
    reply_finish(c, head, 0, 0);
}
void h_lookup_color(struct X11_CONN *c, const uint8_t *p, uint32_t len) {
    (void)len;
    uint16_t nl = rd16(p + 8);
    char name[24];
    int n = nl < 23 ? nl : 23;
    memcpy(name, p + 12, (size_t)n);
    name[n] = 0;
    uint32_t pixel = 0xFFFFFFu;
    if (strncmp(name, "black", 5) == 0)
        pixel = 0x000000u;
    uint8_t head[32];
    reply_init(c, X11_REQ_LookupColor, 0, head);
    uint8_t *b = head + 8;
    uint16_t rv = (uint16_t)(((pixel >> 16) & 0xFF) * 257);
    uint16_t gv = (uint16_t)(((pixel >> 8) & 0xFF) * 257);
    uint16_t bv = (uint16_t)((pixel & 0xFF) * 257);
    wr16(b + 0, rv);
    wr16(b + 2, gv);
    wr16(b + 4, bv);
    wr16(b + 6, rv);
    wr16(b + 8, gv);
    wr16(b + 10, bv);
    reply_finish(c, head, 0, 0);
}
