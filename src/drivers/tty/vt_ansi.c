/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vt_ansi.c — the VT100/ANSI escape engine. (GPLv2)
 *
 * This file owns the byte stream: UTF-8 decoding, the C0 control characters
 * that a terminal acts on (\n, \r, \b, \t) and the escape sequences a shell,
 * ls --color or a full-screen TUI sends.  It does not own any cells or
 * pixels; every visible effect is a callback into fbcon.  That split matches
 * Uinxed's drivers/gpu/fbdev/fbcon.c + drivers/tty/vt_ansi.c layout.
 *
 * The state is per terminal, so an escape sequence interrupted by a write to
 * another VT cannot bleed across.
 */
#include <stdint.h>

#include "vt_ansi.h"

/* The 16 ANSI colours in x8r8g8b8.  Entry 0 is never drawn directly: a cell
 * colour of 0 means "the console's default", resolved by the driver. */
const uint32_t vt_ansi_palette[16] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500, /* black red green yellow */
    0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA, /* blue magenta cyan white */
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55, /* bright variants */
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

enum {
    ST_GROUND,
    ST_ESC,
    ST_CSI,
    ST_OSC,
    ST_OSC_ESC,
    ST_CHARSET, /* ESC ( ) * + # % : swallow the one designator byte */
};

/* ---- cursor / scrolling ------------------------------------------------ */

static void ansi_newline(vt_ansi_t *s, const vt_ansi_callbacks_t *cb, void *ctx);

static void ansi_touch(vt_ansi_t *s, const vt_ansi_callbacks_t *cb, void *ctx)
{
    if (cb->touch)
        cb->touch(ctx, s->x, s->y);
}

static int ansi_param(const vt_ansi_t *s, int idx, int def)
{
    return (idx < (int)s->nparams) ? (int)s->params[idx] : def;
}

void vt_ansi_line_feed(vt_ansi_t *s, const vt_ansi_callbacks_t *cb, void *ctx)
{
    if (s->rows == 0)
        return;
    if (s->y >= s->scroll_bottom) {
        /* At the bottom of the scroll region: scroll instead of walking the
         * cursor off the end.  The old fbcon_lf_on() incremented first and
         * then scrolled, leaving y == rows and silently dropping every
         * subsequent character. */
        cb->scroll_up(ctx, s->scroll_top, s->scroll_bottom, 1);
        s->y = s->scroll_bottom;
    } else if (++s->y >= s->rows) {
        s->y = s->rows - 1;
    }
}

static void ansi_newline(vt_ansi_t *s, const vt_ansi_callbacks_t *cb, void *ctx)
{
    s->x = 0;
    vt_ansi_line_feed(s, cb, ctx);
}

static void ansi_put_codepoint(vt_ansi_t *s, uint32_t cp, int wide,
                               const vt_ansi_callbacks_t *cb, void *ctx)
{
    if (s->cols == 0 || s->rows == 0)
        return;

    /* A double-width glyph that would run off the right edge wraps first. */
    if (wide && s->x + 1 >= s->cols)
        ansi_newline(s, cb, ctx);
    if (s->x >= s->cols)
        ansi_newline(s, cb, ctx);

    cb->draw_char(ctx, s->x, s->y, cp, wide);

    s->x += wide ? 2 : 1;
    if (s->x >= s->cols)
        ansi_newline(s, cb, ctx);
}

/* ---- UTF-8 -------------------------------------------------------------- */

/* Decode one byte; returns 0 while a multi-byte sequence is incomplete, or a
 * code point (possibly U+FFFD for an illegal input) when one is ready. */
