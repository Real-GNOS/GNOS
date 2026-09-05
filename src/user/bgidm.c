/*
 * bgidm.c — AEOS BGIDM Desktop Manager for GNOS. (GPLv2)
 *
 * A framebuffer-based desktop inspired by the original BGIDM.
 * Draws to /dev/fb0 with mouse support from /dev/input/event1.
 *
 * Features: taskbar, start menu, clock, about dialog, paint, pong.
 * Usage: bgidm
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
/* input event structures (from linux/uapi/linux/input.h) */
struct input_event {
    /* struct timeval tv; — 16 bytes on x86_64 */
    long tv_sec, tv_usec;
    unsigned short type, code;
    int value;
};
#define EV_REL   2
#define EV_KEY   1
#define REL_X    0
#define REL_Y    1
#define BTN_LEFT  272
#define BTN_RIGHT 273
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- framebuffer ------------------------------------------------------ */
static int fb_fd = -1;
static unsigned char *fb_mem = NULL;
static int fb_w, fb_h, fb_bpp, fb_stride;

/* input device */
static int mouse_fd = -1;

/* colours (16-bit RGB565) */
static unsigned short COL_BG;
static unsigned short COL_TASKBAR;
static unsigned short COL_TEXT;
static unsigned short COL_ACCENT;
static unsigned short COL_WIN_BG;
static unsigned short COL_WIN_BORDER;
static unsigned short COL_WHITE;
static unsigned short COL_RED;
static unsigned short COL_GREEN;
static unsigned short COL_BLUE;
static unsigned short COL_YELLOW;
static unsigned short COL_BLACK;

static unsigned short rgb(int r, int g, int b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void init_colors(void)
{
    COL_BG         = rgb(30, 30, 46);
    COL_TASKBAR    = rgb(24, 24, 37);
    COL_TEXT       = rgb(205, 214, 244);
    COL_ACCENT     = rgb(137, 180, 250);
    COL_WIN_BG     = rgb(49, 50, 68);
    COL_WIN_BORDER = rgb(137, 180, 250);
    COL_WHITE      = rgb(205, 214, 244);
    COL_RED        = rgb(243, 139, 168);
    COL_GREEN      = rgb(166, 227, 161);
    COL_BLUE       = rgb(137, 180, 250);
    COL_YELLOW     = rgb(249, 226, 175);
    COL_BLACK      = rgb(0, 0, 0);
}

/* ---- pixel operations ------------------------------------------------- */
static inline void putpixel(int x, int y, unsigned short color)
{
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h) return;
    unsigned short *p = (unsigned short *)(fb_mem + y * fb_stride + x * fb_bpp);
    *p = color;
}

static void fill_rect(int x, int y, int w, int h, unsigned short color)
{
    for (int j = y; j < y + h && j < fb_h; j++)
        for (int i = x; i < x + w && i < fb_w; i++)
            putpixel(i, j, color);
}

static void draw_rect(int x, int y, int w, int h, unsigned short color)
{
    for (int i = x; i < x + w; i++) { putpixel(i, y, color); putpixel(i, y + h - 1, color); }
    for (int j = y; j < y + h; j++) { putpixel(x, j, color); putpixel(x + w - 1, j, color); }
}

