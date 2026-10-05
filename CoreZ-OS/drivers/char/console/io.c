#include "drivers/char/console/io.h"

#include <stdarg.h>

#include "kernel/asm_func.h"
#include "kernel/sched/sync.h"
#include "lib/ttf/ttf.h"

extern const unsigned char _binary_font_kernel_ttf_start[];
extern const unsigned char _binary_font_kernel_ttf_end[];

static uint8_t *vram = (uint8_t *)0;
static int scrnx = 0;
static int scrny = 0;
static int pitch = 0;
static int bpp = 8;
static int bpp_bytes = 1;
static size_t vram_bytes = 0;
static int cursor_x = 0;
static int cursor_y = -20;
static uint32_t text_color = 0;
static int gui_active = 0;

static const uint32_t ansi16[16] = {
    0xFF000000u, 0xFFCD0000u, 0xFF00CD00u, 0xFFCDCD00u, 0xFF0000EEu,
    0xFFCD00CDu, 0xFF00CDCDu, 0xFFE5E5E5u, 0xFF7F7F7Fu, 0xFFFF0000u,
    0xFF00FF00u, 0xFFFFFF00u, 0xFF5C5CFFu, 0xFFFF00FFu, 0xFF00FFFFu,
    0xFFFFFFFFu};
uint32_t io_ansi_color(int idx) {
    return ansi16[idx & 15];
}

uint8_t *io_get_vram(void) {
    return vram;
}

int io_get_scrnx(void) {
    return scrnx;
}

int io_get_scrny(void) {
    return scrny;
}

int io_get_pitch(void) {
    return pitch;
}

int io_get_bpp(void) {
    return bpp;
}

size_t io_get_vram_bytes(void) {
    return vram_bytes;
}

void io_set_gui_active(int on) {
    gui_active = on;
    if (!on) {
        cursor_x = 0;
        cursor_y = 0;
    }
}

#define PRINTF_LINE_GAP 20

#define VT_CELL_W 8
#define VT_CELL_H PRINTF_LINE_GAP
#define VT_MAX_ARGS 8

#define VT_IDLE 0
#define VT_ESC 1
#define VT_CSI 2
#define VT_OSC 3
#define VT_DCS 4
#define VT_SKIP 5

static uint8_t vt_state = VT_IDLE;
static uint8_t vt_priv;
static uint8_t vt_nargs;
static uint8_t vt_esc_pending;
static int32_t vt_args[VT_MAX_ARGS];
static int32_t vt_cur;
static uint32_t vt_fg;
static uint32_t vt_bg;
static int vt_save_x;
static int vt_save_y;

static void vt_reset_attrs(void) {
    vt_fg = ansi16[7];
    vt_bg = ansi16[0];
}

void io_init(uint8_t *vram_base, int width, int height, uint32_t bytes,
             int fb_pitch, int fb_bpp) {
    vram = vram_base;
    scrnx = width;
    scrny = height;
    bpp = (fb_bpp > 0) ? fb_bpp : 8;
    bpp_bytes = bpp / 8;
    if (bpp_bytes < 1)
        bpp_bytes = 1;
    pitch = (fb_pitch > 0) ? fb_pitch : width * bpp_bytes;
    vram_bytes = (bytes > 0) ? (size_t)bytes
                             : (size_t)pitch * (size_t)height;
    cursor_x = 0;
    cursor_y = 0;
    text_color = ansi16[7];
    vt_reset_attrs();
    if (!ttf_ready())
        ttf_console_init(_binary_font_kernel_ttf_start,
                         (uint32_t)(_binary_font_kernel_ttf_end -
                                    _binary_font_kernel_ttf_start));
}

void set_text_color(int color) {
    text_color = ansi16[color & 15];
}

void set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
}

int get_cursor_x(void) {
    return cursor_x;
}

int get_cursor_y(void) {
    return cursor_y;
}

#define DEBUG_CONSOLE_PORT 0xE9

