#ifndef GUI_GFX_H
#define GUI_GFX_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t gfx_color;
#define GFX_RGBA(r, g, b, a)                                                   \
    ((gfx_color)((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) |           \
                 (((uint32_t)(g)) << 8) | ((uint32_t)(b))))
#define GFX_RGB(r, g, b) GFX_RGBA((r), (g), (b), 0xFFu)
#define GFX_A(c) ((int)(((c) >> 24) & 0xFFu))
#define GFX_R(c) ((int)(((c) >> 16) & 0xFFu))
#define GFX_G(c) ((int)(((c) >> 8) & 0xFFu))
#define GFX_B(c) ((int)((c) & 0xFFu))
static inline int gfx_div255(int x) {
    uint32_t n = (uint32_t)(x < 0 ? -x : x);
    uint32_t q = (n * 0x8081u) >> 23;
    return x < 0 ? -(int)q : (int)q;
}
static inline gfx_color gfx_alpha_mul(gfx_color c, int a) {
    if (a >= 255)
        return c;
    if (a <= 0)
        return c & 0x00FFFFFFu;
    int na = gfx_div255(GFX_A(c) * a + 127);
    return (c & 0x00FFFFFFu) | ((uint32_t)na << 24);
}

struct GFX_CANVAS {
    gfx_color *pixels;
    int pitch;
    int w, h;
    size_t bytes;
};

static inline int gfx_stride(const struct GFX_CANVAS *c) {
    return c->pitch >> 2;
}

static inline gfx_color *gfx_row(struct GFX_CANVAS *c, int y) {
    return (gfx_color *)(void *)((uint8_t *)c->pixels + (size_t)y *
                                                           (size_t)c->pitch);
}

static inline const gfx_color *gfx_row_c(const struct GFX_CANVAS *c, int y) {
    return (const gfx_color *)(const void *)((const uint8_t *)c->pixels +
                                             (size_t)y * (size_t)c->pitch);
}

static inline gfx_color *gfx_px_at(struct GFX_CANVAS *c, size_t off) {
    return (gfx_color *)(void *)((uint8_t *)c->pixels + off);
}
static inline const gfx_color *gfx_px_at_c(const struct GFX_CANVAS *c,
                                           size_t off) {
    return (const gfx_color *)(const void *)((const uint8_t *)c->pixels + off);
}
static inline size_t gfx_canvas_mapped_bytes(const struct GFX_CANVAS *c,
                                             int *ok) {
    *ok = 1;
    if (c->bytes > 0)
        return c->bytes;
    size_t s;
    if (__builtin_mul_overflow((size_t)c->pitch, (size_t)c->h, &s)) {
        *ok = 0;
        return 0;
    }
    return s;
}

struct GFX_RECT {
    int x, y, w, h;
};

struct GFX_FB_FORMAT {
    int bpp;
    int r_pos, r_bits;
    int g_pos, g_bits;
    int b_pos, b_bits;
};
void gfx_set_fb_format(const struct GFX_FB_FORMAT *fmt);
int gfx_fb_bpp(void);

gfx_color gfx_over(gfx_color dst, gfx_color src, int alpha);

void gfx_px(struct GFX_CANVAS *c, int x, int y, gfx_color color);

void gfx_fill(struct GFX_CANVAS *c, int x, int y, int w, int h, gfx_color color);
void gfx_rect(struct GFX_CANVAS *c, int x, int y, int w, int h, gfx_color color);
void gfx_hline(struct GFX_CANVAS *c, int x, int y, int len, gfx_color color);
void gfx_vline(struct GFX_CANVAS *c, int x, int y, int len, gfx_color color);

void gfx_blit(struct GFX_CANVAS *dst, int dx, int dy,
              const struct GFX_CANVAS *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(struct GFX_CANVAS *dst, int dx, int dy,
                    const struct GFX_CANVAS *src, int sx, int sy, int w, int h,
                    int alpha);

#define GFX_CORNER_TL 1
#define GFX_CORNER_TR 2
#define GFX_CORNER_BL 4
#define GFX_CORNER_BR 8
#define GFX_CORNER_ALL                                                         \
    (GFX_CORNER_TL | GFX_CORNER_TR | GFX_CORNER_BL | GFX_CORNER_BR)

uint8_t gfx_round_coverage(int px, int py, int w, int h, int rad, int corners);
void gfx_fill_round(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                    gfx_color color);
void gfx_fill_round_a(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                      gfx_color color, int alpha);
void gfx_mask_round(struct GFX_CANVAS *c, int x, int y, int w, int h, int rad,
                    gfx_color color, int corners);
void gfx_blit_round(struct GFX_CANVAS *dst, int dx, int dy,
                    const struct GFX_CANVAS *src, int sx, int sy, int w, int h,
                    int alpha, int rx, int ry, int rw, int rh, int rad,
                    int corners);
int gfx_rect_intersect(struct GFX_RECT a, struct GFX_RECT b,
                       struct GFX_RECT *out);
void gfx_blit_scale(struct GFX_CANVAS *dst, int dx, int dy, int dw, int dh,
                    const struct GFX_CANVAS *src, int sx, int sy, int sw,
                    int sh);
void gfx_present(struct GFX_CANVAS *dst, int dx, int dy,
                 const struct GFX_CANVAS *src, int sx, int sy, int w, int h);

#endif