/* ---- simple 8x8 bitmap font ------------------------------------------- */
static const unsigned char font8x8[][8] = {
    /* ASCII 32..126, each char is 8 bytes (rows top-to-bottom, MSB left) */
    [0]  = {0,0,0,0,0,0,0,0},       /* space */
    [1]  = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    [2]  = {0x36,0x36,0x14,0x00,0,0,0,0},               /* " */
    [3]  = {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},   /* # */
    [4]  = {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},   /* $ */
    [5]  = {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},   /* % */
    [6]  = {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},   /* & */
    [7]  = {0x06,0x06,0x03,0x00,0,0,0,0},               /* ' */
    [8]  = {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},   /* ( */
    [9]  = {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},   /* ) */
    [10] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},   /* * */
    [11] = {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},   /* + */
    [12] = {0,0,0,0,0,0x0C,0x0C,0x06},                   /* , */
    [13] = {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},   /* - */
    [14] = {0,0,0,0,0,0x0C,0x0C,0},                      /* . */
    [15] = {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},   /* / */
    [16] = {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},   /* 0 */
    [17] = {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},   /* 1 */
    [18] = {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},   /* 2 */
    [19] = {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},   /* 3 */
    [20] = {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},   /* 4 */
    [21] = {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},   /* 5 */
    [22] = {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},   /* 6 */
    [23] = {0x3F,0x33,0x18,0x0C,0x0C,0x0C,0x0C,0x00},   /* 7 */
    [24] = {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},   /* 8 */
    [25] = {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},   /* 9 */
    [26] = {0,0,0x0C,0,0,0x0C,0},                        /* : */
    [27] = {0,0,0x0C,0,0,0x0C,0x06,0},                   /* ; */
    [28] = {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},   /* < */
    [29] = {0x00,0x00,0x3F,0x00,0x3F,0x00,0x00,0x00},   /* = */
    [30] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},   /* > */
    [31] = {0x1E,0x33,0x30,0x18,0x0C,0,0x0C,0x00},      /* ? */
    [32] = {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},   /* @ */
    [33] = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},   /* A */
    [34] = {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},   /* B */
    [35] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},   /* C */
    [36] = {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},   /* D */
    [37] = {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},   /* E */
    [38] = {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},   /* F */
    [39] = {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},   /* G */
    [40] = {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},   /* H */
    [41] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},   /* I */
    [42] = {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},   /* J */
    [43] = {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},   /* K */
    [44] = {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},   /* L */
    [45] = {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},   /* M */
    [46] = {0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00},   /* N */
    [47] = {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},   /* O */
    [48] = {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},   /* P */
    [49] = {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},   /* Q */
    [50] = {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},   /* R */
    [51] = {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},   /* S */
    [52] = {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},   /* T */
    [53] = {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},   /* U */
    [54] = {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},   /* V */
    [55] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},   /* W */
    [56] = {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},   /* X */
    [57] = {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},   /* Y */
    [58] = {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},   /* Z */
    [59] = {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},   /* [ */
    [60] = {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},   /* \ */
    [61] = {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},   /* ] */
    [62] = {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},   /* ^ */
    [63] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},   /* _ */
    [64] = {0x0C,0x0C,0x18,0x00,0,0,0,0},               /* ` */
    [65] = {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},   /* a */
    [66] = {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},   /* b */
    [67] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},   /* c */
    [68] = {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},   /* d */
    [69] = {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},   /* e */
    [70] = {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},   /* f */
    [71] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},   /* g */
    [72] = {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},   /* h */
    [73] = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},   /* i */
    [74] = {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},   /* j */
    [75] = {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},   /* k */
    [76] = {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},   /* l */
    [77] = {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},   /* m */
    [78] = {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},   /* n */
    [79] = {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},   /* o */
    [80] = {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},   /* p */
    [81] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},   /* q */
    [82] = {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},   /* r */
    [83] = {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},   /* s */
    [84] = {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},   /* t */
    [85] = {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},   /* u */
    [86] = {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},   /* v */
    [87] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},   /* w */
    [88] = {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},   /* x */
    [89] = {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},   /* y */
    [90] = {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},   /* z */
    [91] = {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},   /* { */
    [92] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},   /* | */
    [93] = {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},   /* } */
    [94] = {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},   /* ~ */
};

static void draw_char(int x, int y, char ch, unsigned short color, int scale)
{
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx > 94) return;
    for (int row = 0; row < 8; row++) {
        unsigned char bits = font8x8[idx][row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        putpixel(x + col * scale + sx, y + row * scale + sy, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, unsigned short color, int scale)
{
    while (*str) {
        draw_char(x, y, *str, color, scale);
        x += 8 * scale;
        str++;
    }
}

static int string_width(const char *str, int scale)
{
    return strlen(str) * 8 * scale;
}

/* ---- 8x8 icon bitmaps ------------------------------------------------- */
static const unsigned char icon_mouse[8] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xE0, 0xC8, 0x88, 0x00
};
static const unsigned char icon_doc[8] = {
    0x3C, 0x42, 0x81, 0xBD, 0x81, 0xBD, 0x81, 0x7E
};
static const unsigned char icon_folder[8] = {
    0x00, 0xFE, 0x82, 0x82, 0xFE, 0xFE, 0xFE, 0x00
};
static const unsigned char icon_paint[8] = {
    0x10, 0x38, 0x7C, 0xFE, 0x7C, 0x38, 0x10, 0x00
};

static void draw_icon(int x, int y, const unsigned char *icon, unsigned short color)
{
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (icon[row] & (0x80 >> col))
                putpixel(x + col, y + row, color);
}

/* ---- mouse ----------------------------------------------------------- */
static int mouse_x = 0, mouse_y = 0;
static int mouse_left = 0, mouse_right = 0;

static void read_mouse(void)
{
    struct input_event ev;
    while (read(mouse_fd, &ev, sizeof ev) == sizeof(ev)) {
        if (ev.type == 2) { /* EV_REL */
            if (ev.code == 0) mouse_x += ev.value; /* REL_X */
            if (ev.code == 1) mouse_y += ev.value; /* REL_Y */
        } else if (ev.type == 1) { /* EV_KEY */
            if (ev.code == 272) mouse_left = ev.value;   /* BTN_LEFT */
            if (ev.code == 273) mouse_right = ev.value;  /* BTN_RIGHT */
        }
    }
    /* clamp */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= fb_w) mouse_x = fb_w - 1;
    if (mouse_y >= fb_h) mouse_y = fb_h - 1;
}