static void vram_shift_up(int line_bytes) {
    int total_bytes = scrny * pitch;
    if ((pitch & 3) == 0 && ((uintptr_t)vram & 3) == 0) {
        uint32_t *dw = (uint32_t *)vram;
        int line_dw = line_bytes / 4;
        int total_dw = total_bytes / 4;
        for (int i = line_dw; i < total_dw; i++)
            dw[i - line_dw] = dw[i];
        for (int i = total_dw - line_dw; i < total_dw; i++)
            dw[i] = 0;
    } else {
        for (int i = line_bytes; i < total_bytes; i++)
            vram[i - line_bytes] = vram[i];
        for (int i = total_bytes - line_bytes; i < total_bytes; i++)
            vram[i] = 0;
    }
}

static void vram_zero_all(void) {
    int total_bytes = scrny * pitch;
    if ((pitch & 3) == 0 && ((uintptr_t)vram & 3) == 0) {
        uint32_t *dw = (uint32_t *)vram;
        int total_dw = total_bytes / 4;
        for (int i = 0; i < total_dw; i++)
            dw[i] = 0;
    } else {
        for (int i = 0; i < total_bytes; i++)
            vram[i] = 0;
    }
}

static void scroll_screen(void) {
    if (vram == 0 || scrnx <= 0 || scrny <= 0 || pitch <= 0)
        return;
    vram_shift_up(PRINTF_LINE_GAP * pitch);
    cursor_y -= PRINTF_LINE_GAP;
}

static void putc(char c) {
    if (gui_active) {
        if (c == '\n')
            outb(DEBUG_CONSOLE_PORT, (uint8_t)'\r');
        outb(DEBUG_CONSOLE_PORT, (uint8_t)c);
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x >= 8) {
            cursor_x -= 8;
        }
    } else if (c == '\n') {
        cursor_y += PRINTF_LINE_GAP;
        cursor_x = 0;
        if (cursor_y + PRINTF_LINE_GAP > scrny) {
            scroll_screen();
        }
    } else {
        show_char(vram, pitch, cursor_x, cursor_y, scrnx, scrny, c, text_color,
                  0);
        cursor_x += 8;

        if (cursor_x + 8 > scrnx) {
            cursor_y += PRINTF_LINE_GAP;
            cursor_x = 0;
            if (cursor_y + PRINTF_LINE_GAP > scrny) {
                scroll_screen();
            }
        }
    }
    outb(DEBUG_CONSOLE_PORT, (uint8_t)c);
}

static void print_unsigned(uint32_t v, int base, int upper, int width, int pad0,
                           int hexPrefix) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char buf[33];
    int n = 0;

    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v) {
            buf[n++] = digits[v % base];
            v /= base;
        }
    }

    int body = (hexPrefix ? 2 : 0) + n;
    int pad = (width > body) ? (width - body) : 0;

    if (pad0) {
        if (hexPrefix) {
            putc('0');
            putc(upper ? 'X' : 'x');
        }
        while (pad--)
            putc('0');
    } else {
        while (pad--)
            putc(' ');
        if (hexPrefix) {
            putc('0');
            putc(upper ? 'X' : 'x');
        }
    }
    while (n--)
        putc(buf[n]);
}

static void print_signed(int v, int width, int pad0) {
    unsigned int uv = (unsigned int)v;
    int neg = 0;
    char buf[12];
    int n = 0;

    if (v < 0) {
        neg = 1;
        uv = 0u - uv;
    }
    if (uv == 0) {
        buf[n++] = '0';
    } else {
        while (uv) {
            buf[n++] = '0' + uv % 10;
            uv /= 10;
        }
    }

    int body = neg + n;
    int pad = (width > body) ? (width - body) : 0;

    if (pad0) {
        if (neg)
            putc('-');
        while (pad--)
            putc('0');
    } else {
        while (pad--)
            putc(' ');
        if (neg)
            putc('-');
    }
    while (n--)
        putc(buf[n]);
}

