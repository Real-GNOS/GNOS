#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>

#define BITMAP_MASK 0x80

struct MM_BITMAP {
    uint32_t btmp_bytes_len;
    uint8_t *bits;
};

void bitmap_init(struct MM_BITMAP *btmp);
int bitmap_scan_test(const struct MM_BITMAP *btmp, uint32_t bit_idx);
void bitmap_set(struct MM_BITMAP *btmp, uint32_t bit_idx, int8_t value);
int bitmap_scan(const struct MM_BITMAP *btmp, uint32_t cnt);

#endif
