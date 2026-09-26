/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pagecache.h — a block-device page cache below the filesystems. (GPLv2)
 *
 * Keyed by (device context, byte_offset >> 12) over 4 KiB pages, with an
 * LRU victim list.  Reads are cached; writes are write-through (the page
 * is updated when it is cached, so a later read is served from memory),
 * which keeps the driver honest -- there is no writeback thread yet.
 */
#ifndef GNUCOS_PAGECACHE_H
#define GNUCOS_PAGECACHE_H

#include <stdint.h>

/* Device accessors, exactly the shape ext2's ext2_blkio_t takes. */
typedef int (*pc_dev_read_t)(void *ctx, uint64_t off, void *buf, uint32_t len);
typedef int (*pc_dev_write_t)(void *ctx, uint64_t off, const void *buf,
                              uint32_t len);

void     pagecache_init(void);
int      pagecache_read(void *ctx, pc_dev_read_t rd, uint64_t off,
                        void *buf, uint32_t len);
int      pagecache_write(void *ctx, pc_dev_read_t rd, pc_dev_write_t wr,
                         uint64_t off, const void *buf, uint32_t len);
void     pagecache_invalidate(void *ctx);
void     pagecache_stats(uint32_t *hits, uint32_t *misses, uint32_t *evicts,
                         uint32_t *pages);
void     pagecache_selftest(void);

#endif
