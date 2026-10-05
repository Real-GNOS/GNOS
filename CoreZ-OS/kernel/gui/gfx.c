#include "kernel/gui/gfx.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/str/str.h"

static struct GFX_FB_FORMAT fb_fmt = {32, 16, 8, 8, 8, 0, 8};

void gfx_set_fb_format(const struct GFX_FB_FORMAT *fmt) {
    if (fmt)
        fb_fmt = *fmt;
}

int gfx_fb_bpp(void) {
    return fb_fmt.bpp;
}

gfx_color gfx_over(gfx_color dst, gfx_color src, int alpha) {
    int sa = GFX_A(src);
    if (alpha < 255)
        sa = gfx_div255(sa * alpha + 127);
    if (sa <= 0)
        return dst;
    if (sa >= 255)
        return src | 0xFF000000u;
    int dr = GFX_R(dst), dg = GFX_G(dst), db = GFX_B(dst);
    int sr = GFX_R(src), sg = GFX_G(src), sb = GFX_B(src);
    int r = dr + gfx_div255((sr - dr) * sa + 127);
    int g = dg + gfx_div255((sg - dg) * sa + 127);
    int b = db + gfx_div255((sb - db) * sa + 127);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int gfx_rect_intersect(struct GFX_RECT a, struct GFX_RECT b,
                       struct GFX_RECT *out) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    if (out) {
        out->x = x0;
        out->y = y0;
        out->w = x1 - x0;
        out->h = y1 - y0;
    }
    return 1;
}

static size_t canvas_bytes(const struct GFX_CANVAS *c, int *ok) {
    if (!c || !c->pixels || c->w <= 0 || c->h <= 0 || c->pitch < c->w * 4) {
        *ok = 0;
        return 0;
    }
    return gfx_canvas_mapped_bytes(c, ok);
}

static int gfx_blit_clip(struct GFX_CANVAS *dst, int *dx, int *dy,
                         const struct GFX_CANVAS *src, int *sx, int *sy, int *w,
                         int *h) {
    if (*dx < 0) {
        *sx -= *dx;
        *w += *dx;
        *dx = 0;
    }
    if (*dy < 0) {
        *sy -= *dy;
        *h += *dy;
        *dy = 0;
    }
    if (*dx + *w > dst->w)
        *w = dst->w - *dx;
    if (*dy + *h > dst->h)
        *h = dst->h - *dy;
    if (*w <= 0 || *h <= 0)
        return 0;
    if (*sx < 0) {
        *dx -= *sx;
        *w += *sx;
        *sx = 0;
    }
    if (*sy < 0) {
        *dy -= *sy;
        *h += *sy;
        *sy = 0;
    }
    if (*sx + *w > src->w)
        *w = src->w - *sx;
    if (*sy + *h > src->h)
        *h = src->h - *sy;
    if (*w <= 0 || *h <= 0)
        return 0;
    return 1;
}

static int blit_prepare(struct GFX_CANVAS *dst, int *dx, int *dy,
                        const struct GFX_CANVAS *src, int *sx, int *sy,
                        int *w, int *h) {
    int ok;
    size_t dsz = canvas_bytes(dst, &ok);
    if (!ok)
        return 0;
    size_t ssz = canvas_bytes(src, &ok);
    if (!ok)
        return 0;
    if (!gfx_blit_clip(dst, dx, dy, src, sx, sy, w, h))
        return 0;
    size_t dlast = (size_t)(*dy + *h - 1) * (size_t)dst->pitch +
                   (size_t)(*dx + *w - 1) * 4u + 4u;
    size_t slast = (size_t)(*sy + *h - 1) * (size_t)src->pitch +
                   (size_t)(*sx + *w - 1) * 4u + 4u;
    return dlast <= dsz && slast <= ssz;
}

void gfx_px(struct GFX_CANVAS *c, int x, int y, gfx_color color) {
    if (!c || x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok || (size_t)y * (size_t)c->pitch + (size_t)x * 4u + 4u > mapped)
        return;
    gfx_color *p = gfx_row(c, y) + x;
    *p = gfx_over(*p, color, 255);
}