static struct SCHED_SPINLOCK console_lock;

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t eflags = (uint32_t)asm_save_eflags();
    asm_cli();
    spinlock_acquire(&console_lock);

    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            putc(*fmt);
            continue;
        }
        ++fmt;

        int pad0 = 0, hex_pre = 0, width = 0;
        for (;;) {
            if (*fmt == '0') {
                pad0 = 1;
                ++fmt;
            } else if (*fmt == '#') {
                hex_pre = 1;
                ++fmt;
            } else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        if (*fmt == 'l')
            ++fmt;

        switch (*fmt) {
        case 'd':
            print_signed(va_arg(ap, int), width, pad0);
            break;
        case 'u':
            print_unsigned(va_arg(ap, unsigned int), 10, 0, width, pad0, 0);
            break;
        case 'x':
            print_unsigned(va_arg(ap, unsigned int), 16, 0, width, pad0,
                           hex_pre);
            break;
        case 'X':
            print_unsigned(va_arg(ap, unsigned int), 16, 1, width, pad0,
                           hex_pre);
            break;
        case 'o':
            print_unsigned(va_arg(ap, unsigned int), 8, 0, width, pad0, 0);
            break;
        case 'c':
            putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                putc(*s++);
            break;
        }
        case '%':
            putc('%');
            break;
        case '\0':
            --fmt;
            break;
        default:
            putc('%');
            putc(*fmt);
            break;
        }
    }
    va_end(ap);
    spinlock_release(&console_lock);
    asm_restore_eflags(eflags);
}

void console_putc(char c) {
    putc(c);
}

void io_clear_screen(void) {
    vram_zero_all();
    cursor_x = 0;
    cursor_y = 0;
    text_color = ansi16[7];
    vt_reset_attrs();
    vt_state = VT_IDLE;
    vt_nargs = 0;
    vt_esc_pending = 0;
}

static inline void store_px(uint8_t *p, uint32_t color) {
    if (bpp_bytes == 4) {
        *(uint32_t *)p = color;
    } else if (bpp_bytes == 2) {
        *(uint16_t *)p = (uint16_t)color;
    } else if (bpp_bytes == 3) {
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    } else {
        *p = (uint8_t)color;
    }
}

static uint32_t load_px(const uint8_t *p) {
    if (bpp_bytes == 4)
        return *(const uint32_t *)p;
    if (bpp_bytes == 2)
        return *(const uint16_t *)p;
    if (bpp_bytes == 3)
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return *p;
}

void show_char(uint8_t *vram_ptr, int p, int x, int y, int sw, int sh, char c,
               uint32_t color, int bg) {
    const uint8_t *cov;
    int fr;
    int fg;
    int fb;
    int br = 0;
    int bgc = 0;
    int bb = 0;
    if (!vram_ptr)
        return;
    cov = ttf_mask((uint8_t)c);
    fr = (int)((color >> 16) & 0xFFu);
    fg = (int)((color >> 8) & 0xFFu);
    fb = (int)(color & 0xFFu);
    if (bg >= 0) {
        br = (bg >> 16) & 0xFF;
        bgc = (bg >> 8) & 0xFF;
        bb = bg & 0xFF;
    }

    for (int row = 0; row < TTF_BOX_H; row++) {
        int py = y + row;
        if (py < 0 || py >= sh)
            continue;
        for (int col = 0; col < TTF_BOX_W; col++) {
            int px = x + col;
            int a = cov[row * TTF_BOX_W + col];
            uint8_t *dst;
            if (px < 0 || px >= sw)
                continue;
            dst = vram_ptr + (size_t)py * (size_t)p +
                  (size_t)px * (size_t)bpp_bytes;
            if (bg >= 0) {
                int r = br + (((fr - br) * a) >> 8);
                int g = bgc + (((fg - bgc) * a) >> 8);
                int b = bb + (((fb - bb) * a) >> 8);
                store_px(dst, (uint32_t)((r << 16) | (g << 8) | b));
            } else if (a >= 255) {
                store_px(dst, color);
            } else if (a > 0) {
                if (bpp_bytes == 4) {
                    uint32_t o = load_px(dst);
                    int r = (int)((o >> 16) & 0xFFu);
                    int g = (int)((o >> 8) & 0xFFu);
                    int b = (int)(o & 0xFFu);
                    r += ((fr - r) * a) >> 8;
                    g += ((fg - g) * a) >> 8;
                    b += ((fb - b) * a) >> 8;
                    store_px(dst, (uint32_t)((r << 16) | (g << 8) | b));
                } else if (a >= 128) {
                    store_px(dst, color);
                }
            }
        }
    }
}

void show_string(uint8_t *vram_ptr, int p, int x, int y, int sw, int sh,
                 const char *s, uint32_t color, int bg) {
    while (*s) {
        show_char(vram_ptr, p, x, y, sw, sh, *s, color, bg);
        x += 8;
        s++;
    }
}

