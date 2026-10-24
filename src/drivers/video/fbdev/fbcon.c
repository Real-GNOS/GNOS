/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fbcon.c — framebuffer text consoles (VGA 8x16 + Unifont 16x16). (GPLv2)
 *
 * Draws directly into the linear framebuffer Limine handed us.  The console
 * scrolls by memmove-ing the pixel rows up one glyph height, which is slow but
 * keeps the whole thing dependency-free.
 *
 * There are several consoles and one framebuffer.  Each console keeps a cell
 * buffer that mirrors what its screen *would* look like, and only the active
 * one also paints pixels.  That split is what makes virtual terminals work: a
 * shell on tty3 goes on printing into cells while tty1 is on screen, and
 * fbcon_activate() repaints the whole grid from those cells when the user
 * presses Ctrl-Alt-F3.
 *
 * Two things set this apart from a 1980s VGA console:
 *
 *   - Every cell stores a full Unicode code point, not a byte.  UTF-8 is
 *     decoded by the terminal engine (drivers/tty/vt_ansi.c) and East-Asian-
 *     wide characters take two cells with the glyph drawn across them -- so
 *     Chinese, Japanese and the box-drawing characters a TUI is made of all
 *     render instead of turning into mojibake.  Glyphs come from the bundled
 *     8x16 VGA font for code points below 256 and from the embedded Unifont
 *     bitmap (cjkfont.c) for everything else.
 *
 *   - The escape sequences -- SGR colours, cursor addressing, erase,
 *     insert/delete line and character, scroll regions, alternate screen and
 *     the cursor-visibility toggle -- are parsed by the same vt_ansi engine
 *     Uinxed keeps under drivers/tty/.  This file only implements the
 *     callbacks that turn terminal operations into cell and pixel writes,
 *     which keeps the parser independent of the framebuffer.
 */
#include <stdint.h>

#include "bootinfo.h"
#include "fbcon.h"
#include "font8x16.h"
#include "cjkfont.h"
#include "kstring.h"
#include "debugcon.h"
#include "heap.h"
#include "vt_ansi.h"

/* ---- the framebuffer, shared by every console --------------------------- */
static struct {
    uint32_t *fb;
    uint32_t  w;      /* pixels */
    uint32_t  h;      /* pixels */
    uint32_t  stride; /* uint32_t per scanline */
    uint32_t  cols;   /* glyph columns, clamped to FBCON_MAX_COLS */
    uint32_t  rows;   /* glyph rows,    clamped to FBCON_MAX_ROWS */
} g_fb;

/* When a DRM client owns the scanout, the refresh thread copies its buffer
 * over the console framebuffer every frame.  Any text fbcon paints into the
 * same memory would show up as a flicker for one refresh cycle (a stray
 * "^C" echo, part of the login prompt) before being wiped, so the DRM
 * driver suppresses fbcon rendering while a client framebuffer is live.
 * Console state is still maintained; only the pixel writes are skipped. */
static volatile int g_suppressed;

/* ---- a single screen cell ----------------------------------------------- *
 * Once per cell, not per write: repaint after a switch and scroll and erase
 * must all reconstruct the screen exactly, including colour and wide glyphs. */
typedef struct {
    uint32_t cp;    /* Unicode code point (0 = blank)            */
    uint8_t  flags; /* CELL_* below                              */
    uint8_t  fg;    /* colour index 0..15, 0 = console default   */
    uint8_t  bg;    /* colour index 0..15, 0 = console default   */
    uint8_t  _pad;
} cell_t;

#define CELL_BOLD      0x01
#define CELL_REVERSE   0x02
#define CELL_UNDERLINE 0x04
#define CELL_WIDE      0x08 /* first cell of a double-width glyph */
#define CELL_CONT      0x10 /* second cell of a double-width glyph */

typedef struct {
    int      live;
    uint32_t def_fg; /* the colour fg/bg==0 resolve to */
    uint32_t def_bg;

    /* Cursor, colours, scroll region, saved state, alternate-screen
     * bookkeeping and the parser/UTF-8 decoders all live in the terminal
     * engine, one instance per console. */
    vt_ansi_t ansi;

    cell_t   cells[FBCON_MAX_ROWS * FBCON_MAX_COLS];

    /* Alternate screen (?1049h): a full-screen TUI (nano, vi) switches to a
     * private buffer on entry and back to the normal screen on exit, which
     * is what restores the shell prompt instead of leaving the editor's last
     * frame behind. */
    cell_t  *alt_cells;
} console_t;

