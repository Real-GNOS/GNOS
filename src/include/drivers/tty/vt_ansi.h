/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vt_ansi.h — the VT100/ANSI escape engine used by the framebuffer console.
 * (GPLv2)
 *
 * This is GNOS's counterpart of Uinxed's drivers/tty/vt_ansi.c: the byte
 * stream a terminal receives is decoded here (UTF-8, control characters and
 * the ECMA-48/CSI sequences programs emit), while the actual cell grid and
 * pixels live in the fbcon driver.  The two are joined by the callback table
 * below, so the parser knows nothing about framebuffers or fonts.
 *
 * One vt_ansi_t belongs to one virtual terminal.  Keeping the parser state
 * per terminal matters: a half-received escape sequence on tty3 must not be
 * continued by a byte written to tty1.
 */
#ifndef GNUCOS_VT_ANSI_H
#define GNUCOS_VT_ANSI_H

#include <stdint.h>

#define VT_ANSI_MAX_PARAMS 16
#define VT_ANSI_OSC_MAX    256

/* Attribute bits shared with fbcon's cell flags (CELL_BOLD etc.). */
enum {
    VT_ANSI_ATTR_BOLD      = 0x01,
    VT_ANSI_ATTR_REVERSE   = 0x02,
    VT_ANSI_ATTR_UNDERLINE = 0x04,
};

/*
 * The parser's own state plus everything a terminal has to remember between
 * bytes: cursor, colours, scroll region, saved cursor, the alternate-screen
 * bookkeeping and the partial UTF-8/escape sequences.
 */
typedef struct vt_ansi {
    uint32_t cols, rows;

    uint32_t x, y;
    uint32_t scroll_top, scroll_bottom;

    uint8_t fg, bg; /* palette index, 0 = console default colour */
    uint8_t attrs;  /* VT_ANSI_ATTR_*                           */
    uint8_t cursor_visible;

    uint32_t saved_x, saved_y;
    uint8_t  saved_fg, saved_bg, saved_attrs;

    /* Alternate screen (?1049h): terminal state saved on entry.  The cell
     * buffer itself is swapped by the driver through alt_screen(). */
    uint8_t  alt_active;
    uint32_t alt_x, alt_y;
    uint8_t  alt_fg, alt_bg, alt_attrs;
    uint32_t alt_scroll_top, alt_scroll_bottom;

    /* Escape/UTF-8 decoders. */
    uint8_t state;
    uint8_t private;
    uint8_t  nparams;
    uint32_t params[VT_ANSI_MAX_PARAMS];

    uint16_t osc_len;
    char     osc[VT_ANSI_OSC_MAX];

    uint32_t utf_cp;
    uint8_t  utf_need, utf_got, utf_lower;
} vt_ansi_t;

/*
 * What the engine asks the console driver to do.  Coordinates are in cells.
 * Iteration counts (lines/chars) are already resolved from CSI parameters.
 */
typedef struct vt_ansi_callbacks {
    /* Draw one code point at (x, y).  `wide` marks a double-width glyph; the
     * driver marks the cell to the right as a continuation.  Colours and
     * attributes are read from the vt_ansi_t passed to the engine. */
    void (*draw_char)(void *ctx, uint32_t x, uint32_t y, uint32_t cp, int wide);
    /* Redraw one cell (the cursor moved onto it). */
    void (*touch)(void *ctx, uint32_t x, uint32_t y);
    /* Blank ranges; the engine calls repaint() afterwards. */
    void (*erase_display)(void *ctx, uint32_t mode, uint32_t x, uint32_t y);
    void (*erase_line)(void *ctx, uint32_t mode, uint32_t x, uint32_t y);
    void (*erase_chars)(void *ctx, uint32_t x, uint32_t y, uint32_t count);
    void (*insert_lines)(void *ctx, uint32_t y, uint32_t bottom, uint32_t count);
    void (*delete_lines)(void *ctx, uint32_t y, uint32_t bottom, uint32_t count);
    void (*insert_chars)(void *ctx, uint32_t x, uint32_t y, uint32_t count, uint32_t cols);
    void (*delete_chars)(void *ctx, uint32_t x, uint32_t y, uint32_t count, uint32_t cols);
    /* Scroll [top, bottom] by `count` lines.  scroll_up is also the newline
     * path at the bottom of the region, so it moves pixels as well as cells. */
    void (*scroll_up)(void *ctx, uint32_t top, uint32_t bottom, uint32_t count);
    void (*scroll_down)(void *ctx, uint32_t top, uint32_t bottom, uint32_t count);
    /* Blank the whole console (RIS and "erase display" mode 2). */
    void (*clear)(void *ctx);
    /* Swap the alternate cell buffer; returns 0 on success. */
    int (*alt_screen)(void *ctx, int on);
    /* Repaint the visible grid from the cell buffer. */
    void (*repaint)(void *ctx);
    /* Display width of a code point: 0 = zero-width/control, 1 = narrow,
     * 2 = double-width.  Provided by the font layer. */
    int (*char_width)(void *ctx, uint32_t cp);
} vt_ansi_callbacks_t;

/* The 16-colour ANSI palette in x8r8g8b8.  Index 0 is the "default" slot and
 * is resolved to the console's own colour by the driver. */
extern const uint32_t vt_ansi_palette[16];

/* Initialise/resize a terminal state.  Resizing homes the cursor and resets
 * the scroll region to the full screen. */
void vt_ansi_init(vt_ansi_t *s, uint32_t cols, uint32_t rows);
void vt_ansi_resize(vt_ansi_t *s, uint32_t cols, uint32_t rows);

/* Feed one output byte (already past OPOST) through the terminal. */
void vt_ansi_put_byte(vt_ansi_t *s, uint8_t b, const vt_ansi_callbacks_t *cb, void *ctx);

/* A bare line feed: move down one row without changing the column.  Used by
 * the tty layer when OPOST/ONLCR are off, where '\n' must not behave like
 * a carriage return as well. */
void vt_ansi_line_feed(vt_ansi_t *s, const vt_ansi_callbacks_t *cb, void *ctx);

#endif