static uint32_t utf8_feed(vt_ansi_t *s, uint8_t b)
{
    if (s->utf_need == 0) {
        if (b < 0x80) {
            return b;
        } else if ((b & 0xE0) == 0xC0) { /* 110xxxxx: 2 bytes */
            if ((b & 0x1E) == 0)
                return 0xFFFD; /* 0xC0/0xC1 lead: overlong */
            s->utf_cp    = b & 0x1F;
            s->utf_need  = 1;
            s->utf_got   = 0;
            s->utf_lower = 0x80;
        } else if ((b & 0xF0) == 0xE0) { /* 1110xxxx: 3 bytes */
            s->utf_cp    = b & 0x0F;
            s->utf_need  = 2;
            s->utf_got   = 0;
            s->utf_lower = 0x80;
        } else if ((b & 0xF8) == 0xF0) { /* 11110xxx: 4 bytes */
            if ((b & 0x07) == 0)
                return 0xFFFD; /* 0xF0+0x0..3: overlong */
            s->utf_cp    = b & 0x07;
            s->utf_need  = 3;
            s->utf_got   = 0;
            s->utf_lower = 0x80;
        } else {
            return 0xFFFD; /* 0x80..0xBF stray, or 0xF8+ */
        }
        return 0;
    }

    /* continuation byte expected */
    if ((b & 0xC0) != 0x80)
        goto bad;
    if (b < s->utf_lower)
        goto bad;
    s->utf_lower = 0x80;
    s->utf_cp    = (s->utf_cp << 6) | (b & 0x3F);
    if (++s->utf_got < s->utf_need)
        return 0;

    /* complete */
    uint32_t cp = s->utf_cp;
    s->utf_need = 0;
    if (cp > 0x10FFFF)
        return 0xFFFD; /* beyond Unicode */
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0xFFFD; /* UTF-16 surrogates */
    return cp;

bad:
    s->utf_need = 0;
    return 0xFFFD;
}

/* ---- escape sequence parsing ------------------------------------------- */

static void esc_param_push(vt_ansi_t *s, int digit)
{
    if (s->nparams == 0) {
        s->nparams   = 1;
        s->params[0] = 0;
    }
    uint32_t *p = &s->params[s->nparams - 1];
    if (*p < 100000)
        *p = *p * 10 + (uint32_t)digit;
}

/* A ';' starts the next parameter, even when the current one was omitted:
 * ESC[;5H means "default row, column 5", so the empty slot must stay empty. */
static void esc_param_next(vt_ansi_t *s)
{
    if (s->nparams == 0) {
        s->nparams   = 1;
        s->params[0] = 0;
    }
    if (s->nparams < VT_ANSI_MAX_PARAMS) {
        s->params[s->nparams] = 0;
        s->nparams++;
    }
}

static void save_cursor(vt_ansi_t *s)
{
    s->saved_x     = s->x;
    s->saved_y     = s->y;
    s->saved_fg    = s->fg;
    s->saved_bg    = s->bg;
    s->saved_attrs = s->attrs;
}

static void restore_cursor(vt_ansi_t *s, const vt_ansi_callbacks_t *cb,
                           void *ctx)
{
    s->x     = s->saved_x;
    s->y     = s->saved_y;
    s->fg    = s->saved_fg;
    s->bg    = s->saved_bg;
    s->attrs = s->saved_attrs;
    ansi_touch(s, cb, ctx);
}

static void alt_screen_enter(vt_ansi_t *s, const vt_ansi_callbacks_t *cb,
                             void *ctx)
{
    if (s->alt_active)
        return;
    if (cb->alt_screen(ctx, 1) != 0)
        return;

    s->alt_x              = s->x;
    s->alt_y              = s->y;
    s->alt_fg             = s->fg;
    s->alt_bg             = s->bg;
    s->alt_attrs          = s->attrs;
    s->alt_scroll_top     = s->scroll_top;
    s->alt_scroll_bottom  = s->scroll_bottom;
    s->alt_active         = 1;

    s->x            = 0;
    s->y            = 0;
    s->scroll_top   = 0;
    s->scroll_bottom = s->rows ? s->rows - 1 : 0;
}