static console_t g_con[FBCON_MAX_CONS];
static int       g_active;

/* ---- /dev/vcs access ---------------------------------------------------- */

static void draw_cell(console_t *c, uint32_t col, uint32_t row);

int fbcon_get_cell(int con, uint32_t row, uint32_t col, uint32_t *cp, uint8_t *attr)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live)
        return -1;
    console_t *c = &g_con[con];
    if (row >= g_fb.rows || col >= g_fb.cols)
        return -1;
    const cell_t *cell = &c->cells[row * g_fb.cols + col];
    if (cp)
        *cp = cell->cp;
    if (attr)
        *attr = (uint8_t)(((cell->bg & 0x7) << 4) | (cell->fg & 0x7) |
                          (cell->flags & CELL_BOLD ? 0x08 : 0));
    return 0;
}

int fbcon_set_cell(int con, uint32_t row, uint32_t col, uint32_t cp, uint8_t attr)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live)
        return -1;
    console_t *c = &g_con[con];
    if (row >= g_fb.rows || col >= g_fb.cols)
        return -1;
    cell_t *cell = &c->cells[row * g_fb.cols + col];
    cell->cp     = cp;
    cell->flags  = (attr & 0x08) ? CELL_BOLD : 0;
    cell->fg     = attr & 0x7;
    cell->bg     = (attr >> 4) & 0x7;
    if (con == g_active)
        draw_cell(c, col, row);
    return 0;
}

static inline cell_t *cell_at(console_t *c, uint32_t col, uint32_t row)
{
    return &c->cells[row * FBCON_MAX_COLS + col];
}

/* Resolve a cell's effective pixel colours, honouring bold and reverse. */
static void eff_colors(console_t *c, const cell_t *cell, uint32_t *fgp, uint32_t *bgp)
{
    uint32_t fg = (cell->fg) ? vt_ansi_palette[cell->fg & 0xF] : c->def_fg;
    uint32_t bg = (cell->bg) ? vt_ansi_palette[cell->bg & 0xF] : c->def_bg;
    if (cell->flags & CELL_BOLD)
        fg = (fg & 0x00FF0000) | (((fg & 0x00FF00) << 1) & 0x00FF00) |
             (((fg & 0x0000FF) << 1) & 0x0000FF); /* brighten */
    if (cell->flags & CELL_REVERSE) {
        uint32_t t = fg;
        fg         = bg;
        bg         = t;
    }
    *fgp = fg;
    *bgp = bg;
}

/* Paint one glyph at a cell, using the cell's own colours.  Drawing the
 * continuation half of a wide glyph is a no-op: the left cell already laid the
 * whole 16-pixel-wide bitmap down over it.
 *
 * Blank cells have to paint their background too.  repaint() walks every
 * cell, and before this handled cp == 0 by returning immediately -- so an
 * "erase display" or switching to a console that had been cleared left the
 * old glyph pixels standing. */