static void vt_fill(int x, int y, int w, int h, uint32_t color) {
    if (vram == 0 || w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > scrnx)
        w = scrnx - x;
    if (y + h > scrny)
        h = scrny - y;
    for (int yy = 0; yy < h; yy++) {
        uint8_t *row = vram + (size_t)(y + yy) * (size_t)pitch +
                       (size_t)x * (size_t)bpp_bytes;
        for (int xx = 0; xx < w; xx++)
            store_px(row + (size_t)xx * (size_t)bpp_bytes, color);
    }
}

static void vt_clamp_x(void) {
    int max = scrnx - VT_CELL_W;
    if (max < 0)
        max = 0;
    if (cursor_x < 0)
        cursor_x = 0;
    if (cursor_x > max)
        cursor_x = max;
}

static void vt_clamp_y(void) {
    int max = scrny - VT_CELL_H;
    if (max < 0)
        max = 0;
    if (cursor_y < 0)
        cursor_y = 0;
    if (cursor_y > max)
        cursor_y = max;
}

static void vt_lf(void) {
    cursor_y += VT_CELL_H;
    if (cursor_y + VT_CELL_H > scrny) {
        scroll_screen();
        vt_fill(0, scrny - VT_CELL_H, scrnx, VT_CELL_H, vt_bg);
    }
    vt_clamp_y();
}

static void vt_scroll_up_lines(int n) {
    int shift = n * VT_CELL_H;
    if (shift <= 0 || scrnx <= 0 || scrny <= 0)
        return;
    if (shift >= scrny) {
        vt_fill(0, 0, scrnx, scrny, vt_bg);
        return;
    }
    vram_shift_up(shift * pitch);
    vt_fill(0, scrny - shift, scrnx, shift, vt_bg);
}

static void vt_scroll_down_lines(int n) {
    int shift = n * VT_CELL_H;
    int total;
    if (shift <= 0 || scrnx <= 0 || scrny <= 0 || vram == 0)
        return;
    if (shift >= scrny) {
        vt_fill(0, 0, scrnx, scrny, vt_bg);
        return;
    }
    total = scrny * pitch;
    for (int i = total - 1; i >= shift; i--)
        vram[i] = vram[i - shift];
    vt_fill(0, 0, scrnx, shift, vt_bg);
}

static void vt_erase_line(int mode) {
    if (mode == 1)
        vt_fill(0, cursor_y, cursor_x + VT_CELL_W, VT_CELL_H, vt_bg);
    else if (mode == 2)
        vt_fill(0, cursor_y, scrnx, VT_CELL_H, vt_bg);
    else
        vt_fill(cursor_x, cursor_y, scrnx - cursor_x, VT_CELL_H, vt_bg);
}

static void vt_erase_display(int mode) {
    if (mode == 1) {
        vt_fill(0, 0, scrnx, cursor_y, vt_bg);
        vt_erase_line(1);
    } else if (mode == 2) {
        vt_fill(0, 0, scrnx, scrny, vt_bg);
        cursor_x = 0;
        cursor_y = 0;
    } else {
        vt_erase_line(0);
        vt_fill(0, cursor_y + VT_CELL_H, scrnx,
                scrny - cursor_y - VT_CELL_H, vt_bg);
    }
}

static void vt_glyph(char c) {
    vt_fill(cursor_x, cursor_y, VT_CELL_W, VT_CELL_H, vt_bg);
    show_char(vram, pitch, cursor_x, cursor_y, scrnx, scrny, c, vt_fg, -1);
    cursor_x += VT_CELL_W;
    if (cursor_x + VT_CELL_W > scrnx) {
        cursor_x = 0;
        vt_lf();
    }
}

static int32_t vt_pal_idx(uint32_t c) {
    for (int32_t i = 0; i < 16; i++)
        if (ansi16[i] == c)
            return i;
    return -1;
}

static uint32_t vt_xterm256(int32_t n) {
    static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
    uint32_t v;
    if (n < 0)
        n = 0;
    if (n < 16)
        return ansi16[n];
    if (n < 232) {
        n -= 16;
        return 0xFF000000u | ((uint32_t)lv[n / 36] << 16) |
               ((uint32_t)lv[(n / 6) % 6] << 8) | (uint32_t)lv[n % 6];
    }
    if (n > 255)
        n = 255;
    v = 8u + (uint32_t)(n - 232) * 10u;
    return 0xFF000000u | (v << 16) | (v << 8) | v;
}

