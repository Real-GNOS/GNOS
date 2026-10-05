#include "kernel/gui/font.h"
#include "arch/cpu.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/sync.h"
#include "lib/str/str.h"
static float ft_fabs(float x) {
    return x < 0 ? -x : x;
}

static int ft_ifloor(float x) {
    int i = (int)x;
    return ((float)i > x) ? i - 1 : i;
}

static int ft_iceil(float x) {
    int i = (int)x;
    return ((float)i < x) ? i + 1 : i;
}

static float ft_sqrt(float x) {
    if (x <= 0)
        return 0;
    float g = x;
    for (int i = 0; i < 32; i++) {
        float ng = 0.5f * (g + x / g);
        if (ng == g)
            break;
        g = ng;
    }
    return g;
}

static float ft_fmod(float x, float y) {
    if (y == 0)
        return 0;
    int q = (int)(x / y);
    float r = x - (float)q * y;
    if (r < 0)
        r += y;
    if (r >= y)
        r -= y;
    return r;
}

static float ft_cuberoot(float x) {
    if (x == 0)
        return 0;
    int neg = x < 0;
    float v = neg ? -x : x;
    float g = v;
    for (int i = 0; i < 40; i++) {
        float ng = (2.0f * g + v / (g * g)) / 3.0f;
        if (ng == g)
            break;
        g = ng;
    }
    return neg ? -g : g;
}

static float ft_pow(float b, float e) {
    if (b == 0)
        return 0;
    if (e == 1.0f / 3.0f)
        return b < 0 ? -ft_cuberoot(-b) : ft_cuberoot(b);
    int ie = (int)e;
    if ((float)ie == e) {
        float r = 1, base = b;
        int n = ie < 0 ? -ie : ie;
        for (int i = 0; i < n; i++)
            r *= base;
        if (ie < 0 && r != 0)
            r = 1.0f / r;
        return r;
    }
    return 0;
}

static float ft_cos(float x) {
    const float pi = 3.14159265f;
    while (x > pi)
        x -= 2 * pi;
    while (x < -pi)
        x += 2 * pi;
    float x2 = x * x;
    return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f +
           x2 * x2 * x2 * x2 / 40320.0f - x2 * x2 * x2 * x2 * x2 / 3628800.0f;
}

static float ft_acos(float x) {
    const float pi = 3.14159265f;
    if (x >= 1.0f)
        return 0;
    if (x <= -1.0f)
        return pi;
    float a = x < 0 ? -x : x;
    float a2 = a * a, a3 = a2 * a;
    float as = a + a3 / 6.0f + (3.0f / 40.0f) * a3 * a2 +
               (5.0f / 112.0f) * a3 * a2 * a2;
    float r = pi / 2.0f - as;
    return x < 0 ? pi - r : r;
}

