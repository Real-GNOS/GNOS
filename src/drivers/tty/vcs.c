/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vcs.c — /dev/vcs and /dev/vcsa: the virtual console screen as a file.
 * (GPLv2)
 *
 * Linux exposes the VT's screen memory as plain character devices: vcs
 * reads and writes the characters, vcsa interleaves them with attribute
 * bytes and opens with a four-byte header (rows, cols, cursor row, cursor
 * col).  Here the "screen memory" is the fbcon cell buffer of the ACTIVE
 * console, and every access goes through the grid rather than a hardware
 * text mode -- the cells are the truth, the framebuffer is just paint.
 *
 * The file offset maps linearly: offset / cols = row, offset % cols = col.
 * Writing updates the cells (UTF-8 on input, like everything else in the
 * kernel); reading emits UTF-8 back.
 */
#include <stdint.h>

#include "vcs.h"
#include "vfs.h"
#include "fbcon.h"
#include "kstring.h"
#include "debugcon.h"

/* UTF-8 encode one code point; returns bytes written (1..4). */
static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static uint32_t screen_cols, screen_rows;

static int vcs_read(struct vfs_node *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    int      attr_mode = (int)(uintptr_t)(n && n->priv); /* vcsa vs vcs */
    uint8_t *out       = buf;
    uint32_t done      = 0;

    fbcon_size(&screen_cols, &screen_rows);
    uint32_t cells = screen_cols * screen_rows;

    if (attr_mode) {
        /* the four-byte header only exists at the very start */
        if (off < 4) {
            if (len < 4 - off)
                return -E_INVAL;
            uint32_t cx = 0, cy = 0;
            /* cursor position is not exported by fbcon; report 0,0 */
            (void)cx;
            (void)cy;
            out[0] = (uint8_t)screen_rows;
            out[1] = (uint8_t)screen_cols;
            out[2] = 0;
            out[3] = 0;
            done   = 4 - (uint32_t)off;
            out += done;
            off = 4;
        } else {
            off -= 4;
        }
        off /= 2; /* attr bytes alternate with chars */
        cells = (cells * 2 + 4 + 1) / 2;
        for (uint32_t i = 0; done < len && off + i < screen_cols * screen_rows; i++) {
            uint32_t cp;
            uint8_t  attr;
            uint32_t idx = off + i;
            if (i & 1) {
                /* attribute slot */
                fbcon_get_cell(0, idx / screen_cols, idx % screen_cols, NULL, &attr);
                out[done++] = attr;
            } else {
                fbcon_get_cell(0, idx / screen_cols, idx % screen_cols, &cp, NULL);
                /* vcsa holds one byte per cell: emit '?' for non-ASCII */
                out[done++] = cp < 128 ? (uint8_t)cp : '?';
            }
        }
        return (int)done;
    }

    /* plain vcs: UTF-8 characters, offset = cell index */
    uint32_t idx = (uint32_t)off;
    for (; done < len && idx < cells; idx++) {
        uint32_t cp;
        if (fbcon_get_cell(0, idx / screen_cols, idx % screen_cols, &cp, NULL) < 0)
            break;
        if (cp == 0)
            cp = ' ';
        done += (uint32_t)utf8_encode(cp, (char *)out + done);
    }
    return (int)done;
}

static int32_t vcs_write(struct vfs_node *n, uint64_t off, const void *buf, uint32_t len)
{
    (void)n;
    uint32_t cols, rows;
    fbcon_size(&cols, &rows);
    uint32_t       idx  = (uint32_t)off;
    const uint8_t *p    = buf;
    uint32_t       done = 0;

    while (done < len && idx < cols * rows) {
        uint32_t cp = 0xFFFD;
        uint8_t  c  = p[done++];
        if (c < 0x80)
            cp = c;
        else if ((c & 0xE0) == 0xC0 && done < len) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (p[done++] & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && done + 1 < len) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[done++] & 0x3F) << 6) |
                 (p[done++] & 0x3F);
        }
        fbcon_set_cell(0, idx / cols, idx % cols, cp, 0x07);
        idx++;
    }
    return (int32_t)done;
}

static const vfs_ops_t g_vcs_ops = {
    .read  = vcs_read,
    .write = vcs_write,
};

void vcs_init(void)
{
    vfs_register_dev("vcs", &g_vcs_ops, NULL);       /* active screen  */
    vfs_register_dev("vcsa", &g_vcs_ops, (void *)1); /* with attributes */
    vfs_register_dev("vcs0", &g_vcs_ops, NULL);
    vfs_register_dev("vcsa0", &g_vcs_ops, (void *)1);
}