static void draw_cell(console_t *c, uint32_t col, uint32_t row)
{
    if (!g_fb.fb || col >= g_fb.cols || row >= g_fb.rows)
        return;
    const cell_t *cell = cell_at(c, col, row);
    if (cell->flags & CELL_CONT) /* right half of a wide glyph: nothing */
        return;

    uint32_t fg, bg;
    eff_colors(c, cell, &fg, &bg);

    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    if (!cell->cp) {
        for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
            uint32_t *line = &g_fb.fb[(py + y) * g_fb.stride + px];
            for (uint32_t x = 0; x < FONT_WIDTH; x++)
                line[x] = bg;
        }
        return;
    }

    const uint8_t *glyph;
    uint32_t       w = FONT_WIDTH, h = FONT_HEIGHT;
    int            is_cjk = (cell->cp >= 256);

    if (is_cjk) {
        int gw = 0;
        glyph  = cjkfont_glyph(cell->cp, &gw);
        if (glyph) {
            w = (uint32_t)gw;
            h = CJK_HEIGHT;
        } else
            glyph = NULL;
    } else {
        glyph = &font8x16[(uint8_t)cell->cp * FONT_HEIGHT];
    }

    if (!glyph) {
        /* Not in either font: draw a hollow box so "missing" is visible rather
         * than silent. */
        for (uint32_t y = 0; y < h; y++) {
            uint32_t *line = &g_fb.fb[(py + y) * g_fb.stride + px];
            for (uint32_t x = 0; x < w; x++)
                line[x] = ((y == 0 || y == h - 1 || x == 0 || x == w - 1)) ? fg : bg;
        }
        return;
    }

    for (uint32_t y = 0; y < h; y++) {
        uint8_t   bits = glyph[y];
        uint32_t *line = &g_fb.fb[(py + y) * g_fb.stride + px];
        for (uint32_t x = 0; x < 8; x++)
            line[x] = (bits & (0x80 >> x)) ? fg : bg;
        if (w > 8) {
            uint8_t bits2 = glyph[y + CJK_HEIGHT]; /* second bitmap row */
            for (uint32_t x = 0; x < 8; x++)
                line[8 + x] = (bits2 & (0x80 >> x)) ? fg : bg;
        }
    }
}

/* Repaint one cell's background plus, when a wide glyph lives there, also the
 * cell to its right -- because clearing the left cell alone would leave the
 * right half's pixels behind. */
static void fill_cell_bg(console_t *c, uint32_t col, uint32_t row)
{
    cell_t  *cell = cell_at(c, col, row);
    uint32_t fg, bg;
    eff_colors(c, cell, &fg, &bg);
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;
    for (uint32_t y = 0; y < FONT_HEIGHT; y++)
        for (uint32_t x = 0; x < FONT_WIDTH; x++)
            g_fb.fb[(py + y) * g_fb.stride + px + x] = bg;
    if (cell->flags & CELL_WIDE) {
        px += FONT_WIDTH;
        for (uint32_t y = 0; y < FONT_HEIGHT; y++)
            for (uint32_t x = 0; x < FONT_WIDTH; x++)
                g_fb.fb[(py + y) * g_fb.stride + px + x] = bg;
    }
}

/* Store a code point in the cell buffer and, when this is the console on
 * screen, paint it.  `cjk` says whether cp spans two cells (the caller has
 * already checked width); when set the next column is marked a continuation. */
static void put_codepoint(console_t *c, uint32_t col, uint32_t row, uint32_t cp, int cjk)
{
    if (col >= g_fb.cols || row >= g_fb.rows)
        return;

    cell_t *cell = cell_at(c, col, row);
    cell->cp     = cp;
    cell->fg     = c->ansi.fg;
    cell->bg     = c->ansi.bg;
    cell->flags  = c->ansi.attrs;
    if (cjk) {
        cell->flags |= CELL_WIDE;
        cell_t *right = cell_at(c, col + 1, row);
        right->cp     = 0;
        right->flags  = CELL_CONT;
        right->fg     = c->ansi.fg;
        right->bg     = c->ansi.bg;
    }

    if (c == &g_con[g_active]) {
        fill_cell_bg(c, col, row);
        draw_cell(c, col, row);
        if (cjk && col + 1 < g_fb.cols)
            fill_cell_bg(c, col + 1, row);
    }
}

static void repaint(console_t *c)
{
    if (!g_fb.fb)
        return;
    for (uint32_t row = 0; row < g_fb.rows; row++)
        for (uint32_t col = 0; col < g_fb.cols; col++)
            draw_cell(c, col, row);
}

/* Blank every cell.  The terminal state (cursor, scroll region, colours) is
 * owned by vt_ansi and is deliberately untouched here. */
static void clear_cells(console_t *c)
{
    memset(c->cells, 0, sizeof(c->cells));
}

/* Fill the whole scanout with the console background.  Only the active
 * console owns the screen; a hidden one just updates its cells. */
static void paint_console_bg(console_t *c)
{
    if (!g_fb.fb || c != &g_con[g_active])
        return;
    for (uint32_t y = 0; y < g_fb.h; y++)
        for (uint32_t x = 0; x < g_fb.stride; x++)
            g_fb.fb[y * g_fb.stride + x] = c->def_bg;
}