#define GLYPH_ARENA_PAGES 512
#define ARENA_ALIGN 16
struct GUI_ARENA_HDR {
    size_t size;
    struct GUI_ARENA_HDR *next;
};
static uint8_t *g_arena;
static size_t g_arena_size, g_arena_bump;
static struct GUI_ARENA_HDR *g_freelist;
static void *arena_alloc(size_t n) {
    if (!g_arena)
        return 0;
    n = (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
    struct GUI_ARENA_HDR **pp = &g_freelist;
    while (*pp) {
        if ((*pp)->size >= n) {
            struct GUI_ARENA_HDR *b = *pp;
            *pp = b->next;
            return (void *)(b + 1);
        }
        pp = &(*pp)->next;
    }
    if (g_arena_bump + n + sizeof(struct GUI_ARENA_HDR) > g_arena_size)
        return 0;
    struct GUI_ARENA_HDR *b = (struct GUI_ARENA_HDR *)(void *)(g_arena + g_arena_bump);
    b->size = n;
    g_arena_bump += n + sizeof(struct GUI_ARENA_HDR);
    return (void *)(b + 1);
}

static void arena_release(void *p) {
    if (!p)
        return;
    struct GUI_ARENA_HDR *b = ((struct GUI_ARENA_HDR *)p) - 1;
    b->next = g_freelist;
    g_freelist = b;
}

#define STBTT_ifloor(x) ft_ifloor(x)
#define STBTT_iceil(x) ft_iceil(x)
#define STBTT_sqrt(x) ft_sqrt(x)
#define STBTT_pow(x, y) ft_pow((x), (y))
#define STBTT_fmod(x, y) ft_fmod((x), (y))
#define STBTT_cos(x) ft_cos(x)
#define STBTT_acos(x) ft_acos(x)
#define STBTT_fabs(x) ft_fabs(x)
#define STBTT_malloc(x, u) arena_alloc(x)
#define STBTT_free(x, u) arena_release(x)
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"
#define FONT_SLOTS 512
struct GUI_FONT_GLYPH {
    int32_t cp;
    int px;
    int w, h, ox, oy;
    int advance;
    uint8_t *bm;
    uint32_t lru;
};
static struct GUI_FONT_GLYPH g_glyphs[FONT_SLOTS];
static stbtt_fontinfo g_fi;
static int g_ready;
static uint32_t g_clock;
static struct SCHED_LOCK g_font_lock;
static int g_lock_ready;
static uint8_t g_fpu_buf[512] __attribute__((aligned(64)));
static uint32_t fpu_enter(void) {
    uint32_t e = cpu_eflags();
    cpu_cli();
    __asm__ volatile("fxsave (%0)" ::"r"(g_fpu_buf) : "memory");
    return e;
}

static void fpu_leave(uint32_t e) {
    __asm__ volatile("fxrstor (%0)" ::"r"(g_fpu_buf) : "memory");
    cpu_set_eflags(e);
}

static void cache_reset(void) {
    memset(g_glyphs, 0, sizeof(g_glyphs));
    g_arena_bump = 0;
    g_freelist = 0;
    g_clock = 0;
}

static struct GUI_FONT_GLYPH *glyph_fill(struct GUI_FONT_GLYPH *g, int32_t cp, int px) {
    uint32_t ef = fpu_enter();
    float scale = stbtt_ScaleForPixelHeight(&g_fi, (float)px);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_fi, cp, &adv, &lsb);
    int w = 0, h = 0, ox = 0, oy = 0;
    unsigned char *bm =
        stbtt_GetCodepointBitmap(&g_fi, scale, scale, cp, &w, &h, &ox, &oy);
    uint8_t *dst = 0;
    if (bm && w > 0 && h > 0) {
        size_t need = (size_t)w * (size_t)h;
        dst = (uint8_t *)arena_alloc(need);
        if (!dst) {
            cache_reset();
            dst = (uint8_t *)arena_alloc(need);
        }
        if (dst)
            memcpy(dst, bm, need);
        else
            w = h = 0;
    }
    if (bm)
        stbtt_FreeBitmap(bm, 0);
    fpu_leave(ef);
    g->cp = cp;
    g->px = px;
    g->w = w;
    g->h = h;
    g->ox = ox;
    g->oy = oy;
    g->advance = (int)((float)adv * scale + 0.5f);
    if (g->advance < 0)
        g->advance = 0;
    g->bm = dst;
    g->lru = ++g_clock;
    return g;
}

static struct GUI_FONT_GLYPH *glyph_get(int32_t cp, int px) {
    uint32_t h =
        (((uint32_t)cp * 2654435761u) ^ ((uint32_t)px * 40503u)) &
        (FONT_SLOTS - 1);
    for (int i = 0; i < FONT_SLOTS; i++) {
        struct GUI_FONT_GLYPH *g = &g_glyphs[(h + i) & (FONT_SLOTS - 1)];
        if (g->cp == cp && g->px == px) {
            g->lru = ++g_clock;
            return g;
        }
    }
    for (int i = 0; i < FONT_SLOTS; i++) {
        struct GUI_FONT_GLYPH *g = &g_glyphs[(h + i) & (FONT_SLOTS - 1)];
        if (g->cp == 0)
            return glyph_fill(g, cp, px);
    }
    struct GUI_FONT_GLYPH *victim = &g_glyphs[0];
    for (int i = 1; i < FONT_SLOTS; i++)
        if (g_glyphs[i].lru < victim->lru)
            victim = &g_glyphs[i];
    if (victim->bm)
        arena_release(victim->bm);
    victim->cp = 0;
    victim->bm = 0;
    return glyph_fill(victim, cp, px);
}

int font_utf8_next(const char **sp) {
    const unsigned char *p = (const unsigned char *)*sp;
    int c = p[0];
    if (c == 0)
        return 0;
    if (c < 0x80) {
        *sp += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) == 0x80) {
            *sp += 2;
            return ((c & 0x1F) << 6) | (p[1] & 0x3F);
        }
    } else if ((c & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            *sp += 3;
            return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        }
    } else if ((c & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
            (p[3] & 0xC0) == 0x80) {
            *sp += 4;
            return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                   ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        }
    }
    *sp += 1;
    return c;
}

int font_init(const void *ttf_data, int ttf_len) {
    if (!ttf_data || ttf_len <= 0)
        return 0;
    if (!g_lock_ready) {
        lock_init(&g_font_lock);
        g_lock_ready = 1;
    }
    if (!g_arena) {
        void *mem = get_kernel_pages(GLYPH_ARENA_PAGES);
        if (!mem)
            return 0;
        g_arena = (uint8_t *)mem;
        g_arena_size = (size_t)GLYPH_ARENA_PAGES * PAGE_SIZE;
    }
    cache_reset();
    stbtt_fontinfo tmp;
    if (!stbtt_InitFont(&tmp, (const unsigned char *)ttf_data, 0))
        return 0;
    g_fi = tmp;
    g_ready = 1;
    return 1;
}

