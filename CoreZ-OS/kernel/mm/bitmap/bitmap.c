#include "kernel/mm/bitmap/bitmap.h"

#include "lib/str/str.h"

void bitmap_init(struct MM_BITMAP *btmp) {
    memset(btmp->bits, 0, btmp->btmp_bytes_len);
}

int bitmap_scan_test(const struct MM_BITMAP *btmp, uint32_t bit_idx) {
    uint32_t byte = bit_idx >> 3;
    if (byte >= btmp->btmp_bytes_len) {
        return -1;
    }
    return (btmp->bits[byte] & (BITMAP_MASK >> (bit_idx & 7))) ? 1 : 0;
}

void bitmap_set(struct MM_BITMAP *btmp, uint32_t bit_idx, int8_t value) {
    uint32_t byte = bit_idx >> 3;
    if (byte >= btmp->btmp_bytes_len) {
        return;
    }
    uint8_t mask = BITMAP_MASK >> (bit_idx & 7);
    if (value) {
        btmp->bits[byte] |= mask;
    } else {
        btmp->bits[byte] &= ~mask;
    }
}

static inline uint64_t bitmap_word(const uint64_t *words, uint32_t w) {
    uint64_t x = words[w];
    x = (x & 0x5555555555555555ull) << 1 | ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) << 2 | ((x >> 2) & 0x3333333333333333ull);
    return (x & 0x0f0f0f0f0f0f0f0full) << 4 | ((x >> 4) & 0x0f0f0f0f0f0f0f0full);
}

int bitmap_scan(const struct MM_BITMAP *btmp, uint32_t cnt) {
    if (cnt == 0 || btmp->btmp_bytes_len == 0) {
        return -1;
    }
    const uint64_t *words = (const uint64_t *)btmp->bits;
    uint32_t nwords = btmp->btmp_bytes_len >> 3;
    uint32_t run = 0;
    for (uint32_t w = 0; w < nwords; w++) {
        uint64_t v = bitmap_word(words, w);
        if (v == 0) {
            run += 64;
            if (run >= cnt) {
                return (int)((w << 6) + 64 - run);
            }
            continue;
        }
        if (v == ~0ull) {
            run = 0;
            continue;
        }
        uint32_t base = w << 6;
        uint32_t pos = 0;
        while (pos < 64) {
            uint64_t rest = v >> pos;
            if (rest == 0) {
                run += 64 - pos;
                break;
            }
            uint32_t tz = __builtin_ctzll(rest);
            if (run + tz >= cnt) {
                return (int)(base + pos - run);
            }
            run = 0;
            pos += tz;
            rest >>= tz;
            if (rest == ~0ull) {
                break;
            }
            pos += __builtin_ctzll(~rest);
        }
    }
    for (uint32_t byte = nwords << 3; byte < btmp->btmp_bytes_len; byte++) {
        uint8_t bits = btmp->bits[byte];
        for (uint32_t j = 0; j < 8; j++) {
            if (bits & 0x80u) {
                run = 0;
            } else if (++run >= cnt) {
                return (int)((byte << 3) + j - cnt + 1);
            }
            bits <<= 1;
        }
    }
    return -1;
}