static int mouse_in_rect(int rx, int ry, int rw, int rh)
{
    return mouse_x >= rx && mouse_x < rx + rw && mouse_y >= ry && mouse_y < ry + rh;
}

/* ---- desktop state --------------------------------------------------- */
#define TASKBAR_H 30
#define MENU_ITEMS 5

static int start_menu_open = 0;
static int power_menu_open = 0;
static int about_open = 0;
static int paint_open = 0;
static int pong_open = 0;
static int vtermon = 0;

/* paint state */
static unsigned short paint_grid[80][45]; /* 8-pixel cells */

/* pong state */
static int pong_px = 30, pong_cx = 30, pong_ball_x = 320, pong_ball_y = 240;
static int pong_dx = 3, pong_dy = 2, pong_ps = 0, pong_cs = 0;

static const char *menu_labels[MENU_ITEMS] = {
    "File Manager", "Paint", "Pong", "About", "Shutdown"
};

static void toggle_start_menu(void) { start_menu_open = !start_menu_open; power_menu_open = 0; }
static void close_all(void) { start_menu_open = 0; power_menu_open = 0; about_open = 0; }

/* ---- render functions ------------------------------------------------- */
static void render_taskbar(void)
{
    /* taskbar background */
    fill_rect(0, fb_h - TASKBAR_H, fb_w, TASKBAR_H, COL_TASKBAR);
    draw_rect(0, fb_h - TASKBAR_H, fb_w, 1, COL_ACCENT);

    /* Start button */
    int btn_w = 60;
    int btn_h = TASKBAR_H - 4;
    int btn_y = fb_h - TASKBAR_H + 2;
    unsigned short btn_col = mouse_in_rect(2, btn_y, btn_w, btn_h) ? COL_ACCENT : COL_BLUE;
    fill_rect(2, btn_y, btn_w, btn_h, btn_col);
    draw_string(8, btn_y + 8, "START", COL_WHITE, 1);

    /* clock */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime(&tv.tv_sec);
    char clock[16];
    snprintf(clock, sizeof clock, "%02d:%02d", tm->tm_hour, tm->tm_min);
    draw_string(fb_w - string_width(clock, 2) - 8, btn_y + 6, clock, COL_TEXT, 2);

    /* AEOS label */
    draw_string(btn_w + 12, btn_y + 8, "AEOS 5.11", COL_TEXT, 1);
}

static void render_start_menu(void)
{
    if (!start_menu_open) return;

    int mx = 2, my = fb_h - TASKBAR_H - MENU_ITEMS * 25 - 10;
    int mw = 180, mh = MENU_ITEMS * 25 + 10;

    fill_rect(mx, my, mw, mh, COL_WIN_BG);
    draw_rect(mx, my, mw, mh, COL_WIN_BORDER);

    /* title */
    fill_rect(mx + 1, my + 1, mw - 2, 20, COL_ACCENT);
    draw_string(mx + 6, my + 5, "AEOS Start Menu", COL_BLACK, 1);

    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = my + 22 + i * 25;
        int hovered = mouse_in_rect(mx + 2, iy, mw - 4, 23);
        if (hovered)
            fill_rect(mx + 2, iy, mw - 4, 23, COL_ACCENT);
        draw_string(mx + 10, iy + 7, menu_labels[i], hovered ? COL_BLACK : COL_TEXT, 1);
    }
}