void gfx_fill(struct GFX_CANVAS *c, int x, int y, int w, int h,
              gfx_color color) {
    if (!c || w <= 0 || h <= 0)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok || mapped == 0)
        return;
    struct GFX_RECT clip = {0, 0, c->w, c->h};
    struct GFX_RECT r = {x, y, w, h}, v;
    if (!gfx_rect_intersect(clip, r, &v))
        return;
    int a = GFX_A(color);
    gfx_color rgb = color & 0x00FFFFFFu;
    for (int row = 0; row < v.h; row++) {
        size_t off = (size_t)(v.y + row) * (size_t)c->pitch + (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = gfx_row(c, v.y + row) + v.x;
        if (a >= 255) {
            for (int i = 0; i < v.w; i++)
                p[i] = rgb | 0xFF000000u;
        } else if (a > 0) {
            for (int i = 0; i < v.w; i++)
                p[i] = gfx_over(p[i], rgb, a);
        }
    }
}

void gfx_hline(struct GFX_CANVAS *c, int x, int y, int len, gfx_color color) {
    gfx_fill(c, x, y, len, 1, color);
}

void gfx_vline(struct GFX_CANVAS *c, int x, int y, int len, gfx_color color) {
    gfx_fill(c, x, y, 1, len, color);
}

void gfx_rect(struct GFX_CANVAS *c, int x, int y, int w, int h,
              gfx_color color) {
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(c, x, y, w, color);
    gfx_hline(c, x, y + h - 1, w, color);
    gfx_vline(c, x, y, h, color);
    gfx_vline(c, x + w - 1, y, h, color);
}

