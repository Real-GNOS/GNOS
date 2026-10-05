#include "lib/ttf/ttf.h"

#define TTF_TAG_CMAP 0x636D6170u
#define TTF_TAG_GLYF 0x676C7966u
#define TTF_TAG_HEAD 0x68656164u
#define TTF_TAG_HHEA 0x68686561u
#define TTF_TAG_LOCA 0x6C6F6361u
#define TTF_TAG_MAXP 0x6D617870u

#define TTF_SFNT_1 0x00010000u
#define TTF_SFNT_TRUE 0x74727565u

#define TTF_FX 0x10000
#define TTF_SUB 4
#define TTF_SUB_SHIFT 12
#define TTF_SUB_UNIT 16
#define TTF_SUB_MAX 64
#define TTF_HALF_SUB 8192
#define TTF_COV_MAX 255

#define TTF_PT_MAX 320
#define TTF_EDGE_MAX 768
#define TTF_CURVE_MAX 8
#define TTF_DEPTH_MAX 3
#define TTF_COMP_MAX 8
#define TTF_TAB_MAX 64

#define TTF_ASCII_FIRST 0x20
#define TTF_ASCII_LAST 0x7F
#define TTF_ASCII_COUNT (TTF_ASCII_LAST - TTF_ASCII_FIRST)
#define TTF_BOX_BYTES (TTF_BOX_W * TTF_BOX_H)

#define TTF_CMAP_FMT4 4
#define TTF_CMAP_FMT12 12

#define TTF_ON 0x01
#define TTF_XSHORT 0x02
#define TTF_YSHORT 0x04
#define TTF_REPEAT 0x08
#define TTF_XSAME 0x10
#define TTF_YSAME 0x20

#define TTF_ARG_WORDS 0x0001
#define TTF_ARGS_XY 0x0002
#define TTF_HAVE_SCALE 0x0008
#define TTF_MORE 0x0020
#define TTF_HAVE_XY_SCALE 0x0040
#define TTF_HAVE_2X2 0x0080

struct TTF_PT {
    int32_t x;
    int32_t y;
    uint8_t on;
};

struct TTF_EDGE {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
};

struct TTF_FONT {
    const uint8_t *loca;
    const uint8_t *glyf;
    const uint8_t *cmap;
    uint32_t glyf_len;
    uint32_t cmap_len;
    uint32_t num_glyphs;
    uint16_t cmap_fmt;
    int loca_long;
    int32_t scale;
    int baseline;
};

static struct TTF_FONT ttf_font;
static struct TTF_EDGE ttf_edges[TTF_EDGE_MAX];
static uint16_t ttf_map[256];
static uint8_t ttf_pool[TTF_ASCII_COUNT * TTF_BOX_BYTES];
static uint8_t ttf_zero[TTF_BOX_BYTES];
static uint8_t ttf_scratch[TTF_BOX_BYTES];
static int32_t ttf_scratch_cp = -1;
static int ttf_nedges;
static int ttf_state;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int16_t rds16(const uint8_t *p) {
    return (int16_t)rd16(p);
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int32_t iabs32(int32_t v) {
    return v < 0 ? -v : v;
}

static const uint8_t *ttf_find(const uint8_t *d, uint32_t len, uint32_t tag,
                               uint32_t *out_len) {
    uint32_t ver;
    uint32_t n;
    if (len < 12)
        return 0;
    ver = rd32(d);
    if (ver != TTF_SFNT_1 && ver != TTF_SFNT_TRUE)
        return 0;
    n = rd16(d + 4);
    if (n > TTF_TAB_MAX)
        n = TTF_TAB_MAX;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ro = 12 + i * 16;
        uint32_t off;
        uint32_t l;
        if (ro + 16 > len)
            break;
        if (rd32(d + ro) != tag)
            continue;
        off = rd32(d + ro + 8);
        l = rd32(d + ro + 12);
        if (off >= len)
            return 0;
        if (l > len - off)
            l = len - off;
        *out_len = l;
        return d + off;
    }
    return 0;
}