static void alt_screen_exit(vt_ansi_t *s, const vt_ansi_callbacks_t *cb,
                            void *ctx)
{
    if (!s->alt_active)
        return;
    cb->alt_screen(ctx, 0);
    s->alt_active = 0;

    s->x            = s->alt_x;
    s->y            = s->alt_y;
    s->fg           = s->alt_fg;
    s->bg           = s->alt_bg;
    s->attrs        = s->alt_attrs;
    s->scroll_top   = s->alt_scroll_top;
    s->scroll_bottom = s->alt_scroll_bottom;

    cb->repaint(ctx);
}

/* Apply an SGR parameter list.  A leading 8-bit colour mode in the 38/48
 * family consumes the following parameters. */
static void apply_sgr(vt_ansi_t *s)
{
    int count = s->nparams;

    /* ESC[m is SGR 0 (reset), not a no-op. */
    if (count == 0) {
        s->attrs = 0;
        s->fg    = 0;
        s->bg    = 0;
        return;
    }

    for (int i = 0; i < count; i++) {
        int p = (int)s->params[i];
        switch (p) {
        case 0:
            s->attrs = 0;
            s->fg    = 0;
            s->bg    = 0;
            break;
        case 1:
            s->attrs |= VT_ANSI_ATTR_BOLD;
            break;
        case 4:
            s->attrs |= VT_ANSI_ATTR_UNDERLINE;
            break;
        case 7:
            s->attrs |= VT_ANSI_ATTR_REVERSE;
            break;
        case 22:
            s->attrs &= (uint8_t)~VT_ANSI_ATTR_BOLD;
            break;
        case 24:
            s->attrs &= (uint8_t)~VT_ANSI_ATTR_UNDERLINE;
            break;
        case 27:
            s->attrs &= (uint8_t)~VT_ANSI_ATTR_REVERSE;
            break;
        case 30 ... 37:
            s->fg = (uint8_t)(p - 30);
            break;
        case 39:
            s->fg = 0;
            break;
        case 40 ... 47:
            s->bg = (uint8_t)(p - 40);
            break;
        case 49:
            s->bg = 0;
            break;
        case 90 ... 97:
            s->fg = (uint8_t)(p - 90 + 8);
            break;
        case 100 ... 107:
            s->bg = (uint8_t)(p - 100 + 8);
            break;
        case 38:
        case 48: {
            /* 38;5;n / 38;2;r;g;b ; foreground when p == 38 */
            int mode = ansi_param(s, i + 1, 0);
            if (mode == 5) {
                int n = ansi_param(s, i + 2, 0);
                if (p == 38)
                    s->fg = (uint8_t)n;
                else
                    s->bg = (uint8_t)n;
                i += 2;
            } else if (mode == 2) {
                /* Map 24-bit RGB to the nearest of the 16 palette colours;
                 * the console has no truecolour cells. */
                int r    = ansi_param(s, i + 2, 0);
                int g    = ansi_param(s, i + 3, 0);
                int b    = ansi_param(s, i + 4, 0);
                int best = 0, bestd = 0x7FFFFFFF;
                for (int k = 1; k < 16; k++) {
                    int pr = (int)((vt_ansi_palette[k] >> 16) & 0xFF);
                    int pg = (int)((vt_ansi_palette[k] >> 8) & 0xFF);
                    int pb = (int)(vt_ansi_palette[k] & 0xFF);
                    int d  = (r - pr) * (r - pr) + (g - pg) * (g - pg) +
                             (b - pb) * (b - pb);
                    if (d < bestd) {
                        bestd = d;
                        best  = k;
                    }
                }
                if (p == 38)
                    s->fg = (uint8_t)best;
                else
                    s->bg = (uint8_t)best;
                i += 4;
            }
            break;
        }
        default:
            break; /* unknown SGR attribute: ignore, as a terminal would */
        }
    }
}