static void clear_console(console_t *c)
{
    clear_cells(c);
    paint_console_bg(c);
}

/* Scroll the region [top, bot] up one row, blanking the bottom line. */
static void scroll_region(console_t *c, uint32_t top, uint32_t bot)
{
    for (uint32_t row = top; row < bot; row++)
        memmove(cell_at(c, 0, row), cell_at(c, 0, row + 1), g_fb.cols * sizeof(cell_t));
    for (uint32_t col = 0; col < g_fb.cols; col++) {
        cell_t *cell = cell_at(c, col, bot);
        cell->cp     = 0;
        cell->flags  = 0;
        cell->fg     = 0;
        cell->bg     = 0;
    }

    if (c != &g_con[g_active] || !g_fb.fb)
        return;

    uint32_t top_px = top * FONT_HEIGHT;
    uint32_t bot_px = (bot + 1) * FONT_HEIGHT;
    uint32_t keep   = (bot_px - top_px - FONT_HEIGHT) * g_fb.stride;

    memmove(g_fb.fb + top_px * g_fb.stride, g_fb.fb + (top_px + FONT_HEIGHT) * g_fb.stride,
            (uint64_t)keep * 4);
    for (uint32_t y = bot_px - FONT_HEIGHT; y < bot_px; y++)
        for (uint32_t x = 0; x < g_fb.stride; x++)
            g_fb.fb[y * g_fb.stride + x] = c->def_bg;
}

/* ====================================================================== *
 *  vt_ansi callbacks
 *
 *  The escape/UTF-8 engine in drivers/tty/vt_ansi.c knows nothing about
 *  cells or pixels; these functions are where its requests become screen
 *  updates.  Coordinates and counts have already been clamped by the
 *  parser.
 * ====================================================================== */

static void con_draw_char(void *ctx, uint32_t x, uint32_t y, uint32_t cp, int wide)
{
    put_codepoint((console_t *)ctx, x, y, cp, wide);
}

static void con_touch(void *ctx, uint32_t x, uint32_t y)
{
    console_t *c = ctx;
    if (c == &g_con[g_active])
        draw_cell(c, x, y);
}

static void blank_cell(cell_t *cell)
{
    cell->cp    = 0;
    cell->flags = 0;
}

static void con_erase_display(void *ctx, uint32_t mode, uint32_t x, uint32_t y)
{
    console_t *c = ctx;
    uint32_t   col, row;

    if (mode == 0) { /* cursor to end of screen */
        for (col = x; col < g_fb.cols; col++)
            blank_cell(cell_at(c, col, y));
        for (row = y + 1; row < g_fb.rows; row++)
            for (col = 0; col < g_fb.cols; col++)
                blank_cell(cell_at(c, col, row));
    } else if (mode == 1) { /* start of screen to cursor */
        for (row = 0; row < y; row++)
            for (col = 0; col < g_fb.cols; col++)
                blank_cell(cell_at(c, col, row));
        for (col = 0; col <= x && col < g_fb.cols; col++)
            blank_cell(cell_at(c, col, y));
    } else if (mode == 2 || mode == 3) { /* whole screen / scrollback */
        clear_cells(c);
    }
}

static void con_erase_line(void *ctx, uint32_t mode, uint32_t x, uint32_t y)
{
    console_t *c = ctx;
    uint32_t   col;

    if (mode == 0) {
        for (col = x; col < g_fb.cols; col++)
            blank_cell(cell_at(c, col, y));
    } else if (mode == 1) {
        for (col = 0; col <= x && col < g_fb.cols; col++)
            blank_cell(cell_at(c, col, y));
    } else if (mode == 2) {
        for (col = 0; col < g_fb.cols; col++)
            blank_cell(cell_at(c, col, y));
    }
}

static void con_erase_chars(void *ctx, uint32_t x, uint32_t y, uint32_t count)
{
    console_t *c = ctx;
    for (uint32_t col = x; col < g_fb.cols && count-- > 0; col++)
        blank_cell(cell_at(c, col, y));
}