static void render_power_menu(void)
{
    if (!power_menu_open) return;

    int mx = fb_w / 2 - 120, my = fb_h / 2 - 50;
    int mw = 240, mh = 100;

    fill_rect(mx, my, mw, mh, COL_WIN_BG);
    draw_rect(mx, my, mw, mh, COL_RED);

    fill_rect(mx + 1, my + 1, mw - 2, 20, COL_RED);
    draw_string(mx + 6, my + 5, "Power Options", COL_WHITE, 1);

    const char *opts[] = {"Shutdown", "Reboot", "Cancel"};
    for (int i = 0; i < 3; i++) {
        int bx = mx + 10 + i * 78;
        int by = my + 30;
        int bw = 70, bh = 30;
        int hovered = mouse_in_rect(bx, by, bw, bh);
        fill_rect(bx, by, bw, bh, hovered ? COL_ACCENT : COL_WIN_BG);
        draw_rect(bx, by, bw, bh, COL_WIN_BORDER);
        int tw = string_width(opts[i], 1);
        draw_string(bx + (bw - tw) / 2, by + 10, opts[i], COL_TEXT, 1);
    }
}

static void render_about(void)
{
    if (!about_open) return;

    int mx = fb_w / 2 - 160, my = fb_h / 2 - 60;
    int mw = 320, mh = 120;

    fill_rect(mx, my, mw, mh, COL_WIN_BG);
    draw_rect(mx, my, mw, mh, COL_WIN_BORDER);

    fill_rect(mx + 1, my + 1, mw - 2, 20, COL_ACCENT);
    /* close button */
    int cbx = mx + mw - 22, cby = my + 1;
    fill_rect(cbx, cby, 20, 20, COL_RED);
    draw_string(cbx + 5, cby + 5, "X", COL_WHITE, 1);

    draw_string(mx + 6, my + 5, "About AEOS", COL_BLACK, 1);

    draw_string(mx + 15, my + 30, "AEOS v5.11.2 Build 8070 Patch 4", COL_TEXT, 1);
    draw_string(mx + 15, my + 48, "BGIDM 1.11.0 for GNOS", COL_TEXT, 1);
    draw_string(mx + 15, my + 66, "Kernel: AEOS 0.1 (x86_64)", COL_TEXT, 1);
    draw_string(mx + 15, my + 84, "Based on GNOS kernel project", COL_TEXT, 1);
    draw_string(mx + 15, my + 100, "By Lithium4141", COL_ACCENT, 1);
}

static void render_paint(void)
{
    if (!paint_open) return;

    int mx = 20, my = 20, mw = fb_w - 40, mh = fb_h - TASKBAR_H - 40;
    fill_rect(mx, my, mw, mh, COL_WIN_BG);
    draw_rect(mx, my, mw, mh, COL_WIN_BORDER);

    fill_rect(mx + 1, my + 1, mw - 2, 20, COL_ACCENT);
    int cbx = mx + mw - 22, cby = my + 1;
    fill_rect(cbx, cby, 20, 20, COL_RED);
    draw_string(cbx + 5, cby + 5, "X", COL_WHITE, 1);
    draw_string(mx + 6, my + 5, "Paint", COL_BLACK, 1);

    int gx = mx + 5, gy = my + 25;
    int gw = mw - 10, gh = mh - 30;
    fill_rect(gx, gy, gw, gh, COL_WHITE);

    /* draw grid */
    for (int i = 0; i < 80; i++)
        for (int j = 0; j < 45; j++)
            if (paint_grid[i][j])
                fill_rect(gx + i * 8, gy + j * 8, 8, 8, paint_grid[i][j]);

    /* draw if painting */
    if (mouse_left && mouse_in_rect(gx, gy, gw, gh)) {
        int ci = (mouse_x - gx) / 8;
        int cj = (mouse_y - gy) / 8;
        if (ci >= 0 && ci < 80 && cj >= 0 && cj < 45)
            paint_grid[ci][cj] = COL_BLACK;
    }
    if (mouse_right && mouse_in_rect(gx, gy, gw, gh)) {
        int ci = (mouse_x - gx) / 8;
        int cj = (mouse_y - gy) / 8;
        if (ci >= 0 && ci < 80 && cj >= 0 && cj < 45)
            paint_grid[ci][cj] = 0;
    }
}

