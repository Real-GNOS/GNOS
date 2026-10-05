#ifndef OPS_BLOCK_OPS_H
#define OPS_BLOCK_OPS_H

#include <stdint.h>

struct BLOCK_OPS {
    int (*read_sectors)(void *dev, uint32_t lba, void *buf, uint32_t count);
    int (*write_sectors)(void *dev, uint32_t lba, const void *buf,
                         uint32_t count);
};

extern const struct BLOCK_OPS BLOCK;

#endif /* OPS_BLOCK_OPS_H */