void gfx_blit(struct GFX_CANVAS *dst, int dx, int dy,
              const struct GFX_CANVAS *src, int sx, int sy, int w, int h) {
    if (!blit_prepare(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    if (dx * 4 + w * 4 <= dst->pitch && sx * 4 + w * 4 <= src->pitch) {
        for (int row = 0; row < h; row++)
            memcpy(gfx_row(dst, dy + row) + dx,
                   gfx_row_c(src, sy + row) + sx, (size_t)w * 4u);
        return;
    }
    for (int row = 0; row < h; row++) {
        gfx_color *d = gfx_row(dst, dy + row) + dx;
        const gfx_color *s = gfx_row_c(src, sy + row) + sx;
        for (int i = 0; i < w; i++)
            d[i] = s[i];
    }
}

void gfx_blit_alpha(struct GFX_CANVAS *dst, int dx, int dy,
                    const struct GFX_CANVAS *src, int sx, int sy, int w, int h,
                    int alpha) {
    if (alpha <= 0 || !blit_prepare(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        gfx_color *d = gfx_row(dst, dy + row) + dx;
        const gfx_color *s = gfx_row_c(src, sy + row) + sx;
        for (int i = 0; i < w; i++)
            if (GFX_A(s[i]))
                d[i] = gfx_over(d[i], s[i], alpha);
    }
}

uint8_t gfx_round_coverage(int px, int py, int w, int h, int rad,
                           int corners) {
    int cx, cy;
    if (rad <= 0)
        return 255;
    if (px < rad) {
        if (py < rad) {
            if (!(corners & GFX_CORNER_TL))
                return 255;
            cx = rad;
            cy = rad;
        } else if (py >= h - rad) {
            if (!(corners & GFX_CORNER_BL))
                return 255;
            cx = rad;
            cy = h - rad;
        } else {
            return 255;
        }
    } else if (px >= w - rad) {
        if (py < rad) {
            if (!(corners & GFX_CORNER_TR))
                return 255;
            cx = w - rad;
            cy = rad;
        } else if (py >= h - rad) {
            if (!(corners & GFX_CORNER_BR))
                return 255;
            cx = w - rad;
            cy = h - rad;
        } else {
            return 255;
        }
    } else {
        return 255;
    }
    int rr = rad * 8;
    int rr2 = rr * rr;
    int bx = 8 * px + 1 - 8 * cx;
    int by = 8 * py + 1 - 8 * cy;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = by + 2 * sy;
        int dy2 = dy * dy;
        for (int sx = 0; sx < 4; sx++) {
            int dx = bx + 2 * sx;
            if (dx * dx + dy2 <= rr2)
                hits++;
        }
    }
    return (uint8_t)(hits * 255 / 16);
}

static void round_core(struct GFX_CANVAS *c, int x, int y, int w, int h,
                       int rad, int corners, gfx_color color, int alpha,
                       int outside) {
    if (!c || w <= 0 || h <= 0)
        return;
    int ok;
    size_t mapped = canvas_bytes(c, &ok);
    if (!ok || mapped == 0)
        return;
    struct GFX_RECT clip = {0, 0, c->w, c->h};
    struct GFX_RECT r = {x, y, w, h}, v;
    if (!gfx_rect_intersect(clip, r, &v))
        return;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int ca = gfx_div255(GFX_A(color) * alpha + 127);
    if (ca <= 0)
        return;
    gfx_color rgb = color & 0x00FFFFFFu;
    for (int row = 0; row < v.h; row++) {
        size_t off = (size_t)(v.y + row) * (size_t)c->pitch + (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = gfx_row(c, v.y + row) + v.x;
        int py = v.y + row - y;
        int full_row = (py >= rad && py < h - rad);
        for (int col = 0; col < v.w; col++) {
            int px = v.x + col - x;
            int cov;
            if (full_row && px >= rad && px < w - rad) {
                cov = 255;
            } else {
                cov = gfx_round_coverage(px, py, w, h, rad, corners);
                if (outside)
                    cov = 255 - cov;
            }
            if (cov <= 0)
                continue;
            int a = gfx_div255(ca * cov);
            if (a >= 255)
                p[col] = rgb | 0xFF000000u;
            else if (a > 0)
                p[col] = gfx_over(p[col], rgb, a);
        }
    }
}

void gfx_fill_round(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                    gfx_color color) {
    round_core(c, x, y, w, h, rad, GFX_CORNER_ALL, color, 255, 0);
}

void gfx_fill_round_a(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                      gfx_color color, int alpha) {
    round_core(c, x, y, w, h, rad, GFX_CORNER_ALL, color, alpha, 0);
}

void gfx_mask_round(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                    gfx_color color, int corners) {
    round_core(c, x, y, w, h, rad, corners, color, 255, 1);
}

void gfx_blit_round(struct GFX_CANVAS *dst, int dx, int dy,
                    const struct GFX_CANVAS *src, int sx, int sy, int w, int h,
                    int alpha, int rx, int ry, int rw, int rh, int rad,
                    int corners) {
    if (alpha <= 0 || !blit_prepare(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > rw)
        rad = rw / 2;
    if (rad * 2 > rh)
        rad = rh / 2;
    for (int row = 0; row < h; row++) {
        gfx_color *d = gfx_row(dst, dy + row) + dx;
        const gfx_color *s = gfx_row_c(src, sy + row) + sx;
        int py = dy + row - ry;
        for (int i = 0; i < w; i++) {
            gfx_color sp = s[i];
            int sa = GFX_A(sp);
            if (sa == 0)
                continue;
            int cov = gfx_round_coverage(dx + i - rx, py, rw, rh, rad, corners);
            if (cov <= 0)
                continue;
            int a = gfx_div255(sa * alpha + 127);
            a = gfx_div255(a * cov);
            if (a > 0)
                d[i] = gfx_over(d[i], sp, a * 255 / sa);
        }
    }
}

static int fmt_is_standard_32(const struct GFX_FB_FORMAT *f) {
    return f->bpp == 32 && f->r_pos == 16 && f->r_bits == 8 && f->g_pos == 8 &&
           f->g_bits == 8 && f->b_pos == 0 && f->b_bits == 8;
}

static uint32_t scale_channel(int v, int bits) {
    if (bits >= 8)
        return (uint32_t)v << (bits - 8);
    int max = (1 << bits) - 1;
    return (uint32_t)gfx_div255(v * max + 127);
}

void gfx_present(struct GFX_CANVAS *dst, int dx, int dy,
                 const struct GFX_CANVAS *src, int sx, int sy, int w, int h) {
    if (gfx_fb_bpp() != 32 || fmt_is_standard_32(&fb_fmt)) {
        gfx_blit(dst, dx, dy, src, sx, sy, w, h);
        return;
    }
    if (!blit_prepare(dst, &dx, &dy, src, &sx, &sy, &w, &h))
        return;
    for (int row = 0; row < h; row++) {
        gfx_color *d = gfx_row(dst, dy + row) + dx;
        const gfx_color *s = gfx_row_c(src, sy + row) + sx;
        for (int i = 0; i < w; i++) {
            gfx_color p = s[i];
            uint32_t dev = 0;
            if (fb_fmt.r_bits)
                dev |= scale_channel(GFX_R(p), fb_fmt.r_bits) << fb_fmt.r_pos;
            if (fb_fmt.g_bits)
                dev |= scale_channel(GFX_G(p), fb_fmt.g_bits) << fb_fmt.g_pos;
            if (fb_fmt.b_bits)
                dev |= scale_channel(GFX_B(p), fb_fmt.b_bits) << fb_fmt.b_pos;
            d[i] = dev;
        }
    }
}

void gfx_blit_scale(struct GFX_CANVAS *dst, int dx, int dy, int dw, int dh,
                    const struct GFX_CANVAS *src, int sx, int sy, int sw,
                    int sh) {
    if (!dst || !dst->pixels || !src || !src->pixels)
        return;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    int dstride = gfx_stride(dst);
    int sstride = gfx_stride(src);
    int sex = sx + sw - 1;
    int sey = sy + sh - 1;
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < 0 || py >= dst->h)
            continue;
        uint64_t fy = (((uint64_t)y * (uint64_t)sh) << 16) / (uint64_t)dh;
        int sy0 = sy + (int)(fy >> 16);
        int wy = (int)((fy >> 8) & 0xFFu);
        if (sy0 > sey)
            sy0 = sey;
        int sy1 = (sy0 < sey) ? sy0 + 1 : sey;
        const gfx_color *r0 = src->pixels + (size_t)sy0 * (size_t)sstride;
        const gfx_color *r1 = src->pixels + (size_t)sy1 * (size_t)sstride;
        gfx_color *drow = dst->pixels + (size_t)py * (size_t)dstride;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= dst->w)
                continue;
            uint64_t fx = (((uint64_t)x * (uint64_t)sw) << 16) / (uint64_t)dw;
            int sx0 = sx + (int)(fx >> 16);
            int wx = (int)((fx >> 8) & 0xFFu);
            if (sx0 > sex)
                sx0 = sex;
            int sx1 = (sx0 < sex) ? sx0 + 1 : sex;
            gfx_color c00 = r0[sx0], c01 = r0[sx1];
            gfx_color c10 = r1[sx0], c11 = r1[sx1];
            int w00 = (256 - wx) * (256 - wy);
            int w01 = wx * (256 - wy);
            int w10 = (256 - wx) * wy;
            int w11 = wx * wy;
            int r = (GFX_R(c00) * w00 + GFX_R(c01) * w01 + GFX_R(c10) * w10 +
                     GFX_R(c11) * w11) >> 16;
            int g = (GFX_G(c00) * w00 + GFX_G(c01) * w01 + GFX_G(c10) * w10 +
                     GFX_G(c11) * w11) >> 16;
            int b = (GFX_B(c00) * w00 + GFX_B(c01) * w01 + GFX_B(c10) * w10 +
                     GFX_B(c11) * w11) >> 16;
            drow[px] = GFX_RGB(r, g, b);
        }
    }
}