static void con_insert_lines(void *ctx, uint32_t y, uint32_t bottom, uint32_t count)
{
    console_t *c = ctx;

    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t row = bottom; row > y; row--)
            memmove(cell_at(c, 0, row), cell_at(c, 0, row - 1),
                    g_fb.cols * sizeof(cell_t));
        for (uint32_t col = 0; col < g_fb.cols; col++) {
            cell_t *cell = cell_at(c, col, y);
            cell->cp     = 0;
            cell->flags  = 0;
            cell->fg     = 0;
            cell->bg     = 0;
        }
    }
}

static void con_delete_lines(void *ctx, uint32_t y, uint32_t bottom, uint32_t count)
{
    console_t *c = ctx;

    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t row = y; row < bottom; row++)
            memmove(cell_at(c, 0, row), cell_at(c, 0, row + 1),
                    g_fb.cols * sizeof(cell_t));
        for (uint32_t col = 0; col < g_fb.cols; col++) {
            cell_t *cell = cell_at(c, col, bottom);
            cell->cp     = 0;
            cell->flags  = 0;
            cell->fg     = 0;
            cell->bg     = 0;
        }
    }
}

static void con_insert_chars(void *ctx, uint32_t x, uint32_t y, uint32_t count,
                             uint32_t cols)
{
    console_t *c   = ctx;
    uint32_t   col = x;

    while (count-- > 0 && col < cols) {
        for (uint32_t p = cols - 1; p > col; p--)
            *cell_at(c, p, y) = *cell_at(c, p - 1, y);
        blank_cell(cell_at(c, col, y));
        col++;
    }
}

static void con_delete_chars(void *ctx, uint32_t x, uint32_t y, uint32_t count,
                             uint32_t cols)
{
    console_t *c = ctx;

    for (uint32_t k = 0; k < count; k++) {
        uint32_t col;
        for (col = x; col + 1 < cols; col++)
            *cell_at(c, col, y) = *cell_at(c, col + 1, y);
        blank_cell(cell_at(c, col, y));
    }
}

static void con_scroll_up(void *ctx, uint32_t top, uint32_t bottom, uint32_t count)
{
    console_t *c = ctx;
    for (uint32_t k = 0; k < count; k++)
        scroll_region(c, top, bottom);
}

static void con_scroll_down(void *ctx, uint32_t top, uint32_t bottom, uint32_t count)
{
    console_t *c = ctx;

    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t row = bottom; row > top; row--)
            memmove(cell_at(c, 0, row), cell_at(c, 0, row - 1),
                    g_fb.cols * sizeof(cell_t));
        for (uint32_t col = 0; col < g_fb.cols; col++) {
            cell_t *cell = cell_at(c, col, top);
            cell->cp     = 0;
            cell->flags  = 0;
            cell->fg     = 0;
            cell->bg     = 0;
        }
    }
}

static void con_clear(void *ctx)
{
    clear_console((console_t *)ctx);
}

static int con_alt_screen(void *ctx, int on)
{
    console_t *c = ctx;

    if (on) {
        if (!c->alt_cells) {
            c->alt_cells = kmalloc(sizeof(c->cells));
            if (!c->alt_cells)
                return -1;
        }
        memcpy(c->alt_cells, c->cells, sizeof(c->cells));
        clear_console(c); /* the engine has not moved the cursor yet */
        return 0;
    }

    if (!c->alt_cells)
        return -1;
    memcpy(c->cells, c->alt_cells, sizeof(c->cells));
    return 0;
}

static void con_repaint(void *ctx)
{
    console_t *c = ctx;
    if (c == &g_con[g_active])
        repaint(c);
}

static int con_char_width(void *ctx, uint32_t cp)
{
    (void)ctx;
    return cjkfont_width(cp);
}

static const vt_ansi_callbacks_t g_ansi_cb = {
    .draw_char     = con_draw_char,
    .touch         = con_touch,
    .erase_display = con_erase_display,
    .erase_line    = con_erase_line,
    .erase_chars   = con_erase_chars,
    .insert_lines  = con_insert_lines,
    .delete_lines  = con_delete_lines,
    .insert_chars  = con_insert_chars,
    .delete_chars  = con_delete_chars,
    .scroll_up     = con_scroll_up,
    .scroll_down   = con_scroll_down,
    .clear         = con_clear,
    .alt_screen    = con_alt_screen,
    .repaint       = con_repaint,
    .char_width    = con_char_width,
};