static void render_pong(void)
{
    if (!pong_open) return;

    int mx = fb_w / 4, my = 40, mw = fb_w / 2, mh = fb_h - TASKBAR_H - 60;
    fill_rect(mx, my, mw, mh, COL_WIN_BG);
    draw_rect(mx, my, mw, mh, COL_WIN_BORDER);

    fill_rect(mx + 1, my + 1, mw - 2, 20, COL_ACCENT);
    int cbx = mx + mw - 22, cby = my + 1;
    fill_rect(cbx, cby, 20, 20, COL_RED);
    draw_string(cbx + 5, cby + 5, "X", COL_WHITE, 1);
    draw_string(mx + 6, my + 5, "Pong", COL_BLACK, 1);

    int px = mx + 5, py = my + 25, pw = mw - 10, ph = mh - 30;
    fill_rect(px, py, pw, ph, COL_BLACK);
    draw_rect(px, py, pw, ph, COL_WIN_BORDER);

    /* draw paddles and ball */
    fill_rect(px + 2, py + pong_px, 4, 30, COL_WHITE);
    fill_rect(px + pw - 6, py + pong_cx, 4, 30, COL_WHITE);
    fill_rect(px + pong_ball_x - 2, py + pong_ball_y - 2, 5, 5, COL_WHITE);

    /* score */
    char sc[32];
    snprintf(sc, sizeof sc, "%d - %d", pong_ps, pong_cs);
    draw_string(px + (pw - string_width(sc, 1)) / 2, py + ph + 5, sc, COL_TEXT, 1);
}

/* ---- mouse cursor ---------------------------------------------------- */
static void render_cursor(void)
{
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (icon_mouse[row] & (0x80 >> col))
                putpixel(mouse_x + col, mouse_y + row, COL_WHITE);
}