static void csi_dispatch(vt_ansi_t *s, char fin, const vt_ansi_callbacks_t *cb,
                         void *ctx)
{
    int n, m;

    switch (fin) {
    case 'm':
        apply_sgr(s);
        break;

    case 'A': /* CUU: cursor up */
        n    = ansi_param(s, 0, 1);
        s->y = ((uint32_t)n > s->y) ? 0 : s->y - (uint32_t)n;
        ansi_touch(s, cb, ctx);
        break;
    case 'B': /* CUD: cursor down */
        n    = ansi_param(s, 0, 1);
        s->y = (s->y + (uint32_t)n >= s->rows) ? s->rows - 1 : s->y + (uint32_t)n;
        ansi_touch(s, cb, ctx);
        break;
    case 'C': /* CUF: cursor right */
        n    = ansi_param(s, 0, 1);
        s->x = (s->x + (uint32_t)n >= s->cols) ? s->cols - 1 : s->x + (uint32_t)n;
        ansi_touch(s, cb, ctx);
        break;
    case 'D': /* CUB: cursor left */
        n    = ansi_param(s, 0, 1);
        s->x = ((uint32_t)n > s->x) ? 0 : s->x - (uint32_t)n;
        ansi_touch(s, cb, ctx);
        break;
    case 'E': /* CNL: down n, column 0 */
        n    = ansi_param(s, 0, 1);
        s->y = (s->y + (uint32_t)n >= s->rows) ? s->rows - 1 : s->y + (uint32_t)n;
        s->x = 0;
        break;
    case 'F': /* CPL: up n, column 0 */
        n    = ansi_param(s, 0, 1);
        s->y = ((uint32_t)n > s->y) ? 0 : s->y - (uint32_t)n;
        s->x = 0;
        break;
    case 'G': /* CHA: cursor to column n (1-based) */
        n    = ansi_param(s, 0, 1);
        s->x = (n <= 0) ? 0 : (n > (int)s->cols ? s->cols - 1 : (uint32_t)(n - 1));
        ansi_touch(s, cb, ctx);
        break;
    case 'd': /* VPA: vertical position absolute, row n */
        n    = ansi_param(s, 0, 1);
        s->y = (n <= 0) ? 0 : (n > (int)s->rows ? s->rows - 1 : (uint32_t)(n - 1));
        break;

    case 'H':
    case 'f': /* CUP / HVP: row;col (1-based) */
        n    = ansi_param(s, 0, 1);
        m    = ansi_param(s, 1, 1);
        s->y = (n <= 0) ? 0 : (n > (int)s->rows ? s->rows - 1 : (uint32_t)(n - 1));
        s->x = (m <= 0) ? 0 : (m > (int)s->cols ? s->cols - 1 : (uint32_t)(m - 1));
        ansi_touch(s, cb, ctx);
        break;

    case 'J': /* ED: erase in display */
        n = ansi_param(s, 0, 0);
        cb->erase_display(ctx, (uint32_t)n, s->x, s->y);
        if (n == 2 || n == 3) {
            s->x = 0;
            s->y = 0;
        }
        cb->repaint(ctx);
        break;

    case 'K': /* EL: erase in line */
        n = ansi_param(s, 0, 0);
        cb->erase_line(ctx, (uint32_t)n, s->x, s->y);
        cb->repaint(ctx);
        break;

    case 'L': /* IL: insert n lines at cursor */
        n = ansi_param(s, 0, 1);
        cb->insert_lines(ctx, s->y, s->scroll_bottom, (uint32_t)n);
        cb->repaint(ctx);
        break;

    case 'M': /* DL: delete n lines at cursor */
        n = ansi_param(s, 0, 1);
        cb->delete_lines(ctx, s->y, s->scroll_bottom, (uint32_t)n);
        cb->repaint(ctx);
        break;

    case '@': /* ICH: insert n chars at cursor (shift right) */
        n = ansi_param(s, 0, 1);
        cb->insert_chars(ctx, s->x, s->y, (uint32_t)n, s->cols);
        cb->repaint(ctx);
        break;

    case 'P': /* DCH: delete n chars at cursor (shift left) */
        n = ansi_param(s, 0, 1);
        cb->delete_chars(ctx, s->x, s->y, (uint32_t)n, s->cols);
        cb->repaint(ctx);
        break;

    case 'X': /* ECH: erase n chars at cursor */
        n = ansi_param(s, 0, 1);
        cb->erase_chars(ctx, s->x, s->y, (uint32_t)n);
        cb->repaint(ctx);
        break;

    case 'S': /* SU: scroll up n lines */
        n = ansi_param(s, 0, 1);
        cb->scroll_up(ctx, s->scroll_top, s->scroll_bottom, (uint32_t)n);
        cb->repaint(ctx);
        break;

    case 'T': /* SD: scroll down n lines */
        n = ansi_param(s, 0, 1);
        cb->scroll_down(ctx, s->scroll_top, s->scroll_bottom, (uint32_t)n);
        cb->repaint(ctx);
        break;

    case 'r': /* DECSTBM: set scroll region [top;bottom] (1-based) */
        n = ansi_param(s, 0, 1);
        m = ansi_param(s, 1, (int)s->rows);
        if (n >= 1 && m >= 1 && (uint32_t)n <= s->rows &&
            (uint32_t)m <= s->rows && n < m) {
            s->scroll_top    = (uint32_t)n - 1;
            s->scroll_bottom = (uint32_t)m - 1;
        }
        break;

    case 'h':
    case 'l': /* DEC private modes via '?' */
        if (s->private == '?') {
            for (int i = 0; i < (int)s->nparams; i++) {
                int p = (int)s->params[i];
                if (p == 25) { /* DECTCEM: visible cursor */
                    s->cursor_visible = (fin == 'h');
                } else if (p == 1049 || p == 1047 || p == 47) {
                    if (fin == 'h')
                        alt_screen_enter(s, cb, ctx);
                    else
                        alt_screen_exit(s, cb, ctx);
                }
            }
        }
        break;

    case 's': /* save cursor */
        save_cursor(s);
        break;
    case 'u': /* restore cursor */
        restore_cursor(s, cb, ctx);
        break;

    default:
        break; /* unrecognised CSI final: ignore */
    }
}