/* ---- setup ------------------------------------------------------------- */
void fbcon_init(const bootinfo_t *bi)
{
    cjkfont_init();

    g_fb.fb     = (uint32_t *)(uintptr_t)bi->fb_addr;
    g_fb.w      = bi->fb_width;
    g_fb.h      = bi->fb_height;
    g_fb.stride = bi->fb_pitch / 4;

    g_fb.cols = g_fb.w / FONT_WIDTH;
    g_fb.rows = g_fb.h / FONT_HEIGHT;
    if (g_fb.cols > FBCON_MAX_COLS)
        g_fb.cols = FBCON_MAX_COLS;
    if (g_fb.rows > FBCON_MAX_ROWS)
        g_fb.rows = FBCON_MAX_ROWS;

    memset(g_con, 0, sizeof(g_con));
    g_active = 0;

    g_con[0].live   = 1;
    g_con[0].def_fg = 0x00CCCCCC;
    g_con[0].def_bg = 0x00000000;
    vt_ansi_init(&g_con[0].ansi, g_fb.cols, g_fb.rows);
    clear_console(&g_con[0]);

    dbg_puts("FBCON init g_active=");
    dbg_puts_dec(g_active);
    dbg_puts(" fb=");
    dbg_puts_hexn((uint64_t)(uintptr_t)g_fb.fb, 8);
    dbg_puts(" cols=");
    dbg_puts_dec(g_fb.cols);
    dbg_puts(" rows=");
    dbg_puts_dec(g_fb.rows);
    dbg_puts("\n");
}

int fbcon_alloc(void)
{
    for (int i = 1; i < FBCON_MAX_CONS; i++) {
        if (g_con[i].live)
            continue;
        g_con[i].live   = 1;
        g_con[i].def_fg = 0x00CCCCCC;
        g_con[i].def_bg = 0x00000000;
        vt_ansi_init(&g_con[i].ansi, g_fb.cols, g_fb.rows);
        clear_console(&g_con[i]); /* not active: cells only */
        return i;
    }
    return -1;
}

void fbcon_activate(int con)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live || con == g_active)
        return;
    dbg_puts("FBCON activate con=");
    dbg_puts_dec(con);
    dbg_puts(" from=");
    dbg_puts_dec(g_active);
    dbg_puts("\n");
    g_active = con;

    /*
     * Wipe first: repaint() walks the text grid, while the framebuffer may be
     * larger than the clamped grid (a mode above 2048x1536); any pixels
     * outside the grid belong to the previous console.
     */
    if (g_fb.fb && !g_suppressed) {
        for (uint32_t y = 0; y < g_fb.h; y++)
            for (uint32_t x = 0; x < g_fb.stride; x++)
                g_fb.fb[y * g_fb.stride + x] = g_con[con].def_bg;
    }
    repaint(&g_con[con]);
}

int fbcon_active(void)
{
    return g_active;
}

/* ---- per-console output ------------------------------------------------ */
static console_t *pick(int con)
{
    if (con < 0 || con >= FBCON_MAX_CONS || !g_con[con].live)
        return NULL;
    return &g_con[con];
}

void fbcon_suppress(int on)
{
    g_suppressed = on;
}

void fbcon_putc_on(int con, char c)
{
    console_t *t;
    if (g_suppressed)
        return;
    t = pick(con);
    if (t)
        vt_ansi_put_byte(&t->ansi, (uint8_t)c, &g_ansi_cb, t);
}

void fbcon_lf_on(int con)
{
    console_t *t;
    if (g_suppressed)
        return;
    t = pick(con);
    if (t)
        vt_ansi_line_feed(&t->ansi, &g_ansi_cb, t);
}

void fbcon_clear_on(int con)
{
    console_t *t;
    if (g_suppressed)
        return;
    dbg_puts("FBCON fbcon_clear_on con=");
    dbg_puts_dec(con);
    dbg_puts("\n");
    t = pick(con);
    if (t) {
        clear_console(t);
        vt_ansi_resize(&t->ansi, g_fb.cols, g_fb.rows);
    }
}