/* ---- main loop ------------------------------------------------------- */
static void pong_update(void)
{
    pong_ball_x += pong_dx;
    pong_ball_y += pong_dy;

    /* bounce off top/bottom */
    if (pong_ball_y <= 0 || pong_ball_y >= 180) pong_dy = -pong_dy;

    /* bounce off paddles */
    if (pong_ball_x <= 10 && pong_ball_y >= pong_px && pong_ball_y <= pong_px + 30)
        pong_dx = -pong_dx;
    if (pong_ball_x >= 280 && pong_ball_y >= pong_cx && pong_ball_y <= pong_cx + 30)
        pong_dx = -pong_dx;

    /* score */
    if (pong_ball_x <= 0) { pong_cs++; pong_ball_x = 150; pong_ball_y = 90; pong_dx = 3; }
    if (pong_ball_x >= 300) { pong_ps++; pong_ball_x = 150; pong_ball_y = 90; pong_dx = -3; }

    /* CPU paddle follow */
    if (pong_cx + 15 < pong_ball_y) pong_cx += 2;
    if (pong_cx + 15 > pong_ball_y) pong_cx -= 2;
    if (pong_cx < 0) pong_cx = 0;
    if (pong_cx > 150) pong_cx = 150;

    /* player paddle from mouse */
    int gy = fb_h - TASKBAR_H - 100;
    if (mouse_in_rect(0, gy, fb_w, 100)) {
        pong_px = (mouse_y - gy) * 180 / 100 - 15;
        if (pong_px < 0) pong_px = 0;
        if (pong_px > 150) pong_px = 150;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* open framebuffer */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "bgidm: cannot open /dev/fb0: %s\n", strerror(errno));
        return 1;
    }

    /* get screen info */
    struct {
        unsigned type, height, width, bpp;
        int line_length, x_offset, y_offset;
    } finfo;
    struct {
        unsigned xres, yres, xres_virt, yres_virt, bits_per_pixel;
    } vinfo;

    if (ioctl(fb_fd, 0x4600, &finfo) < 0 ||   /* FBIOGET_FSCREENINFO */
        ioctl(fb_fd, 0x4601, &vinfo) < 0) {   /* FBIOGET_VSCREENINFO */
        fprintf(stderr, "bgidm: ioctl failed: %s\n", strerror(errno));
        close(fb_fd);
        return 1;
    }

    fb_w = vinfo.xres;
    fb_h = vinfo.yres;
    fb_bpp = vinfo.bits_per_pixel / 8;
    fb_stride = finfo.line_length;

    size_t fb_size = fb_stride * fb_h;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "bgidm: mmap failed: %s\n", strerror(errno));
        close(fb_fd);
        return 1;
    }

    init_colors();

    /* open mouse */
    mouse_fd = open("/dev/input/event1", O_RDONLY);
    if (mouse_fd < 0) {
        fprintf(stderr, "bgidm: warning: no mouse (/dev/input/event1): %s\n", strerror(errno));
    }

    /* set non-blocking on mouse */
    if (mouse_fd >= 0) {
        int flags = fcntl(mouse_fd, F_GETFL, 0);
        fcntl(mouse_fd, F_SETFL, flags | O_NONBLOCK);
    }

    memset(paint_grid, 0, sizeof paint_grid);

    printf("BGIDM v1.11.0 starting on AEOS...\n");

    /* main loop */
    while (1) {
        /* read input */
        if (mouse_fd >= 0) read_mouse();

        /* handle clicks on taskbar */
        if (mouse_left) {
            int btn_y = fb_h - TASKBAR_H + 2;
            if (mouse_in_rect(2, btn_y, 60, TASKBAR_H - 4)) {
                toggle_start_menu();
                /* debounce */
                while (mouse_left && mouse_fd >= 0) read_mouse();
            }

            /* start menu items */
            if (start_menu_open) {
                int mmy = fb_h - TASKBAR_H - MENU_ITEMS * 25 - 10;
                for (int i = 0; i < MENU_ITEMS; i++) {
                    int iy = mmy + 22 + i * 25;
                    if (mouse_in_rect(4, iy, 176, 23)) {
                        close_all();
                        if (i == 0) { /* File Manager - open terminal */
                            if (fork() == 0) { execl("/bin/sh", "sh", "-c", "ls /", NULL); _exit(0); }
                        } else if (i == 1) { paint_open = 1; memset(paint_grid, 0, sizeof paint_grid); }
                        else if (i == 2) { pong_open = 1; pong_ball_x = 150; pong_ball_y = 90; pong_ps = 0; pong_cs = 0; }
                        else if (i == 3) { about_open = 1; }
                        else if (i == 4) { power_menu_open = 1; }
                        while (mouse_left && mouse_fd >= 0) read_mouse();
                        break;
                    }
                }
            }

            /* power menu buttons */
            if (power_menu_open) {
                int pmx = fb_w / 2 - 120, pmy = fb_h / 2 - 50;
                for (int i = 0; i < 3; i++) {
                    int bx = pmx + 10 + i * 78;
                    if (mouse_in_rect(bx, pmy + 30, 70, 30)) {
                        close_all();
                        if (i == 0) { /* Shutdown */
                            sync();
                            execl("/sbin/halt", "halt", "-p", NULL);
                            execl("/bin/sh", "sh", "-c", "poweroff", NULL);
                        } else if (i == 1) { /* Reboot */
                            sync();
                            execl("/sbin/reboot", "reboot", NULL);
                        }
                        while (mouse_left && mouse_fd >= 0) read_mouse();
                        break;
                    }
                }
            }

            /* close buttons */
            if (about_open) {
                int abx = fb_w / 2 - 160 + 320 - 22, aby = fb_h / 2 - 60 + 1;
                if (mouse_in_rect(abx, aby, 20, 20)) { about_open = 0; while (mouse_left && mouse_fd >= 0) read_mouse(); }
            }
            if (paint_open) {
                int pbx = fb_w - 40 + 20 - 22, pby = 20 + 1;
                if (mouse_in_rect(pbx, pby, 20, 20)) { paint_open = 0; while (mouse_left && mouse_fd >= 0) read_mouse(); }
            }
            if (pong_open) {
                int obx = fb_w / 4 + fb_w / 2 - 22, oby = 40 + 1;
                if (mouse_in_rect(obx, oby, 20, 20)) { pong_open = 0; while (mouse_left && mouse_fd >= 0) read_mouse(); }
            }
        }

        /* clear screen */
        fill_rect(0, 0, fb_w, fb_h, COL_BG);

        /* render desktop */
        render_start_menu();
        render_power_menu();
        render_about();
        render_paint();
        render_pong();
        render_taskbar();
        render_cursor();

        /* pong update */
        if (pong_open) pong_update();

        /* flush */
        usleep(16000); /* ~60fps */
    }

    munmap(fb_mem, fb_size);
    close(fb_fd);
    if (mouse_fd >= 0) close(mouse_fd);
    return 0;
}
