#include "libc/user/stdio.h"
#include "lib/str/str.h"
#include "syscall.h"

struct FS_STAT {
    uint32_t st_ino;
    uint32_t st_size;
    int32_t st_filetype;
};

static int my_ifloor(float x) {
    int i = (int)x;
    return ((float)i > x) ? i - 1 : i;
}

static int my_iceil(float x) {
    int i = (int)x;
    return ((float)i < x) ? i + 1 : i;
}

static float my_fabsf(float x) {
    return x < 0 ? -x : x;
}

static float my_sqrtf(float x) {
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

static float my_fmodf(float x, float y) {
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

static float my_cosf(float x) {
    const float pi = 3.14159265f;
    while (x > pi)
        x -= 2 * pi;
    while (x < -pi)
        x += 2 * pi;
    float x2 = x * x;
    return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f +
           x2 * x2 * x2 * x2 / 40320.0f - x2 * x2 * x2 * x2 * x2 / 3628800.0f;
}

static float my_acosf(float x) {
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

static float my_cuberoot(float x) {
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

static float my_powf(float b, float e) {
    if (b == 0)
        return 0;
    if (e == 1.0f / 3.0f) {
        return b < 0 ? -my_cuberoot(-b) : my_cuberoot(b);
    }
    int ie = (int)e;
    if ((float)ie == e) {
        float r = 1, base = b;
        int n = ie < 0 ? -ie : ie;
        for (int i = 0; i < n; i++)
            r *= base;
        if (ie < 0)
            r = 1.0f / r;
        return r;
    }
    return 0;
}

#define STBTT_ifloor(x) my_ifloor(x)
#define STBTT_iceil(x) my_iceil(x)
#define STBTT_sqrt(x) my_sqrtf(x)
#define STBTT_pow(x, y) my_powf((x), (y))
#define STBTT_fmod(x, y) my_fmodf((x), (y))
#define STBTT_cos(x) my_cosf(x)
#define STBTT_acos(x) my_acosf(x)
#define STBTT_fabs(x) my_fabsf(x)

static char g_heap[512 * 1024];
static uint32_t g_heap_used = 0;
static void *my_malloc(int size) {
    g_heap_used = (g_heap_used + 3u) & ~3u;
    if (g_heap_used + (uint32_t)size > sizeof(g_heap))
        return 0;
    void *p = g_heap + g_heap_used;
    g_heap_used += (uint32_t)size;
    return p;
}

#define STBTT_malloc(x, u) my_malloc(x)
#define STBTT_free(x, u) ((void)0)
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "lib/stb_truetype.h"

struct BOOT_INFO {
    uint8_t cyls, leds, vmode, pad;
    uint16_t scrnx, scrny;
    uint32_t vram;
};

static unsigned char g_font[64 * 1024];

static void blit_gray(const unsigned char *bm, int w, int h, uint8_t *vram,
                      int pitch, int sx0, int sy0, int scrnx, int scrny) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int sx = sx0 + xx, sy = sy0 + yy;
            if (sx < 0 || sx >= scrnx || sy < 0 || sy >= scrny)
                continue;
            unsigned int a = bm[yy * w + xx];
            uint8_t c;
            if (a >= 200)
                c = 15;
            else if (a >= 100)
                c = 7;
            else if (a >= 40)
                c = 8;
            else
                c = 0;
            vram[sy * pitch + sx] = c;
        }
    }
}

static int utf8_decode(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0];
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        int cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        int cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3;
        return cp;
    }
    *s += 1;
    return c;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct BOOT_INFO *bi = (struct BOOT_INFO *)0x0FF0;
    uint8_t *vram = (uint8_t *)bi->vram;
    int scrnx = bi->scrnx, scrny = bi->scrny;
    int pitch = scrnx;

    struct FS_STAT st;
    if (stat("/font_subset.ttf", &st) == -1) {
        printf("font: /font_subset.ttf not found\n");
        exit(-1);
    }
    if (st.st_size > (int)sizeof(g_font)) {
        printf("font: too large %d\n", (int)st.st_size);
        exit(-1);
    }
    int fd = open("/font_subset.ttf", 0);
    if (fd == -1) {
        printf("font: open failed\n");
        exit(-1);
    }
    int got = read(fd, g_font, (uint32_t)sizeof(g_font));
    close(fd);
    if (got <= 0) {
        printf("font: read failed\n");
        exit(-1);
    }

    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, g_font, 0)) {
        printf("font: init failed\n");
        exit(-1);
    }

    const int pixel_height = 48;
    float scale = stbtt_ScaleForPixelHeight(&fi, (float)pixel_height);

    static const char *lines[] = {"A", "B", "Pipe: A|B <-> C",
                                  "0123456789 !@#$%^&*()", NULL};

    int x = 50, y = 80;
    for (int li = 0; lines[li] != NULL; li++) {
        x = 50;
        const char *s = lines[li];
        while (*s) {
            int cp = utf8_decode(&s);
            int w = 0, h = 0, ox = 0, oy = 0;
            unsigned char *bm = stbtt_GetCodepointBitmap(&fi, scale, scale, cp,
                                                         &w, &h, &ox, &oy);
            if (bm) {
                blit_gray(bm, w, h, vram, pitch, x + ox, y + oy, scrnx, scrny);
            }
            int ax = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&fi, cp, &ax, &lsb);
            x += (int)((float)ax * scale) + 6;
        }
        y += pixel_height + 18;
    }

    printf("font rendered, press Ctrl+C to exit\n");

    for (;;) {
    }
    return 0;
}