int font_ready(void) {
    return g_ready;
}

int font_ascent(int px) {
    if (!g_ready)
        return px;
    lock_acquire(&g_font_lock);
    uint32_t ef = fpu_enter();
    float scale = stbtt_ScaleForPixelHeight(&g_fi, (float)px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&g_fi, &asc, &desc, &gap);
    fpu_leave(ef);
    lock_release(&g_font_lock);
    int a = (int)((float)asc * scale + 0.5f);
    return a > 0 ? a : px;
}

int font_line_height(int px) {
    if (!g_ready)
        return px + 2;
    lock_acquire(&g_font_lock);
    uint32_t ef = fpu_enter();
    float scale = stbtt_ScaleForPixelHeight(&g_fi, (float)px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&g_fi, &asc, &desc, &gap);
    fpu_leave(ef);
    lock_release(&g_font_lock);
    int lh = (int)((float)(asc - desc + gap) * scale + 0.5f);
    return lh > 0 ? lh : px + 2;
}

int font_text_width(const char *utf8, int px) {
    if (!g_ready || !utf8)
        return 0;
    lock_acquire(&g_font_lock);
    uint32_t ef = fpu_enter();
    float scale = stbtt_ScaleForPixelHeight(&g_fi, (float)px);
    int w = 0, prev = 0;
    const char *s = utf8;
    while (*s) {
        int cp = font_utf8_next(&s);
        if (cp <= 0)
            break;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_fi, cp, &adv, &lsb);
        if (prev)
            w += (int)(stbtt_GetCodepointKernAdvance(&g_fi, prev, cp) * scale +
                       0.5f);
        w += (int)((float)adv * scale + 0.5f);
        prev = cp;
    }
    fpu_leave(ef);
    lock_release(&g_font_lock);
    return w;
}

static int font_draw_impl(struct GFX_CANVAS *c, int x, int y, const char *utf8,
                          int px, gfx_color fg, const struct GFX_RECT *clip) {
    if (!g_ready || !c || !c->pixels || !utf8)
        return x;
    lock_acquire(&g_font_lock);
    uint32_t ef = fpu_enter();
    float scale = stbtt_ScaleForPixelHeight(&g_fi, (float)px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&g_fi, &asc, &desc, &gap);
    int ybase = y + (int)((float)asc * scale + 0.5f);
    gfx_color base = (gfx_color)((fg & 0x00FFFFFFu) | 0xFF000000u);
    int fa = GFX_A(fg);
    int penx = x;
    int prev = 0;
    const char *s = utf8;
    while (*s) {
        int cp = font_utf8_next(&s);
        if (cp <= 0)
            break;
        struct GUI_FONT_GLYPH *g = glyph_get(cp, px);
        if (prev)
            penx += (int)(stbtt_GetCodepointKernAdvance(&g_fi, prev, cp) *
                              scale +
                          0.5f);
        if (g->bm && g->w > 0 && g->h > 0) {
            int px0 = penx + g->ox;
            int py0 = ybase + g->oy;
            for (int gy = 0; gy < g->h; gy++) {
                int py = py0 + gy;
                if (py < 0 || py >= c->h)
                    continue;
                if (clip && (py < clip->y || py >= clip->y + clip->h))
                    continue;
                gfx_color *row = gfx_row(c, py);
                const uint8_t *sr = g->bm + (size_t)gy * (size_t)g->w;
                for (int gx = 0; gx < g->w; gx++) {
                    int pxx = px0 + gx;
                    if (pxx < 0 || pxx >= c->w)
                        continue;
                    if (clip && (pxx < clip->x || pxx >= clip->x + clip->w))
                        continue;
                    int cov = sr[gx];
                    if (!cov)
                        continue;
                    int a = gfx_div255(fa * cov);
                    if (a <= 0)
                        continue;
                    if (a >= 255)
                        row[pxx] = base;
                    else
                        row[pxx] = gfx_over(row[pxx], base, a);
                }
            }
        }
        penx += g->advance;
        prev = cp;
    }
    fpu_leave(ef);
    lock_release(&g_font_lock);
    return penx;
}

int font_draw(struct GFX_CANVAS *c, int x, int y, const char *utf8, int px,
              gfx_color fg) {
    return font_draw_impl(c, x, y, utf8, px, fg, 0);
}

int font_draw_clip(struct GFX_CANVAS *c, int x, int y, const char *utf8, int px,
                   gfx_color fg, const struct GFX_RECT *clip) {
    if (!clip)
        return font_draw_impl(c, x, y, utf8, px, fg, 0);
    struct GFX_RECT cv = {0, 0, c->w, c->h}, v;
    if (!gfx_rect_intersect(*clip, cv, &v))
        return x;
    return font_draw_impl(c, x, y, utf8, px, fg, &v);
}
