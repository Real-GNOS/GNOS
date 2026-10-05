#include "lib/png/png.h"

#include <stddef.h>

#include "kernel/mm/pool/pool.h"
#include "lib/str/str.h"

#define PNG_MAXBITS 15
#define PNG_SIG_LEN 8
#define PNG_MAX_DIM 8192
#define PNG_MAX_PIXELS (16u * 1024u * 1024u)
#define PNG_RGBA(r, g, b, a)                                                   \
    ((uint32_t)((((uint32_t)(a)) << 24) | (((uint32_t)(r)) << 16) |           \
                (((uint32_t)(g)) << 8) | ((uint32_t)(b))))

struct PNG_BITS {
    const uint8_t *in;
    uint32_t len;
    uint32_t pos;
    uint32_t buf;
    int cnt;
};

struct PNG_OUT {
    uint8_t *out;
    uint32_t cap;
    uint32_t pos;
};

struct PNG_HUFF {
    int16_t count[PNG_MAXBITS + 1];
    int16_t symbol[288];
};

static const uint8_t png_sig[PNG_SIG_LEN] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};

static const uint16_t len_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};

static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                      1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                      4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t dist_base[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};

static const uint8_t dist_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                       4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static const uint8_t clc_order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};

static uint8_t g_lengths[320];
static uint8_t g_clc[19];
static struct PNG_HUFF g_lencode;
static struct PNG_HUFF g_distcode;
static struct PNG_HUFF g_clcode;

static uint32_t png_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int png_bits(struct PNG_BITS *s, int need) {
    uint32_t val = s->buf;
    int cnt = s->cnt;
    while (cnt < need) {
        if (s->pos >= s->len)
            return -1;
        val |= (uint32_t)s->in[s->pos++] << cnt;
        cnt += 8;
    }
    s->buf = val >> need;
    s->cnt = cnt - need;
    return (int)(val & ((1u << need) - 1u));
}

static int huff_build(struct PNG_HUFF *h, const uint8_t *lengths, int n) {
    int i, len, left;
    int16_t offs[PNG_MAXBITS + 1];
    for (i = 0; i <= PNG_MAXBITS; i++)
        h->count[i] = 0;
    for (i = 0; i < n; i++) {
        if (lengths[i] > PNG_MAXBITS)
            return -1;
        h->count[lengths[i]]++;
    }
    left = 1;
    for (len = 1; len <= PNG_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (len = 1; len < PNG_MAXBITS; len++)
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; i++) {
        if (lengths[i] != 0)
            h->symbol[offs[lengths[i]]++] = (int16_t)i;
    }
    return 0;
}