static void vt_sgr(void) {
    for (uint8_t i = 0; i < vt_nargs; i++) {
        int32_t a = vt_args[i];
        int32_t k;
        if (a == 0) {
            vt_reset_attrs();
        } else if (a == 1) {
            k = vt_pal_idx(vt_fg);
            if (k >= 0 && k < 8)
                vt_fg = ansi16[k + 8];
        } else if (a == 2) {
            vt_fg = ansi16[8];
        } else if (a == 22) {
            k = vt_pal_idx(vt_fg);
            if (k >= 8 && k < 16)
                vt_fg = ansi16[k - 8];
        } else if (a >= 30 && a <= 37) {
            vt_fg = ansi16[a - 30];
        } else if (a == 39) {
            vt_fg = ansi16[7];
        } else if (a >= 40 && a <= 47) {
            vt_bg = ansi16[a - 40];
        } else if (a == 49) {
            vt_bg = ansi16[0];
        } else if (a >= 90 && a <= 97) {
            vt_fg = ansi16[a - 90 + 8];
        } else if (a >= 100 && a <= 107) {
            vt_bg = ansi16[a - 100 + 8];
        } else if (a == 38 || a == 48) {
            uint32_t *dst = (a == 38) ? &vt_fg : &vt_bg;
            if (i + 2 < vt_nargs && vt_args[i + 1] == 5) {
                *dst = vt_xterm256(vt_args[i + 2]);
                i += 2;
            } else if (i + 4 < vt_nargs && vt_args[i + 1] == 2) {
                *dst = 0xFF000000u |
                       ((uint32_t)(vt_args[i + 2] & 0xFF) << 16) |
                       ((uint32_t)(vt_args[i + 3] & 0xFF) << 8) |
                       (uint32_t)(vt_args[i + 4] & 0xFF);
                i += 4;
            }
        }
    }
}

static void vt_mode(int set) {
    (void)set;
    for (uint8_t i = 0; i < vt_nargs; i++) {
        if (vt_args[i] == 1049) {
            vt_fill(0, 0, scrnx, scrny, vt_bg);
            cursor_x = 0;
            cursor_y = 0;
        }
    }
}

static void vt_csi_dispatch(char f) {
    int32_t p0 = vt_nargs > 0 ? vt_args[0] : 0;
    int32_t n = p0 > 0 ? p0 : 1;
    switch (f) {
    case 'A':
        cursor_y -= n * VT_CELL_H;
        vt_clamp_y();
        break;
    case 'B':
    case 'e':
        cursor_y += n * VT_CELL_H;
        vt_clamp_y();
        break;
    case 'C':
    case 'a':
        cursor_x += n * VT_CELL_W;
        vt_clamp_x();
        break;
    case 'D':
        cursor_x -= n * VT_CELL_W;
        vt_clamp_x();
        break;
    case 'E':
        cursor_x = 0;
        cursor_y += n * VT_CELL_H;
        vt_clamp_y();
        break;
    case 'F':
        cursor_x = 0;
        cursor_y -= n * VT_CELL_H;
        vt_clamp_y();
        break;
    case 'G':
        cursor_x = (n - 1) * VT_CELL_W;
        vt_clamp_x();
        break;
    case 'd':
        cursor_y = (n - 1) * VT_CELL_H;
        vt_clamp_y();
        break;
    case 'H':
    case 'f': {
        int32_t row = vt_nargs > 0 && vt_args[0] > 0 ? vt_args[0] : 1;
        int32_t col = vt_nargs > 1 && vt_args[1] > 0 ? vt_args[1] : 1;
        cursor_y = (row - 1) * VT_CELL_H;
        cursor_x = (col - 1) * VT_CELL_W;
        vt_clamp_x();
        vt_clamp_y();
        break;
    }
    case 'J':
        vt_erase_display(p0 > 2 ? 2 : p0);
        break;
    case 'K':
        vt_erase_line(p0 > 2 ? 2 : p0);
        break;
    case 'X':
        vt_fill(cursor_x, cursor_y, n * VT_CELL_W, VT_CELL_H, vt_bg);
        break;
    case 'm':
        vt_sgr();
        break;
    case 's':
        vt_save_x = cursor_x;
        vt_save_y = cursor_y;
        break;
    case 'u':
        if (!vt_priv) {
            cursor_x = vt_save_x;
            cursor_y = vt_save_y;
            vt_clamp_x();
            vt_clamp_y();
        }
        break;
    case 'S':
        vt_scroll_up_lines(n);
        break;
    case 'T':
        vt_scroll_down_lines(n);
        break;
    case 'h':
        vt_mode(1);
        break;
    case 'l':
        vt_mode(0);
        break;
    default:
        break;
    }
}

