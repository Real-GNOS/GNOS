#include "kernel/gui/gpu.h"

#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

#define GPU_QUEUE 512

static struct GPU_CMD cmd_queue[GPU_QUEUE];
static int cmd_n;
static struct GFX_CANVAS *gpu_dst;
static struct GFX_RECT gpu_clip = {0, 0, 0, 0};

struct SHADOW_CACHE {
    struct GFX_CANVAS cv;
    int fw, fh, rad, blur;
    int valid;
};

static struct SHADOW_CACHE sh;

static int isqrt_int(int v) {
    int res = 0, bit = 1 << 30;
    if (v <= 0)
        return 0;
    while (bit > v)
        bit >>= 2;
    while (bit > 0) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static int shadow_rebuild(int fw, int fh, int rad, int blur) {
    int sw = fw + 2 * blur;
    int shh = fh + 2 * blur;
    size_t bytes = (size_t)sw * (size_t)shh * 4u;
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint8_t *mem = (uint8_t *)get_kernel_pages(pages);
    if (mem == 0)
        return -1;
    struct GFX_CANVAS *cv = &sh.cv;
    cv->pixels = (gfx_color *)mem;
    cv->pitch = sw * 4;
    cv->w = sw;
    cv->h = shh;
    cv->bytes = bytes;
    memset(mem, 0, pages * PAGE_SIZE);

    int hw = fw / 2, hh = fh / 2;
    int ex = hw - rad, ey = hh - rad;
    for (int y = 0; y < shh; y++) {
        gfx_color *row = cv->pixels + (size_t)y * (size_t)sw;
        int iy = y - blur - hh;
        int qy = (iy < 0 ? -iy : iy) - ey;
        for (int x = 0; x < sw; x++) {
            int ix = x - blur - hw;
            int qx = (ix < 0 ? -ix : ix) - ex;
            int ax = qx > 0 ? qx : 0;
            int ay = qy > 0 ? qy : 0;
            int m = (qx > qy ? qx : qy);
            if (m > 0)
                m = 0;
            int d = isqrt_int(ax * ax + ay * ay) + m - rad;
            if (d < 0)
                d = 0;
            if (d >= blur)
                continue;

            int u = blur - d;
            int a = (90 * u * u) / (blur * blur);
            if (a <= 0)
                continue;
            row[x] = (uint32_t)a << 24;
        }
    }
    sh.fw = fw;
    sh.fh = fh;
    sh.rad = rad;
    sh.blur = blur;
    sh.valid = 1;
    return 0;
}

struct GFX_CANVAS *gpu_shadow_sprite(int fw, int fh, int rad, int blur) {
    if (fw <= 0 || fh <= 0 || blur <= 0)
        return 0;
    if (rad < 0)
        rad = 0;
    if (sh.valid && sh.fw == fw && sh.fh == fh && sh.rad == rad &&
        sh.blur == blur)
        return &sh.cv;
    if (shadow_rebuild(fw, fh, rad, blur) != 0)
        return 0;
    return &sh.cv;
}

static int clip_rect(struct GFX_RECT *v, const struct GFX_RECT *r) {
    return gfx_rect_intersect(*r, gpu_clip, v);
}

static void exec_fill(const struct GPU_CMD *c) {
    struct GFX_RECT r = {c->x, c->y, c->w, c->h}, v;
    if (!clip_rect(&v, &r) || !gpu_dst)
        return;
    int a = gfx_div255(GFX_A(c->color) * c->alpha + 127);
    if (a <= 0)
        return;
    gfx_color rgb = c->color & 0x00FFFFFFu;
    size_t mapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    for (int row = 0; row < v.h; row++) {
        size_t off = (size_t)(v.y + row) * (size_t)gpu_dst->pitch +
                     (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = gfx_px_at(gpu_dst, off);
        if (a >= 255) {
            for (int i = 0; i < v.w; i++)
                p[i] = rgb | 0xFF000000u;
        } else {
            for (int i = 0; i < v.w; i++)
                p[i] = gfx_over(p[i], rgb, a);
        }
    }
}

static void exec_copy(const struct GPU_CMD *c) {
    if (!gpu_dst || !c->src || !c->src->pixels)
        return;
    int dx = c->x, dy = c->y, sx = c->sx, sy = c->sy, w = c->w, h = c->h;
    struct GFX_RECT r = {dx, dy, w, h}, v;
    if (!clip_rect(&v, &r))
        return;
    sx += v.x - dx;
    sy += v.y - dy;
    w = v.w;
    h = v.h;
    dx = v.x;
    dy = v.y;
    if (sx < 0 || sy < 0 || sx + w > c->src->w || sy + h > c->src->h)
        return;
    size_t dmapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    size_t smapped = gfx_canvas_mapped_bytes(c->src, &(int){0});
    for (int row = 0; row < h; row++) {
        size_t doff = (size_t)(dy + row) * (size_t)gpu_dst->pitch +
                      (size_t)dx * 4u;
        size_t soff = (size_t)(sy + row) * (size_t)c->src->pitch +
                      (size_t)sx * 4u;
        if (doff + (size_t)w * 4u > dmapped || soff + (size_t)w * 4u > smapped)
            return;
        uint8_t *d = (uint8_t *)gpu_dst->pixels + doff;
        const uint8_t *s = (const uint8_t *)c->src->pixels + soff;
        for (int i = 0; i < w * 4; i++)
            d[i] = s[i];
    }
}

static inline gfx_color blend_px(gfx_color d, gfx_color s, int sa) {
    int dr = GFX_R(d), dg = GFX_G(d), db = GFX_B(d);
    int sr = GFX_R(s), sg = GFX_G(s), sb = GFX_B(s);
    return (uint32_t)0xFF000000u |
           ((uint32_t)(dr + gfx_div255((sr - dr) * sa + 127)) << 16) |
           ((uint32_t)(dg + gfx_div255((sg - dg) * sa + 127)) << 8) |
           (uint32_t)(db + gfx_div255((sb - db) * sa + 127));
}

static void blend_run(gfx_color *d, const gfx_color *s, int n, int ga) {
    for (int i = 0; i < n; i++) {
        gfx_color sp = s[i];
        int sa = GFX_A(sp);
        if (sa == 0)
            continue;
        sa = gfx_div255(sa * ga + 127);
        d[i] = blend_px(d[i], sp, sa);
    }
}

static void copy_run(gfx_color *d, const gfx_color *s, int n) {
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void exec_blend(const struct GPU_CMD *c) {
    if (!gpu_dst || !c->src || !c->src->pixels || c->alpha <= 0)
        return;
    int dx = c->x, dy = c->y, sx = c->sx, sy = c->sy, w = c->w, h = c->h;
    struct GFX_RECT r = {dx, dy, w, h}, v;
    if (!clip_rect(&v, &r))
        return;
    sx += v.x - dx;
    sy += v.y - dy;
    w = v.w;
    h = v.h;
    dx = v.x;
    dy = v.y;
    if (sx < 0 || sy < 0 || sx + w > c->src->w || sy + h > c->src->h)
        return;
    size_t dmapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    size_t smapped = gfx_canvas_mapped_bytes(c->src, &(int){0});
    for (int row = 0; row < h; row++) {
        size_t doff = (size_t)(dy + row) * (size_t)gpu_dst->pitch +
                      (size_t)dx * 4u;
        size_t soff = (size_t)(sy + row) * (size_t)c->src->pitch +
                      (size_t)sx * 4u;
        if (doff + (size_t)w * 4u > dmapped || soff + (size_t)w * 4u > smapped)
            return;
        blend_run(gfx_px_at(gpu_dst, doff),
                  gfx_px_at_c(c->src, soff),
                  w, c->alpha);
    }
}

static void exec_round_blend(const struct GPU_CMD *c) {
    if (!gpu_dst || !c->src || !c->src->pixels || c->alpha <= 0)
        return;
    int w = c->w, h = c->h, rad = c->rad;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int dx0 = c->x, dy0 = c->y, sx0 = c->sx, sy0 = c->sy;
    struct GFX_RECT r = {dx0, dy0, w, h}, v;
    if (!clip_rect(&v, &r))
        return;
    size_t dmapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    size_t smapped = gfx_canvas_mapped_bytes(c->src, &(int){0});
    int top_band = (c->corners & (GFX_CORNER_TL | GFX_CORNER_TR)) ? rad : 0;
    int bot_band =
        (c->corners & (GFX_CORNER_BL | GFX_CORNER_BR)) ? rad : 0;
    for (int row = 0; row < h; row++) {
        int py = dy0 + row;
        if (py < v.y || py >= v.y + v.h)
            continue;
        int ly = py - dy0;
        size_t doff = (size_t)py * (size_t)gpu_dst->pitch + (size_t)dx0 * 4u;
        size_t soff = (size_t)(sy0 + row) * (size_t)c->src->pitch +
                      (size_t)sx0 * 4u;
        if (doff + (size_t)w * 4u > dmapped || soff + (size_t)w * 4u > smapped)
            return;
        gfx_color *d = gfx_px_at(gpu_dst, doff);
        const gfx_color *s =
            gfx_px_at_c(c->src, soff);
        int in_corner_band = (ly < top_band) || (ly >= h - bot_band);
        int mid0 = 0, mid1 = w;
        if (in_corner_band && rad > 0) {
            for (int lx = 0; lx < rad; lx++) {
                int px = dx0 + lx;
                if (px < v.x || px >= v.x + v.w)
                    continue;
                int cov = gfx_round_coverage(lx, ly, w, h, rad, c->corners);
                if (cov <= 0)
                    continue;
                gfx_color sp = s[lx];
                int sa = gfx_div255(GFX_A(sp) * c->alpha + 127);
                sa = gfx_div255(sa * cov);
                if (sa > 0)
                    d[lx] = blend_px(d[lx], sp, sa);
            }
            for (int lx = w - rad; lx < w; lx++) {
                int px = dx0 + lx;
                if (px < v.x || px >= v.x + v.w)
                    continue;
                int cov = gfx_round_coverage(lx, ly, w, h, rad, c->corners);
                if (cov <= 0)
                    continue;
                gfx_color sp = s[lx];
                int sa = gfx_div255(GFX_A(sp) * c->alpha + 127);
                sa = gfx_div255(sa * cov);
                if (sa > 0)
                    d[lx] = blend_px(d[lx], sp, sa);
            }
            mid0 = rad;
            mid1 = w - rad;
        }
        int n = mid1 - mid0;
        if (n > 0) {
            int px0 = dx0 + mid0;
            if (px0 < v.x) {
                int skip = v.x - px0;
                mid0 += skip;
                n -= skip;
                px0 = v.x;
            }
            if (px0 + n > v.x + v.w)
                n = v.x + v.w - px0;
            if (n > 0) {
                if (c->alpha >= 255)
                    copy_run(d + (px0 - dx0), s + (px0 - dx0), n);
                else
                    blend_run(d + (px0 - dx0), s + (px0 - dx0), n, c->alpha);
            }
        }
    }
}

static void exec_roundfill(const struct GPU_CMD *c) {
    struct GFX_RECT r = {c->x, c->y, c->w, c->h}, v;
    if (!clip_rect(&v, &r) || !gpu_dst)
        return;
    int w = c->w, h = c->h, rad = c->rad;
    if (rad < 0)
        rad = 0;
    if (rad * 2 > w)
        rad = w / 2;
    if (rad * 2 > h)
        rad = h / 2;
    int ca = gfx_div255(GFX_A(c->color) * c->alpha + 127);
    if (ca <= 0)
        return;
    gfx_color rgb = c->color & 0x00FFFFFFu;
    int top_band = (c->corners & (GFX_CORNER_TL | GFX_CORNER_TR)) ? rad : 0;
    int bot_band = (c->corners & (GFX_CORNER_BL | GFX_CORNER_BR)) ? rad : 0;
    size_t mapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    for (int row = 0; row < v.h; row++) {
        int py = v.y + row;
        int ly = py - c->y;
        size_t off = (size_t)py * (size_t)gpu_dst->pitch + (size_t)v.x * 4u;
        if (off + (size_t)v.w * 4u > mapped)
            return;
        gfx_color *p = gfx_px_at(gpu_dst, off);
        int full_row = (ly >= top_band && ly < h - bot_band);
        if (full_row && ca >= 255) {
            for (int col = 0; col < v.w; col++)
                p[col] = rgb | 0xFF000000u;
            continue;
        }
        for (int col = 0; col < v.w; col++) {
            int lx = v.x + col - c->x;
            int cov;
            if (full_row)
                cov = 255;
            else
                cov = gfx_round_coverage(lx, ly, w, h, rad, c->corners);
            if (cov <= 0)
                continue;
            int a = gfx_div255(ca * cov);
            if (a >= 255)
                p[col] = rgb | 0xFF000000u;
            else
                p[col] = gfx_over(p[col], rgb, a);
        }
    }
}

static void exec_shadow(const struct GPU_CMD *c) {
    if (!gpu_dst || !c->src || !c->src->pixels || c->alpha <= 0)
        return;
    int blur = c->rad;
    int rad = c->corners;
    int fw = c->w, fh = c->h;
    struct GFX_CANVAS *sp = c->src;
    int sw = sp->w, shh = sp->h;
    int dx0 = c->x, dy0 = c->y;
    struct GFX_RECT r = {dx0, dy0, sw, shh}, v;
    if (!clip_rect(&v, &r))
        return;
    size_t dmapped = gfx_canvas_mapped_bytes(gpu_dst, &(int){0});
    size_t smapped = gfx_canvas_mapped_bytes(sp, &(int){0});
    int cx0 = v.x - dx0, cx1 = v.x + v.w - dx0;
    if (cx0 < 0)
        cx0 = 0;
    if (cx1 > sw)
        cx1 = sw;
    for (int ly = 0; ly < shh; ly++) {
        int py = dy0 + ly;
        if (py < v.y || py >= v.y + v.h)
            continue;
        size_t soff = (size_t)ly * (size_t)sp->pitch + (size_t)cx0 * 4u;
        size_t doff = (size_t)py * (size_t)gpu_dst->pitch +
                      (size_t)(dx0 + cx0) * 4u;
        int span = cx1 - cx0;
        if (span <= 0)
            continue;
        if (soff + (size_t)span * 4u > smapped ||
            doff + (size_t)span * 4u > dmapped)
            return;
        const gfx_color *s =
            gfx_px_at_c(sp, soff);
        gfx_color *d =
            gfx_px_at(gpu_dst, doff);
        if (ly >= blur && ly < blur + fh) {
            int lend = (cx1 < blur) ? cx1 : blur;
            if (cx0 < lend)
                blend_run(d, s, lend - cx0, c->alpha);
            int r0 = (cx0 > fw + blur) ? cx0 : fw + blur;
            if (r0 < cx1)
                blend_run(d + (r0 - cx0), s + (r0 - cx0), cx1 - r0, c->alpha);

            if (ly < blur + rad || ly >= blur + fh - rad) {
                int spans[2][2] = {
                    {blur, blur + rad},
                    {blur + fw - rad, blur + fw},
                };
                for (int si = 0; si < 2; si++) {
                    int s0 = spans[si][0], s1 = spans[si][1];
                    if (s0 < cx0)
                        s0 = cx0;
                    if (s1 > cx1)
                        s1 = cx1;
                    if (s0 < s1)
                        blend_run(d + (s0 - cx0), s + (s0 - cx0), s1 - s0,
                                  c->alpha);
                }
            }
        } else {
            blend_run(d, s, span, c->alpha);
        }
    }
}

static void exec_cmd(const struct GPU_CMD *c) {
    switch (c->op) {
    case GPU_OP_FILL:
        exec_fill(c);
        break;
    case GPU_OP_COPY:
        exec_copy(c);
        break;
    case GPU_OP_BLEND:
        exec_blend(c);
        break;
    case GPU_OP_ROUND_BLEND:
        exec_round_blend(c);
        break;
    case GPU_OP_ROUNDFILL:
        exec_roundfill(c);
        break;
    case GPU_OP_SHADOW:
        exec_shadow(c);
        break;
    default:
        break;
    }
}

void gpu_set_target(struct GFX_CANVAS *c) {
    gpu_dst = c;
    gpu_clip.x = 0;
    gpu_clip.y = 0;
    gpu_clip.w = c ? c->w : 0;
    gpu_clip.h = c ? c->h : 0;
}

struct GFX_CANVAS *gpu_target(void) {
    return gpu_dst;
}

void gpu_batch_begin(void) {
    cmd_n = 0;
}

void gpu_batch_clip(const struct GFX_RECT *r) {
    if (!r) {
        gpu_clip.x = 0;
        gpu_clip.y = 0;
        gpu_clip.w = gpu_dst ? gpu_dst->w : 0;
        gpu_clip.h = gpu_dst ? gpu_dst->h : 0;
        return;
    }
    gpu_clip = *r;
}

void gpu_push(const struct GPU_CMD *c) {
    if (!c || c->w <= 0 || c->h <= 0)
        return;
    if (cmd_n >= GPU_QUEUE)
        gpu_batch_flush();
    if (cmd_n >= GPU_QUEUE)
        return;
    cmd_queue[cmd_n++] = *c;
}

void gpu_batch_flush(void) {
    for (int i = 0; i < cmd_n; i++)
        exec_cmd(&cmd_queue[i]);
    cmd_n = 0;
}