static int huff_decode(struct PNG_BITS *s, const struct PNG_HUFF *h) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= PNG_MAXBITS; len++) {
        int b = png_bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(struct PNG_BITS *s, struct PNG_OUT *o,
                         const struct PNG_HUFF *lencode,
                         const struct PNG_HUFF *distcode) {
    for (;;) {
        int sym = huff_decode(s, lencode);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (o->pos >= o->cap)
                return -1;
            o->out[o->pos++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256)
            return 0;
        sym -= 257;
        if (sym >= 29)
            return -1;
        int e = png_bits(s, len_extra[sym]);
        if (e < 0)
            return -1;
        uint32_t length = (uint32_t)len_base[sym] + (uint32_t)e;
        int dsym = huff_decode(s, distcode);
        if (dsym < 0 || dsym >= 30)
            return -1;
        int de = png_bits(s, dist_extra[dsym]);
        if (de < 0)
            return -1;
        uint32_t dist = (uint32_t)dist_base[dsym] + (uint32_t)de;
        if (dist > o->pos)
            return -1;
        while (length-- > 0) {
            if (o->pos >= o->cap)
                return -1;
            o->out[o->pos] = o->out[o->pos - dist];
            o->pos++;
        }
    }
}

static int inflate_fixed(struct PNG_BITS *s, struct PNG_OUT *o) {
    int i;
    for (i = 0; i < 144; i++)
        g_lengths[i] = 8;
    for (; i < 256; i++)
        g_lengths[i] = 9;
    for (; i < 280; i++)
        g_lengths[i] = 7;
    for (; i < 288; i++)
        g_lengths[i] = 8;
    if (huff_build(&g_lencode, g_lengths, 288) != 0)
        return -1;
    for (i = 0; i < 30; i++)
        g_lengths[i] = 5;
    if (huff_build(&g_distcode, g_lengths, 30) != 0)
        return -1;
    return inflate_codes(s, o, &g_lencode, &g_distcode);
}

static int inflate_dynamic(struct PNG_BITS *s, struct PNG_OUT *o) {
    int nlen = png_bits(s, 5);
    int ndist = png_bits(s, 5);
    int ncode = png_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30)
        return -1;
    int i;
    for (i = 0; i < 19; i++)
        g_clc[i] = 0;
    for (i = 0; i < ncode; i++) {
        int b = png_bits(s, 3);
        if (b < 0)
            return -1;
        g_clc[clc_order[i]] = (uint8_t)b;
    }
    if (huff_build(&g_clcode, g_clc, 19) != 0)
        return -1;
    int total = nlen + ndist;
    i = 0;
    while (i < total) {
        int sym = huff_decode(s, &g_clcode);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            g_lengths[i++] = (uint8_t)sym;
            continue;
        }
        int rep, val;
        if (sym == 16) {
            if (i == 0)
                return -1;
            rep = png_bits(s, 2);
            val = g_lengths[i - 1];
        } else if (sym == 17) {
            rep = png_bits(s, 3);
            val = 0;
        } else {
            rep = png_bits(s, 7);
            val = 0;
        }
        if (rep < 0)
            return -1;
        rep += (sym == 16) ? 3 : ((sym == 17) ? 3 : 11);
        while (rep-- > 0 && i < total)
            g_lengths[i++] = (uint8_t)val;
    }
    if (g_lengths[256] == 0)
        return -1;
    if (huff_build(&g_lencode, g_lengths, nlen) != 0)
        return -1;
    if (huff_build(&g_distcode, g_lengths + nlen, ndist) != 0)
        return -1;
    return inflate_codes(s, o, &g_lencode, &g_distcode);
}

static int inflate_stored(struct PNG_BITS *s, struct PNG_OUT *o) {
    int l, nl;
    s->buf = 0;
    s->cnt = 0;
    if (s->pos + 4 > s->len)
        return -1;
    l = (int)(s->in[s->pos] | ((uint32_t)s->in[s->pos + 1] << 8));
    nl = (int)(s->in[s->pos + 2] | ((uint32_t)s->in[s->pos + 3] << 8));
    s->pos += 4;
    if ((l ^ 0xFFFF) != nl)
        return -1;
    if (s->pos + (uint32_t)l > s->len)
        return -1;
    if (o->pos + (uint32_t)l > o->cap)
        return -1;
    memcpy(o->out + o->pos, s->in + s->pos, (size_t)l);
    o->pos += (uint32_t)l;
    s->pos += (uint32_t)l;
    return 0;
}

static int inflate_raw(const uint8_t *in, uint32_t len, uint8_t *out,
                       uint32_t ocap, uint32_t *oused) {
    struct PNG_BITS s;
    struct PNG_OUT o;
    int last = 0;
    s.in = in;
    s.len = len;
    s.pos = 0;
    s.buf = 0;
    s.cnt = 0;
    o.out = out;
    o.cap = ocap;
    o.pos = 0;
    do {
        last = png_bits(&s, 1);
        if (last < 0)
            return -1;
        int type = png_bits(&s, 2);
        if (type < 0)
            return -1;
        int rc;
        if (type == 0)
            rc = inflate_stored(&s, &o);
        else if (type == 1)
            rc = inflate_fixed(&s, &o);
        else if (type == 2)
            rc = inflate_dynamic(&s, &o);
        else
            return -1;
        if (rc != 0)
            return -1;
    } while (!last);
    *oused = o.pos;
    return 0;
}

static int zlib_inflate(const uint8_t *in, uint32_t len, uint8_t *out,
                        uint32_t ocap, uint32_t *oused) {
    if (len < 6)
        return -1;
    int cmf = in[0];
    int flg = in[1];
    if ((cmf & 0x0F) != 8)
        return -1;
    if ((((cmf << 8) | flg) % 31) != 0)
        return -1;
    uint32_t off = 2;
    if (flg & 0x20) {
        if (len < 10)
            return -1;
        off += 4;
    }
    return inflate_raw(in + off, len - off, out, ocap, oused);
}