/* Feed one byte through the escape parser.  Returns 1 when the byte was
 * consumed as part of a sequence, 0 when the caller still has to interpret
 * it as a printable/control byte. */
static int esc_feed(vt_ansi_t *s, uint8_t b, const vt_ansi_callbacks_t *cb,
                    void *ctx)
{
    switch (s->state) {
    case ST_GROUND:
        if (b == 0x1B) { /* ESC */
            s->state = ST_ESC;
            return 1;
        }
        return 0;

    case ST_ESC:
        if (b == '[') {
            s->state     = ST_CSI;
            s->nparams   = 0;
            s->private   = 0;
            s->params[0] = 0;
            return 1;
        }
        if (b == ']') { /* OSC: title/colour, body ignored */
            s->state   = ST_OSC;
            s->osc_len = 0;
            return 1;
        }
        if (b == 'c') { /* RIS: full reset */
            s->state = ST_GROUND;
            if (s->alt_active) {
                alt_screen_exit(s, cb, ctx);
            } else {
                s->x = 0;
                s->y = 0;
                cb->clear(ctx);
            }
            s->attrs          = 0;
            s->fg             = 0;
            s->bg             = 0;
            s->cursor_visible = 1;
            s->scroll_top     = 0;
            s->scroll_bottom  = s->rows ? s->rows - 1 : 0;
            return 1;
        }
        if (b == '7') { /* DECSC: save cursor + attrs */
            s->state = ST_GROUND;
            save_cursor(s);
            return 1;
        }
        if (b == '8') { /* DECRC: restore cursor + attrs */
            s->state = ST_GROUND;
            restore_cursor(s, cb, ctx);
            return 1;
        }
        if (b == '=' || b == '>') { /* DECKPAM/DECKPNM: keypad mode */
            s->state = ST_GROUND;
            return 1;
        }
        if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' ||
            b == '%') {
            /* Charset/line-attribute designation: the next byte is a
             * designator, not text.  The old parser returned to ground here,
             * so ESC ( B used to draw a stray 'B'. */
            s->state = ST_CHARSET;
            return 1;
        }
        /* Unknown two-byte escape: consume it.  Dropping the second byte is
         * what a terminal does; emitting it used to put stray letters on
         * screen after sequences we do not implement. */
        s->state = ST_GROUND;
        return 1;

    case ST_CSI:
        if (b == '?') {
            s->private = '?';
            return 1;
        }
        if (b >= '0' && b <= '9') {
            esc_param_push(s, b - '0');
            return 1;
        }
        if (b == ';') {
            esc_param_next(s);
            return 1;
        }
        if (b == ':') /* sub-parameter separator: ignore */
            return 1;
        if (b >= 0x20 && b <= 0x2F) /* intermediate: ignore */
            return 1;
        if (b >= 0x40 && b <= 0x7E) { /* final byte: dispatch */
            csi_dispatch(s, (char)b, cb, ctx);
            s->state = ST_GROUND;
            return 1;
        }
        if (b == 0x1B) { /* a new escape aborts the broken one */
            s->state = ST_ESC;
            return 1;
        }
        /* anything else: abandon the sequence and reprocess the byte */
        s->state = ST_GROUND;
        return 0;

    case ST_OSC:
        if (b == 0x07) { /* BEL terminates */
            s->state = ST_GROUND;
        } else if (b == 0x1B) { /* or ESC \ (ST) */
            s->state = ST_OSC_ESC;
        } else if (s->osc_len < VT_ANSI_OSC_MAX - 1) {
            s->osc[s->osc_len++] = (char)b;
        }
        return 1;

    case ST_OSC_ESC:
        s->state = ST_GROUND;
        /* ESC \ is the real terminator; any other byte after ESC is a fresh
         * sequence and must be reprocessed. */
        return b == '\\';

    case ST_CHARSET:
        s->state = ST_GROUND;
        return 1;
    }

    s->state = ST_GROUND;
    return 0;
}