static uint32_t ttf_cmap4(const uint8_t *t, uint32_t cp) {
    uint32_t segx2;
    uint32_t seg;
    const uint8_t *end;
    const uint8_t *start;
    const uint8_t *delta;
    const uint8_t *range;
    if (cp > 0xFFFFu)
        return 0;
    segx2 = rd16(t + 6);
    seg = segx2 >> 1;
    end = t + 14;
    start = end + segx2 + 2;
    delta = start + segx2;
    range = delta + segx2;
    for (uint32_t i = 0; i < seg; i++) {
        if (cp <= rd16(end + i * 2)) {
            uint32_t s = rd16(start + i * 2);
            uint32_t ro;
            if (cp < s)
                return 0;
            ro = rd16(range + i * 2);
            if (ro == 0)
                return (uint32_t)((cp + (uint32_t)rds16(delta + i * 2)) & 0xFFFFu);
            return (uint32_t)((rd16(range + i * 2 + ro + (cp - s) * 2) +
                               (uint32_t)rds16(delta + i * 2)) & 0xFFFFu);
        }
    }
    return 0;
}

static uint32_t ttf_cmap12(const uint8_t *t, uint32_t cp) {
    uint32_t n = rd32(t + 12);
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        const uint8_t *g = t + 16 + mid * 12;
        uint32_t s = rd32(g);
        uint32_t e = rd32(g + 4);
        if (cp < s)
            hi = mid;
        else if (cp > e)
            lo = mid + 1;
        else
            return rd32(g + 8) + (cp - s);
    }
    return 0;
}

static uint32_t ttf_lookup(uint32_t cp) {
    if (!ttf_font.cmap)
        return 0;
    if (ttf_font.cmap_fmt == TTF_CMAP_FMT12)
        return ttf_cmap12(ttf_font.cmap, cp);
    return ttf_cmap4(ttf_font.cmap, cp);
}

static const uint8_t *ttf_cmap_pick(const uint8_t *cm, uint32_t len,
                                    uint16_t *fmt) {
    const uint8_t *best12 = 0;
    const uint8_t *best4 = 0;
    uint32_t n;
    if (len < 4)
        return 0;
    n = rd16(cm + 2);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ro = 4 + i * 8;
        uint32_t off;
        const uint8_t *st;
        uint16_t f;
        if (ro + 8 > len)
            break;
        off = rd32(cm + ro + 4);
        if (off + 2 > len)
            continue;
        st = cm + off;
        f = rd16(st);
        if (f == TTF_CMAP_FMT12 && !best12)
            best12 = st;
        else if (f == TTF_CMAP_FMT4 && !best4)
            best4 = st;
    }
    if (best12) {
        *fmt = TTF_CMAP_FMT12;
        return best12;
    }
    if (best4) {
        *fmt = TTF_CMAP_FMT4;
        return best4;
    }
    return 0;
}

static const uint8_t *ttf_glyph_data(uint32_t gid, uint32_t *plen) {
    uint32_t o0;
    uint32_t o1;
    if (gid >= ttf_font.num_glyphs)
        return 0;
    if (ttf_font.loca_long) {
        o0 = rd32(ttf_font.loca + gid * 4);
        o1 = rd32(ttf_font.loca + gid * 4 + 4);
    } else {
        o0 = (uint32_t)rd16(ttf_font.loca + gid * 2) * 2;
        o1 = (uint32_t)rd16(ttf_font.loca + gid * 2 + 2) * 2;
    }
    if (o1 <= o0 || o1 > ttf_font.glyf_len)
        return 0;
    *plen = o1 - o0;
    return ttf_font.glyf + o0;
}

static void ttf_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    struct TTF_EDGE *e;
    if (y0 == y1 || ttf_nedges >= TTF_EDGE_MAX)
        return;
    e = &ttf_edges[ttf_nedges++];
    e->x0 = x0;
    e->y0 = y0;
    e->x1 = x1;
    e->y1 = y1;
}