static int png_check_ihdr(int depth, int color) {
    switch (color) {
    case 0:
        return (depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
                depth == 16)
                   ? 0
                   : -1;
    case 3:
        return (depth == 1 || depth == 2 || depth == 4 || depth == 8) ? 0
                                                                      : -1;
    case 2:
    case 4:
    case 6:
        return (depth == 8 || depth == 16) ? 0 : -1;
    default:
        return -1;
    }
}

static int png_channels(int color) {
    switch (color) {
    case 0:
        return 1;
    case 2:
        return 3;
    case 3:
        return 1;
    case 4:
        return 2;
    default:
        return 4;
    }
}

static int png_sub_sample(const uint8_t *row, int index, int depth) {
    int per = 8 / depth;
    int shift = 8 - depth * ((index % per) + 1);
    return (row[index / per] >> shift) & ((1 << depth) - 1);
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

static void unfilter_row(uint8_t *cur, const uint8_t *prev, int rowbytes,
                         int bpp, int filter) {
    int i;
    for (i = 0; i < rowbytes; i++) {
        int a = (i >= bpp) ? cur[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        int v;
        switch (filter) {
        case 1:
            v = a;
            break;
        case 2:
            v = b;
            break;
        case 3:
            v = (a + b) >> 1;
            break;
        case 4:
            v = paeth(a, b, c);
            break;
        default:
            v = 0;
            break;
        }
        cur[i] = (uint8_t)(cur[i] + v);
    }
}

static int trns_gray_key(const uint8_t *trns, int depth) {
    if (depth == 16)
        return ((int)trns[0] << 8) | trns[1];
    if (depth == 8)
        return trns[1];
    return trns[1] & ((1 << depth) - 1);
}

static int trns_rgb_key(const uint8_t *trns, int idx, int depth) {
    if (depth == 16)
        return ((int)trns[idx * 2] << 8) | trns[idx * 2 + 1];
    return trns[idx * 2 + 1];
}

static void row_to_rgba(const uint8_t *row, int w, int depth, int color,
                        const uint8_t *plte, const uint8_t *trns,
                        uint32_t *dst) {
    int i;
    if (color == 3) {
        for (i = 0; i < w; i++) {
            int idx = (depth == 8) ? row[i] : png_sub_sample(row, i, depth);
            dst[i] = PNG_RGBA(plte[idx * 3 + 0], plte[idx * 3 + 1],
                              plte[idx * 3 + 2], trns ? trns[idx] : 255);
        }
        return;
    }
    if (color == 0) {
        int key = trns ? trns_gray_key(trns, depth) : -1;
        if (depth == 16) {
            for (i = 0; i < w; i++) {
                int s = ((int)row[i * 2] << 8) | row[i * 2 + 1];
                int v = row[i * 2];
                dst[i] = PNG_RGBA(v, v, v, (s == key) ? 0 : 255);
            }
        } else if (depth == 8) {
            for (i = 0; i < w; i++) {
                int s = row[i];
                dst[i] = PNG_RGBA(s, s, s, (s == key) ? 0 : 255);
            }
        } else {
            int maxv = (1 << depth) - 1;
            for (i = 0; i < w; i++) {
                int s = png_sub_sample(row, i, depth);
                int v = s * 255 / maxv;
                dst[i] = PNG_RGBA(v, v, v, (s == key) ? 0 : 255);
            }
        }
        return;
    }
    if (color == 2) {
        int kr = trns ? trns_rgb_key(trns, 0, depth) : -1;
        int kg = trns ? trns_rgb_key(trns, 1, depth) : -1;
        int kb = trns ? trns_rgb_key(trns, 2, depth) : -1;
        if (depth == 16) {
            for (i = 0; i < w; i++) {
                int r = ((int)row[i * 6] << 8) | row[i * 6 + 1];
                int g = ((int)row[i * 6 + 2] << 8) | row[i * 6 + 3];
                int b = ((int)row[i * 6 + 4] << 8) | row[i * 6 + 5];
                int a = (r == kr && g == kg && b == kb) ? 0 : 255;
                dst[i] =
                    PNG_RGBA(row[i * 6], row[i * 6 + 2], row[i * 6 + 4], a);
            }
        } else {
            for (i = 0; i < w; i++) {
                int r = row[i * 3];
                int g = row[i * 3 + 1];
                int b = row[i * 3 + 2];
                int a = (r == kr && g == kg && b == kb) ? 0 : 255;
                dst[i] = PNG_RGBA(r, g, b, a);
            }
        }
        return;
    }
    if (depth == 16) {
        if (color == 4) {
            for (i = 0; i < w; i++)
                dst[i] = PNG_RGBA(row[i * 4], row[i * 4], row[i * 4],
                                  row[i * 4 + 2]);
        } else {
            for (i = 0; i < w; i++)
                dst[i] = PNG_RGBA(row[i * 8], row[i * 8 + 2], row[i * 8 + 4],
                                  row[i * 8 + 6]);
        }
        return;
    }
    if (color == 4) {
        for (i = 0; i < w; i++)
            dst[i] =
                PNG_RGBA(row[i * 2], row[i * 2], row[i * 2], row[i * 2 + 1]);
    } else {
        for (i = 0; i < w; i++)
            dst[i] = PNG_RGBA(row[i * 4], row[i * 4 + 1], row[i * 4 + 2],
                              row[i * 4 + 3]);
    }
}

int png_probe(const void *data, uint32_t len, int *w, int *h, int *depth,
              int *color) {
    const uint8_t *p = (const uint8_t *)data;
    if (!p || len < 33)
        return PNG_ERR_FORMAT;
    if (memcmp(p, png_sig, PNG_SIG_LEN) != 0)
        return PNG_ERR_FORMAT;
    if (memcmp(p + 12, "IHDR", 4) != 0)
        return PNG_ERR_FORMAT;
    if (w)
        *w = (int)png_be32(p + 16);
    if (h)
        *h = (int)png_be32(p + 20);
    if (depth)
        *depth = p[24];
    if (color)
        *color = p[25];
    return PNG_OK;
}

int png_decode(const void *data, uint32_t len, struct PNG_IMAGE *out) {
    static uint8_t plte[256 * 3];
    static uint8_t trns[256];
    const uint8_t *p = (const uint8_t *)data;
    uint32_t off, idat_total = 0, idat_got = 0;
    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    int have_ihdr = 0, have_plte = 0, have_trns = 0, have_iend = 0;
    uint8_t *raw = 0, *idat = 0;
    uint32_t rowbytes, raw_size, got = 0;
    int bpp, channels;

    if (!out)
        return PNG_ERR_FORMAT;
    out->pixels = 0;
    out->w = 0;
    out->h = 0;
    if (!p || len < PNG_SIG_LEN + 25)
        return PNG_ERR_FORMAT;
    if (memcmp(p, png_sig, PNG_SIG_LEN) != 0)
        return PNG_ERR_FORMAT;

    off = PNG_SIG_LEN;
    while (off + 12 <= len) {
        uint32_t clen = png_be32(p + off);
        const uint8_t *type = p + off + 4;
        if (clen > 0x7FFFFFFFu || off + 12 + clen > len)
            break;
        const uint8_t *cd = p + off + 8;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (clen != 13)
                return PNG_ERR_FORMAT;
            w = (int)png_be32(cd);
            h = (int)png_be32(cd + 4);
            depth = cd[8];
            color = cd[9];
            if (cd[10] != 0 || cd[11] != 0)
                return PNG_ERR_UNSUPPORTED;
            interlace = cd[12];
            have_ihdr = 1;
            if (w <= 0 || h <= 0 || w > PNG_MAX_DIM || h > PNG_MAX_DIM)
                return PNG_ERR_UNSUPPORTED;
            if ((uint32_t)w * (uint32_t)h > PNG_MAX_PIXELS)
                return PNG_ERR_UNSUPPORTED;
            if (png_check_ihdr(depth, color) != 0)
                return PNG_ERR_UNSUPPORTED;
            if (interlace != 0)
                return PNG_ERR_UNSUPPORTED;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (clen % 3 != 0 || clen > sizeof(plte))
                return PNG_ERR_FORMAT;
            memcpy(plte, cd, clen);
            have_plte = 1;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (clen > sizeof(trns))
                return PNG_ERR_FORMAT;
            memset(trns, 0xFF, sizeof(trns));
            memcpy(trns, cd, clen);
            have_trns = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!have_ihdr)
                return PNG_ERR_FORMAT;
            idat_total += clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            have_iend = 1;
            break;
        }
        off += 12 + clen;
    }
    if (!have_ihdr || idat_total == 0)
        return PNG_ERR_FORMAT;
    (void)have_iend;
    if (color == 3 && !have_plte)
        return PNG_ERR_FORMAT;

    idat = (uint8_t *)get_kernel_pages(
        (uint32_t)DIV_ROUND_UP(idat_total, PAGE_SIZE));
    if (!idat)
        return PNG_ERR_NOMEM;
    off = PNG_SIG_LEN;
    while (off + 12 <= len && idat_got < idat_total) {
        uint32_t clen = png_be32(p + off);
        const uint8_t *type = p + off + 4;
        if (clen > 0x7FFFFFFFu || off + 12 + clen > len)
            break;
        if (memcmp(type, "IDAT", 4) == 0) {
            uint32_t take = clen;
            if (take > idat_total - idat_got)
                take = idat_total - idat_got;
            memcpy(idat + idat_got, p + off + 8, take);
            idat_got += take;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        off += 12 + clen;
    }

    channels = png_channels(color);
    rowbytes = ((uint32_t)w * (uint32_t)channels * (uint32_t)depth + 7u) / 8u;
    raw_size = (rowbytes + 1u) * (uint32_t)h;
    raw = (uint8_t *)get_kernel_pages(
        (uint32_t)DIV_ROUND_UP(raw_size, PAGE_SIZE));
    if (!raw) {
        free_kernel_page((uint32_t)(uintptr_t)idat);
        return PNG_ERR_NOMEM;
    }
    if (zlib_inflate(idat, idat_got, raw, raw_size, &got) != 0) {
        free_kernel_page((uint32_t)(uintptr_t)raw);
        free_kernel_page((uint32_t)(uintptr_t)idat);
        return PNG_ERR_DATA;
    }
    free_kernel_page((uint32_t)(uintptr_t)idat);
    if (got < raw_size) {
        free_kernel_page((uint32_t)(uintptr_t)raw);
        return PNG_ERR_DATA;
    }

    size_t px_bytes = (size_t)w * (size_t)h * 4u;
    uint32_t *pix = (uint32_t *)get_kernel_pages(
        (uint32_t)DIV_ROUND_UP(px_bytes, PAGE_SIZE));
    if (!pix) {
        free_kernel_page((uint32_t)(uintptr_t)raw);
        return PNG_ERR_NOMEM;
    }

    bpp = (channels * depth + 7) / 8;
    if (bpp < 1)
        bpp = 1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (rowbytes + 1u);
        int filter = row[0];
        uint8_t *data_row = row + 1;
        const uint8_t *prev =
            (y > 0) ? (raw + (size_t)(y - 1) * (rowbytes + 1u) + 1) : 0;
        if (filter > 4) {
            free_kernel_page((uint32_t)(uintptr_t)raw);
            free_kernel_page((uint32_t)(uintptr_t)pix);
            return PNG_ERR_DATA;
        }
        unfilter_row(data_row, prev, (int)rowbytes, bpp, filter);
        row_to_rgba(data_row, w, depth, color, plte,
                    have_trns ? trns : 0, pix + (size_t)y * (size_t)w);
    }
    free_kernel_page((uint32_t)(uintptr_t)raw);

    out->w = w;
    out->h = h;
    out->pixels = pix;
    return PNG_OK;
}

void png_image_free(struct PNG_IMAGE *img) {
    if (!img || !img->pixels)
        return;
    free_kernel_page((uint32_t)(uintptr_t)img->pixels);
    img->pixels = 0;
    img->w = 0;
    img->h = 0;
}