/* ---- public entry points ----------------------------------------------- */

void vt_ansi_init(vt_ansi_t *s, uint32_t cols, uint32_t rows)
{
    *s = (vt_ansi_t){0};
    s->cols            = cols;
    s->rows            = rows;
    s->scroll_bottom   = rows ? rows - 1 : 0;
    s->cursor_visible  = 1;
    s->state           = ST_GROUND;
}

void vt_ansi_resize(vt_ansi_t *s, uint32_t cols, uint32_t rows)
{
    s->cols          = cols;
    s->rows          = rows;
    s->x             = 0;
    s->y             = 0;
    s->scroll_top    = 0;
    s->scroll_bottom = rows ? rows - 1 : 0;
}

void vt_ansi_put_byte(vt_ansi_t *s, uint8_t b, const vt_ansi_callbacks_t *cb,
                      void *ctx)
{
    /* Control characters still mean something mid-sequence: CR or LF must
     * act now, not wait for the sequence to end. */
    if (esc_feed(s, b, cb, ctx))
        return;

    switch (b) {
    case '\n':
        ansi_newline(s, cb, ctx);
        return;
    case '\r':
        s->x = 0;
        return;
    case '\b':
        if (s->x > 0) {
            s->x--;
        } else if (s->y > 0) {
            s->y--;
            s->x = s->cols ? s->cols - 1 : 0;
        }
        /* ECHOE semantics: rub the glyph out, not just revisit the cell. */
        cb->draw_char(ctx, s->x, s->y, ' ', 0);
        return;
    case '\t':
        if (s->cols == 0)
            return;
        do {
            vt_ansi_put_byte(s, ' ', cb, ctx);
        } while (s->x & 7);
        return;
    }

    uint32_t cp = utf8_feed(s, b);
    if (cp == 0) /* mid-sequence: nothing to draw yet */
        return;

    int w = cb->char_width(ctx, cp);
    if (w <= 0) /* control / combining: take no cell */
        return;

    ansi_put_codepoint(s, cp, w == 2, cb, ctx);
}