static void ttf_quad(int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                     int32_t x1, int32_t y1) {
    int32_t span = iabs32(cx - x0) + iabs32(cy - y0) + iabs32(x1 - cx) +
                   iabs32(y1 - cy);
    int n = (int)(span >> 17);
    int32_t px = x0;
    int32_t py = y0;
    if (n < 2)
        n = 2;
    if (n > TTF_CURVE_MAX)
        n = TTF_CURVE_MAX;
    for (int i = 1; i <= n; i++) {
        int32_t t = (int32_t)(((int64_t)i * TTF_FX) / n);
        int32_t mt = TTF_FX - t;
        int32_t a = (int32_t)(((int64_t)mt * mt) >> 16);
        int32_t b = (int32_t)(((int64_t)2 * mt * t) >> 16);
        int32_t c = (int32_t)(((int64_t)t * t) >> 16);
        int32_t qx = (int32_t)(((int64_t)a * x0 + (int64_t)b * cx +
                                (int64_t)c * x1) >> 16);
        int32_t qy = (int32_t)(((int64_t)a * y0 + (int64_t)b * cy +
                                (int64_t)c * y1) >> 16);
        ttf_edge(px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

static void ttf_emit(struct TTF_PT *pts, int s, int e) {
    struct TTF_PT norm[TTF_PT_MAX];
    struct TTF_PT rot[TTF_PT_MAX];
    int cnt = e - s + 1;
    int m = 0;
    int k0 = -1;
    int32_t cx;
    int32_t cy;
    int i;
    if (cnt < 2 || cnt > TTF_PT_MAX / 2)
        return;
    for (i = 0; i < cnt; i++) {
        int j = s + i;
        int k = s + (i + 1 == cnt ? 0 : i + 1);
        if (m >= TTF_PT_MAX)
            return;
        norm[m++] = pts[j];
        if (!pts[j].on && !pts[k].on) {
            if (m >= TTF_PT_MAX)
                return;
            norm[m].x = (pts[j].x + pts[k].x) >> 1;
            norm[m].y = (pts[j].y + pts[k].y) >> 1;
            norm[m].on = 1;
            m++;
        }
    }
    for (i = 0; i < m; i++) {
        if (norm[i].on) {
            k0 = i;
            break;
        }
    }
    if (k0 < 0)
        return;
    for (i = 0; i < m; i++)
        rot[i] = norm[(k0 + i) % m];
    cx = rot[0].x;
    cy = rot[0].y;
    i = 1;
    while (i < m) {
        if (rot[i].on) {
            ttf_edge(cx, cy, rot[i].x, rot[i].y);
            cx = rot[i].x;
            cy = rot[i].y;
            i++;
        } else {
            int j = (i + 1) % m;
            ttf_quad(cx, cy, rot[i].x, rot[i].y, rot[j].x, rot[j].y);
            cx = rot[j].x;
            cy = rot[j].y;
            i += 2;
        }
    }
    ttf_edge(cx, cy, rot[0].x, rot[0].y);
}

static void ttf_simple(const uint8_t *g, uint32_t glen, int32_t tx, int32_t ty,
                       int32_t m00, int32_t m01, int32_t m10, int32_t m11) {
    struct TTF_PT pts[TTF_PT_MAX];
    uint16_t ends[TTF_PT_MAX / 2];
    uint8_t flags[TTF_PT_MAX];
    const uint8_t *p;
    const uint8_t *end;
    int nc;
    int npts;
    int n;
    int32_t v;
    int s;
    if (glen < 10)
        return;
    nc = rds16(g);
    if (nc <= 0 || nc > (int)(TTF_PT_MAX / 2))
        return;
    p = g + 10;
    end = g + glen;
    if (p + nc * 2 + 2 > end)
        return;
    npts = 0;
    for (int i = 0; i < nc; i++) {
        ends[i] = rd16(p);
        p += 2;
        npts = ends[i] + 1;
    }
    if (npts <= 0 || npts > (int)TTF_PT_MAX)
        return;
    p += 2 + rd16(p);
    if (p > end)
        return;
    n = 0;
    while (n < npts) {
        uint8_t f;
        if (p >= end)
            return;
        f = *p++;
        flags[n++] = f;
        if (f & TTF_REPEAT) {
            uint8_t r;
            if (p >= end)
                return;
            r = *p++;
            while (r-- > 0 && n < npts)
                flags[n++] = f;
        }
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t f = flags[i];
        if (f & TTF_XSHORT) {
            int32_t dx;
            if (p >= end)
                return;
            dx = *p++;
            v += (f & TTF_XSAME) ? dx : -dx;
        } else if (!(f & TTF_XSAME)) {
            if (p + 2 > end)
                return;
            v += rds16(p);
            p += 2;
        }
        pts[i].x = v;
        pts[i].on = (f & TTF_ON) ? 1u : 0u;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t f = flags[i];
        if (f & TTF_YSHORT) {
            int32_t dy;
            if (p >= end)
                return;
            dy = *p++;
            v += (f & TTF_YSAME) ? dy : -dy;
        } else if (!(f & TTF_YSAME)) {
            if (p + 2 > end)
                return;
            v += rds16(p);
            p += 2;
        }
        pts[i].y = v;
    }
    for (int i = 0; i < npts; i++) {
        int32_t fx = (int32_t)((int64_t)pts[i].x * ttf_font.scale);
        int32_t fy = (int32_t)((int64_t)pts[i].y * ttf_font.scale);
        int32_t rx = (int32_t)(((int64_t)m00 * fx + (int64_t)m01 * fy) >> 16) + tx;
        int32_t ry = (int32_t)(((int64_t)m10 * fx + (int64_t)m11 * fy) >> 16) + ty;
        pts[i].x = rx;
        pts[i].y = (int32_t)((int32_t)(ttf_font.baseline << 16) - ry);
    }
    s = 0;
    for (int c = 0; c < nc; c++) {
        int e = ends[c];
        if (e >= npts)
            e = npts - 1;
        ttf_emit(pts, s, e);
        s = e + 1;
    }
}

static void ttf_glyph(uint32_t gid, int depth, int32_t tx, int32_t ty,
                      int32_t m00, int32_t m01, int32_t m10, int32_t m11) {
    uint32_t glen = 0;
    const uint8_t *g = ttf_glyph_data(gid, &glen);
    const uint8_t *p;
    const uint8_t *end;
    uint16_t flags = TTF_MORE;
    int guard = 0;
    if (!g || glen < 10 || depth > TTF_DEPTH_MAX)
        return;
    if (rds16(g) >= 0) {
        ttf_simple(g, glen, tx, ty, m00, m01, m10, m11);
        return;
    }
    p = g + 10;
    end = g + glen;
    while ((flags & TTF_MORE) && guard++ < TTF_COMP_MAX) {
        uint32_t cgid;
        int32_t a1;
        int32_t a2;
        int32_t c00 = TTF_FX;
        int32_t c11 = TTF_FX;
        int32_t c01 = 0;
        int32_t c10 = 0;
        if (p + 4 > end)
            return;
        flags = rd16(p);
        cgid = rd16(p + 2);
        p += 4;
        if (flags & TTF_ARG_WORDS) {
            if (p + 4 > end)
                return;
            a1 = rds16(p);
            a2 = rds16(p + 2);
            p += 4;
        } else {
            if (p + 2 > end)
                return;
            a1 = (int8_t)p[0];
            a2 = (int8_t)p[1];
            p += 2;
        }
        if (flags & TTF_HAVE_SCALE) {
            if (p + 2 > end)
                return;
            c00 = c11 = (int32_t)((int16_t)rd16(p)) << 2;
            p += 2;
        } else if (flags & TTF_HAVE_XY_SCALE) {
            if (p + 4 > end)
                return;
            c00 = (int32_t)((int16_t)rd16(p)) << 2;
            c11 = (int32_t)((int16_t)rd16(p + 2)) << 2;
            p += 4;
        } else if (flags & TTF_HAVE_2X2) {
            if (p + 8 > end)
                return;
            c00 = (int32_t)((int16_t)rd16(p)) << 2;
            c01 = (int32_t)((int16_t)rd16(p + 2)) << 2;
            c10 = (int32_t)((int16_t)rd16(p + 4)) << 2;
            c11 = (int32_t)((int16_t)rd16(p + 6)) << 2;
            p += 8;
        }
        if (!(flags & TTF_ARGS_XY))
            continue;
        {
            int32_t dpx = (int32_t)((int64_t)a1 * ttf_font.scale);
            int32_t dpy = (int32_t)((int64_t)a2 * ttf_font.scale);
            int32_t ntx = (int32_t)(((int64_t)m00 * dpx + (int64_t)m01 * dpy) >>
                                    16) + tx;
            int32_t nty = (int32_t)(((int64_t)m10 * dpx + (int64_t)m11 * dpy) >>
                                    16) + ty;
            int32_t n00 = (int32_t)(((int64_t)m00 * c00 + (int64_t)m01 * c10) >>
                                    16);
            int32_t n01 = (int32_t)(((int64_t)m00 * c01 + (int64_t)m01 * c11) >>
                                    16);
            int32_t n10 = (int32_t)(((int64_t)m10 * c00 + (int64_t)m11 * c10) >>
                                    16);
            int32_t n11 = (int32_t)(((int64_t)m10 * c01 + (int64_t)m11 * c11) >>
                                    16);
            ttf_glyph(cgid, depth + 1, ntx, nty, n00, n01, n10, n11);
        }
    }
}

static void ttf_span(int32_t *acc, int32_t xa, int32_t xb) {
    int32_t limit = TTF_BOX_W << 16;
    int i0;
    int i1;
    if (xb <= 0 || xa >= limit)
        return;
    if (xa < 0)
        xa = 0;
    if (xb > limit)
        xb = limit;
    i0 = (int)(xa >> 16);
    i1 = (int)(xb >> 16);
    if (i0 == i1) {
        acc[i0] += (int32_t)((xb - xa) >> TTF_SUB_SHIFT);
        return;
    }
    acc[i0] += (int32_t)((((i0 + 1) << 16) - xa) >> TTF_SUB_SHIFT);
    for (int i = i0 + 1; i < i1; i++)
        acc[i] += TTF_SUB_UNIT;
    if (i1 < TTF_BOX_W)
        acc[i1] += (int32_t)((xb - (i1 << 16)) >> TTF_SUB_SHIFT);
}

static void ttf_raster(uint8_t *mask) {
    int32_t xs[TTF_EDGE_MAX];
    int8_t ws[TTF_EDGE_MAX];
    for (int row = 0; row < TTF_BOX_H; row++) {
        int32_t acc[TTF_BOX_W];
        for (int i = 0; i < TTF_BOX_W; i++)
            acc[i] = 0;
        for (int s = 0; s < TTF_SUB; s++) {
            int32_t y = ((int32_t)row << 16) + (int32_t)((s * 2 + 1) * TTF_HALF_SUB);
            int n = 0;
            int wind = 0;
            for (int e = 0; e < ttf_nedges; e++) {
                struct TTF_EDGE *E = &ttf_edges[e];
                int32_t y0 = E->y0;
                int32_t y1 = E->y1;
                int dir;
                if (y0 < y1) {
                    if (y < y0 || y >= y1)
                        continue;
                    dir = 1;
                } else {
                    if (y < y1 || y >= y0)
                        continue;
                    dir = -1;
                }
                xs[n] = E->x0 + (int32_t)(((int64_t)(y - y0) * (E->x1 - E->x0)) /
                                          (E->y1 - E->y0));
                ws[n] = (int8_t)dir;
                n++;
                if (n >= TTF_EDGE_MAX)
                    break;
            }
            for (int i = 1; i < n; i++) {
                int32_t xv = xs[i];
                int8_t wv = ws[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > xv) {
                    xs[j + 1] = xs[j];
                    ws[j + 1] = ws[j];
                    j--;
                }
                xs[j + 1] = xv;
                ws[j + 1] = wv;
            }
            for (int i = 0; i + 1 < n; i++) {
                wind += ws[i];
                if (wind != 0 && xs[i + 1] > xs[i])
                    ttf_span(acc, xs[i], xs[i + 1]);
            }
        }
        for (int i = 0; i < TTF_BOX_W; i++) {
            int a = (int)(acc[i] * TTF_COV_MAX / TTF_SUB_MAX);
            if (a > TTF_COV_MAX)
                a = TTF_COV_MAX;
            mask[row * TTF_BOX_W + i] = (uint8_t)a;
        }
    }
}

static void ttf_build(uint32_t gid, uint8_t *mask) {
    ttf_nedges = 0;
    if (gid)
        ttf_glyph(gid, 0, 0, 0, TTF_FX, 0, 0, TTF_FX);
    if (ttf_nedges)
        ttf_raster(mask);
    else
        for (int i = 0; i < TTF_BOX_BYTES; i++)
            mask[i] = 0;
}

int ttf_console_init(const void *data, uint32_t len) {
    const uint8_t *d = (const uint8_t *)data;
    const uint8_t *head;
    const uint8_t *hhea;
    const uint8_t *maxp;
    const uint8_t *loca;
    const uint8_t *glyf;
    const uint8_t *cmap;
    uint32_t l = 0;
    uint32_t cmap_len;
    uint32_t glyf_len;
    uint32_t upm;
    int32_t desc;
    int base;
    if (ttf_state)
        return 0;
    if (!d || len < 64)
        return -1;
    head = ttf_find(d, len, TTF_TAG_HEAD, &l);
    hhea = ttf_find(d, len, TTF_TAG_HHEA, &l);
    maxp = ttf_find(d, len, TTF_TAG_MAXP, &l);
    loca = ttf_find(d, len, TTF_TAG_LOCA, &l);
    cmap = ttf_find(d, len, TTF_TAG_CMAP, &l);
    cmap_len = l;
    glyf = ttf_find(d, len, TTF_TAG_GLYF, &l);
    glyf_len = l;
    if (!head || !hhea || !maxp || !loca || !cmap || !glyf)
        return -1;
    ttf_font.loca = loca;
    ttf_font.glyf = glyf;
    ttf_font.glyf_len = glyf_len;
    ttf_font.num_glyphs = rd16(maxp + 4);
    ttf_font.loca_long = rds16(head + 50) != 0;
    cmap = ttf_cmap_pick(cmap, cmap_len, &ttf_font.cmap_fmt);
    if (!cmap)
        return -1;
    ttf_font.cmap = cmap;
    ttf_font.cmap_len = cmap_len;
    upm = rd16(head + 18);
    if (upm < 16 || upm > 16384)
        upm = 1000;
    ttf_font.scale = (int32_t)(((uint32_t)TTF_PX << 16) / upm);
    desc = rds16(hhea + 6);
    base = TTF_BOX_H - (int)(-((int32_t)(((int64_t)desc * ttf_font.scale) >> 16)));
    if (base < 1)
        base = 1;
    if (base > TTF_BOX_H - 1)
        base = TTF_BOX_H - 1;
    ttf_font.baseline = base;
    for (uint32_t ch = 0; ch < 256; ch++)
        ttf_map[ch] = (uint16_t)ttf_lookup(ch);
    for (int ch = TTF_ASCII_FIRST; ch < TTF_ASCII_LAST; ch++)
        ttf_build(ttf_map[ch],
                  &ttf_pool[(ch - TTF_ASCII_FIRST) * TTF_BOX_BYTES]);
    ttf_state = 1;
    return 0;
}

int ttf_ready(void) {
    return ttf_state;
}

const uint8_t *ttf_mask(uint8_t ch) {
    if (!ttf_state)
        return ttf_zero;
    if (ch >= TTF_ASCII_FIRST && ch < TTF_ASCII_LAST)
        return &ttf_pool[(ch - TTF_ASCII_FIRST) * TTF_BOX_BYTES];
    if (ch < TTF_ASCII_FIRST)
        return ttf_zero;
    if (ttf_scratch_cp != (int32_t)ch) {
        ttf_scratch_cp = (int32_t)ch;
        ttf_build(ttf_map[ch], ttf_scratch);
    }
    return ttf_scratch;
}