void fbcon_set_color_on(int con, uint32_t fg, uint32_t bg)
{
    console_t *t = pick(con);
    if (!t)
        return;
    /* This old API set raw pixels; keep it meaningful by mapping it to the
     * default colours and clearing, so callers still get "reset". */
    t->def_fg      = fg;
    t->def_bg      = bg;
    t->ansi.fg     = 0;
    t->ansi.bg     = 0;
    t->ansi.attrs  = 0;
}

/* ---- console 0 --------------------------------------------------------- */
void fbcon_putc(char c)
{
    fbcon_putc_on(0, c);
}
void fbcon_lf(void)
{
    fbcon_lf_on(0);
}
void fbcon_clear(void)
{
    fbcon_clear_on(0);
}
void fbcon_set_color(uint32_t fg, uint32_t bg)
{
    fbcon_set_color_on(0, fg, bg);
}

void fbcon_puts(const char *s)
{
    for (; *s; s++)
        fbcon_putc_on(0, (uint8_t)*s);
}

void fbcon_size(uint32_t *cols, uint32_t *rows)
{
    if (cols)
        *cols = g_fb.cols;
    if (rows)
        *rows = g_fb.rows;
}

/*
 * The framebuffer's pixel geometry, for the drivers that draw into the same
 * memory the console owns (the DRM driver's SETCRTC blit) and for fbdev's
 * ioctls, which must report the *current* mode rather than the one the
 * bootloader set up before the first modeset.
 */
void fbcon_geometry(uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    if (w)
        *w = g_fb.w;
    if (h)
        *h = g_fb.h;
    if (pitch)
        *pitch = g_fb.stride * 4;
}

void *fbcon_fb(void)
{
    return g_fb.fb;
}

uint32_t fbcon_pitch(void)
{
    return g_fb.stride * 4;
}

/*
 * A modeset changed the scanout resolution.  Re-derive the text grid, clamp
 * every console's cursor and scroll region into it, and repaint the active
 * console so the screen does not come back as garbage from the old mode's
 * pitch.  The framebuffer pointer itself never moves -- mode switches on the
 * bochs VBE keep the same linear BAR.
 */
void fbcon_resize(uint32_t w, uint32_t h, uint32_t pitch)
{
    if (w < 320 || h < 200 || pitch < w * 4)
        return;

    g_fb.w      = w;
    g_fb.h      = h;
    g_fb.stride = pitch / 4;
    g_fb.cols   = (w / FONT_WIDTH) > FBCON_MAX_COLS ? FBCON_MAX_COLS : w / FONT_WIDTH;
    g_fb.rows   = (h / FONT_HEIGHT) > FBCON_MAX_ROWS ? FBCON_MAX_ROWS : h / FONT_HEIGHT;

    for (int i = 0; i < FBCON_MAX_CONS; i++) {
        console_t *c = &g_con[i];
        if (!c->live)
            continue;
        /* The old cells were laid out for the old width; repainting a
         * half-stale buffer would smear half-rows across the screen. */
        clear_cells(c);
        vt_ansi_resize(&c->ansi, g_fb.cols, g_fb.rows);
    }

    clear_console(&g_con[g_active]);
    repaint(&g_con[g_active]);
    dbg_puts("FBCON: resized to ");
    dbg_puts_dec(g_fb.cols);
    dbg_puts("x");
    dbg_puts_dec(g_fb.rows);
    dbg_puts(" glyphs\r\n");
}

void fbcon_panic(void)
{
    /* Panic output must always be visible, client scanout or not. */
    g_suppressed = 0;
    /* Whatever terminal the user was on, the panic goes on console 0 -- so
     * bring console 0 to the front before painting it red. */
    fbcon_activate(0);
    g_active = 0;

    g_con[0].def_bg = 0x00800000; /* dark red */
    g_con[0].def_fg = 0x00FFFFFF;
    clear_console(&g_con[0]);
    g_con[0].ansi.x             = 0;
    g_con[0].ansi.y             = 0;
    g_con[0].ansi.scroll_top    = 0;
    g_con[0].ansi.scroll_bottom = g_fb.rows ? g_fb.rows - 1 : 0;
}
