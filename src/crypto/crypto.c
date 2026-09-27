/* SPDX-License-Identifier: GPL-2.0 */
/*
 * crypto.c — SHA-256, CRC-32 and AES-128, with boot-time known-answer
 * tests. (GPLv2)
 *
 * The implementations follow the specifications directly (FIPS 180-4,
 * FIPS 197, IEEE 802.3) rather than any optimised variant: clarity and
 * auditability first, and the kernel's crypto volume does not justify
 * hand-scheduled assembly yet.
 */
#include <stdint.h>
#include <stddef.h>

#include "crypto.h"
#include "kstring.h"
#include "debugcon.h"

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_ctx_t *ctx)
{
    static const uint32_t H0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->h, H0, sizeof H0);
    ctx->total = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    ctx->total += len;
    while (len) {
        uint32_t take = 64 - ctx->buflen;
        if (take > len)
            take = (uint32_t)len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha256_block(ctx->h, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->total * 8;
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->buflen != 56)
        sha256_update(ctx, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    memcpy(ctx->buf + 56, lenb, 8);
    sha256_block(ctx->h, ctx->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void crypto_sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ---- CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -------------------- */

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

uint32_t crypto_crc32(const void *data, size_t len)
{
    if (!crc_table_ready)
        crc_init_table();
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- AES-128 (FIPS 197) ------------------------------------------------- */

static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t gmul(uint8_t a, uint8_t b)
{
    /* GF(2^8) multiply, poly 0x11B — double-and-add, no lookup tables. */
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return r;
}

void crypto_aes128_expand_key(const uint8_t key[16], uint32_t roundkeys[44])
{
    static const uint8_t RCON[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    for (int i = 0; i < 4; i++)
        roundkeys[i] = ((uint32_t)key[i * 4] << 24) | ((uint32_t)key[i * 4 + 1] << 16) |
                       ((uint32_t)key[i * 4 + 2] << 8) | (uint32_t)key[i * 4 + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = roundkeys[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);                     /* RotWord */
            t = ((uint32_t)SBOX[(t >> 24) & 0xFF] << 24) |
                ((uint32_t)SBOX[(t >> 16) & 0xFF] << 16) |
                ((uint32_t)SBOX[(t >> 8) & 0xFF] << 8) |
                (uint32_t)SBOX[t & 0xFF];                 /* SubWord */
            t ^= (uint32_t)RCON[i / 4 - 1] << 24;
        }
        roundkeys[i] = roundkeys[i - 4] ^ t;
    }
}

static void aes_add_round_key(uint8_t st[16], const uint32_t rk[4])
{
    for (int i = 0; i < 4; i++) {
        st[i * 4]     ^= (uint8_t)(rk[i] >> 24);
        st[i * 4 + 1] ^= (uint8_t)(rk[i] >> 16);
        st[i * 4 + 2] ^= (uint8_t)(rk[i] >> 8);
        st[i * 4 + 3] ^= (uint8_t)(rk[i]);
    }
}

/* The inverse S-box, generated at init from the forward one (the inverse of
 * a permutation is its transpose — 256 bytes, once). */
static uint8_t INV_SBOX[256];
static int aes_tables_ready;

static void aes_init_tables(void)
{
    for (int i = 0; i < 256; i++)
        INV_SBOX[SBOX[i]] = (uint8_t)i;
    aes_tables_ready = 1;
}

static void aes_mix_columns(uint8_t st[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = st[c * 4], a1 = st[c * 4 + 1];
        uint8_t a2 = st[c * 4 + 2], a3 = st[c * 4 + 3];
        st[c * 4]     = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
        st[c * 4 + 1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
        st[c * 4 + 2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
        st[c * 4 + 3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
    }
}

static void aes_inv_mix_columns(uint8_t st[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = st[c * 4], a1 = st[c * 4 + 1];
        uint8_t a2 = st[c * 4 + 2], a3 = st[c * 4 + 3];
        st[c * 4]     = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
        st[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
        st[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
        st[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
}

void crypto_aes128_encrypt_block(const uint32_t roundkeys[44],
                                 const uint8_t in[16], uint8_t out[16])
{
    uint8_t st[16];
    memcpy(st, in, 16);
    aes_add_round_key(st, roundkeys);
    for (int round = 1; round < 10; round++) {
        for (int i = 0; i < 16; i++)
            st[i] = SBOX[st[i]];
        /* ShiftRows */
        uint8_t t[16];
        memcpy(t, st, 16);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                st[c * 4 + r] = t[((c + r) & 3) * 4 + r];
        aes_mix_columns(st);
        aes_add_round_key(st, roundkeys + round * 4);
    }
    for (int i = 0; i < 16; i++)
        st[i] = SBOX[st[i]];
    uint8_t t[16];
    memcpy(t, st, 16);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            st[c * 4 + r] = t[((c + r) & 3) * 4 + r];
    aes_add_round_key(st, roundkeys + 40);
    memcpy(out, st, 16);
}

void crypto_aes128_decrypt_block(const uint32_t roundkeys[44],
                                 const uint8_t in[16], uint8_t out[16])
{
    if (!aes_tables_ready)
        aes_init_tables();
    uint8_t st[16];
    memcpy(st, in, 16);
    aes_add_round_key(st, roundkeys + 40);
    for (int round = 9; round >= 1; round--) {
        /* InvShiftRows */
        uint8_t t[16];
        memcpy(t, st, 16);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                st[c * 4 + r] = t[((c - r + 4) & 3) * 4 + r];
        for (int i = 0; i < 16; i++)
            st[i] = INV_SBOX[st[i]];
        aes_add_round_key(st, roundkeys + round * 4);
        aes_inv_mix_columns(st);
    }
    uint8_t t[16];
    memcpy(t, st, 16);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            st[c * 4 + r] = t[((c - r + 4) & 3) * 4 + r];
    for (int i = 0; i < 16; i++)
        st[i] = INV_SBOX[st[i]];
    aes_add_round_key(st, roundkeys);
    memcpy(out, st, 16);
}

/* ---- known-answer tests (KAT) ------------------------------------------- */

static int hex_eq(const uint8_t *got, const char *want, int n)
{
    /* compare `n` bytes against a lowercase hex string, char by char */
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        char hi = hx[got[i] >> 4], lo = hx[got[i] & 0xF];
        if (hi != want[i * 2] || lo != want[i * 2 + 1])
            return 0;
    }
    return 1;
}

void crypto_selftest(void)
{
    int ok = 1;

    /* CRC-32("123456789") = 0xCBF43926 — the standard check value. */
    if (crypto_crc32("123456789", 9) != 0xCBF43926u)
        ok = 0;

    /* SHA-256("abc"):
     * ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    uint8_t h[32];
    crypto_sha256("abc", 3, h);
    if (!hex_eq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32))
        ok = 0;
    /* SHA-256("") = e3b0c442... */
    crypto_sha256("", 0, h);
    if (!hex_eq(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32))
        ok = 0;
    /* streaming form must agree with the one-shot form on a long input */
    {
        sha256_ctx_t ctx;
        uint8_t big[1000];
        memset(big, 0x5A, sizeof big);
        sha256_init(&ctx);
        for (size_t i = 0; i < sizeof big; i += 137)
            sha256_update(&ctx, big + i,
                          (sizeof big - i < 137) ? sizeof big - i : 137);
        sha256_final(&ctx, h);
        uint8_t h2[32];
        crypto_sha256(big, sizeof big, h2);
        if (memcmp(h, h2, 32))
            ok = 0;
    }

    /* AES-128 FIPS 197 appendix C.1:
     * key 000102030405060708090a0b0c0d0e0f
     * pt  00112233445566778899aabbccddeeff
     * ct  69c4e0d86a7b0430d8cdb78070b4c55a */
    {
        uint8_t key[16], pt[16], ct[16], back[16];
        for (int i = 0; i < 16; i++) {
            key[i] = (uint8_t)i;
            pt[i] = (uint8_t)(i * 0x11);
        }
        uint32_t rk[44];
        crypto_aes128_expand_key(key, rk);
        crypto_aes128_encrypt_block(rk, pt, ct);
        static const char want_ct[] = "69c4e0d86a7b0430d8cdb78070b4c55a";
        uint8_t want[16];
        for (int i = 0; i < 16; i++) {
            static const char hx[] = "0123456789abcdef";
            char byte[3] = { want_ct[i * 2], want_ct[i * 2 + 1], 0 };
            (void)byte;
            want[i] = (uint8_t)((strchr(want_ct + i * 2, want_ct[i * 2]) ? 0 : 0));
        }
        /* decode without strtol: nibble math on the hex string */
        for (int i = 0; i < 16; i++) {
            uint8_t hi = 0, lo = 0;
            for (int k = 0; k < 16; k++) {
                if ("0123456789abcdef"[k] == want_ct[i * 2]) hi = (uint8_t)k;
                if ("0123456789abcdef"[k] == want_ct[i * 2 + 1]) lo = (uint8_t)k;
            }
            want[i] = (uint8_t)((hi << 4) | lo);
        }
        if (memcmp(ct, want, 16))
            ok = 0;
        crypto_aes128_decrypt_block(rk, ct, back);
        if (memcmp(back, pt, 16))
            ok = 0;
    }

    dbg_puts("CRYPTO: self-test ");
    dbg_puts(ok ? "PASS (sha256/crc32/aes128)\r\n" : "FAIL\r\n");
}