static void vt_arg_push(void) {
    if (vt_nargs < VT_MAX_ARGS)
        vt_args[vt_nargs++] = vt_cur;
    vt_cur = 0;
}

static void vt_feed(char c) {
    switch (vt_state) {
    case VT_IDLE:
        if (c == 0x1b) {
            vt_state = VT_ESC;
        } else if (c == '\n' || c == '\v' || c == '\f') {
            cursor_x = 0;
            vt_lf();
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\b') {
            cursor_x -= VT_CELL_W;
            if (cursor_x < 0)
                cursor_x = 0;
        } else if (c == '\t') {
            int col = (cursor_x / VT_CELL_W + 8) & ~7;
            cursor_x = col * VT_CELL_W;
            vt_clamp_x();
        } else if (c == '\a' || c == 0x7f) {
        } else if ((uint8_t)c >= 0x20) {
            vt_glyph(c);
        }
        return;
    case VT_ESC:
        if (c == '[') {
            vt_state = VT_CSI;
            vt_nargs = 0;
            vt_cur = 0;
            vt_priv = 0;
        } else if (c == ']') {
            vt_state = VT_OSC;
            vt_esc_pending = 0;
        } else if (c == 'P') {
            vt_state = VT_DCS;
            vt_esc_pending = 0;
        } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' ||
                   c == '%' || c == ' ') {
            vt_state = VT_SKIP;
        } else if (c == '7') {
            vt_save_x = cursor_x;
            vt_save_y = cursor_y;
            vt_state = VT_IDLE;
        } else if (c == '8') {
            cursor_x = vt_save_x;
            cursor_y = vt_save_y;
            vt_clamp_x();
            vt_clamp_y();
            vt_state = VT_IDLE;
        } else if (c == 'D') {
            cursor_x = 0;
            vt_lf();
            vt_state = VT_IDLE;
        } else if (c == 'M') {
            if (cursor_y <= 0)
                vt_scroll_down_lines(1);
            else {
                cursor_y -= VT_CELL_H;
                vt_clamp_y();
            }
            vt_state = VT_IDLE;
        } else if (c == 'E') {
            cursor_x = 0;
            vt_lf();
            vt_state = VT_IDLE;
        } else if (c == 'c') {
            vt_fill(0, 0, scrnx, scrny, vt_bg);
            cursor_x = 0;
            cursor_y = 0;
            vt_reset_attrs();
            vt_state = VT_IDLE;
        } else {
            vt_state = VT_IDLE;
        }
        return;
    case VT_CSI:
        if (c == 0x1b) {
            vt_state = VT_ESC;
        } else if (c >= '0' && c <= '9') {
            vt_cur = vt_cur * 10 + (c - '0');
            if (vt_cur > 9999)
                vt_cur = 9999;
        } else if (c == ';' || c == ':') {
            vt_arg_push();
        } else if (c == '?' || c == '<' || c == '=' || c == '>') {
            vt_priv = (uint8_t)c;
        } else if (c >= 0x20 && c <= 0x2f) {
        } else {
            vt_arg_push();
            vt_csi_dispatch(c);
            vt_state = VT_IDLE;
        }
        return;
    case VT_OSC:
    case VT_DCS:
        if (c == 0x07) {
            vt_state = VT_IDLE;
        } else if (vt_esc_pending) {
            vt_state = VT_IDLE;
        } else if (c == 0x1b) {
            vt_esc_pending = 1;
        }
        return;
    case VT_SKIP:
    default:
        vt_state = VT_IDLE;
        return;
    }
}

void console_vt_write(const char *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (gui_active) {
            putc(buf[i]);
            continue;
        }
        outb(DEBUG_CONSOLE_PORT, (uint8_t)buf[i]);
        vt_feed(buf[i]);
    }
}
