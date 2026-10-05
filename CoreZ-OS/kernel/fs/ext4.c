#include "kernel/fs/ext4.h"
#include "kernel/fs/fs.h"
#include "kernel/sched/thread.h"

#include "ops/block_ops.h"
#include "kernel/sched/sync.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/dir.h"
#include "drivers/char/rtc.h"

static struct SCHED_RWLOCK ext4_lock;

static int ext4_read_inode_impl(uint32_t ino, struct FS_INODE *out);
static int ext4_write_inode_impl(uint32_t ino, const struct FS_INODE *in);
static int ext4_write_to_inode_impl(struct FS_INODE *ino, uint32_t off,
                                    const void *buf, uint32_t count);
static void ext4_truncate_inode_impl(struct FS_INODE *ino);
static uint32_t ext4_new_inode_impl(uint32_t mode, struct FS_INODE *out);
static int ext4_add_entry_impl(struct FS_INODE *dino, uint32_t ino,
                               const char *name, uint8_t dtype);
static int ext4_remove_entry_impl(struct FS_INODE *dino, const char *name);
static int ext4_read_from_inode_impl(const struct FS_INODE *ino, uint32_t off,
                                     void *buf, uint32_t count);
static int ext4_dir_next_impl(const struct FS_INODE *dino, uint32_t *pos,
                              struct FS_DIRENT *out);
static int ext4_lookup_depth(const char *path, uint32_t *ino, int *ftype,
                             int follow, int depth);
static uint32_t ext4_alloc_block(void);
static void ext4_free_block(uint32_t blk);
static void ext4_free_inode_impl(uint32_t ino);

static struct DISK *disk = NULL;
static struct DISK_PARTITION *part = NULL;
static uint32_t start = 0;
static uint32_t bs = 1024;
static uint32_t sect_per_block = 2;
static uint32_t inodes_per_group = 0;
static uint32_t blocks_per_group = 0;
static uint32_t first_data_block = 0;
static uint32_t total_blocks = 0;
static uint32_t n_groups = 0;
static uint32_t desc_size = 32;
static uint32_t inode_size = 256;
static uint32_t gdt_blk = 0;
static uint32_t gdt_blocks = 0;
static uint32_t free_blocks = 0;
static uint32_t free_inodes = 0;
static uint32_t csum_seed = 0;
static int has_csum = 0;
static int has_64bit = 0;
static int has_journal = 0;

static uint8_t uuid[16];

#define EXT4_GDT_MAX 64u
#define EXT4_GDT_STRIDE 64u
static uint8_t g_gdt[EXT4_GDT_MAX * EXT4_GDT_STRIDE];
static uint8_t g_sb[1024];

static uint32_t j_maxlen = 0;
static uint32_t j_first = 1;
static uint32_t j_seq = 1;
static uint8_t g_jsb[1024];
static uint32_t j_map_log[2];
static uint32_t j_map_phys[2];
static uint32_t j_nmap = 0;

#define EXT4_BCACHE_SLOTS 1024u

static uint8_t *s_bc_data;
static uint32_t s_bc_tag[EXT4_BCACHE_SLOTS];
static uint8_t s_bc_valid[EXT4_BCACHE_SLOTS];
static uint32_t s_bc_slots;

static uint32_t crc_table[256];
static int crc_ready = 0;

static void ext4_crc_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (uint32_t k = 0; k < 8u; k++) {
            c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32c(uint32_t crc, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

static uint16_t ld16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void st16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int ext4_disk_read(uint32_t blk, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    if (blk >= total_blocks && total_blocks != 0) {
        kprintf("[ext4] read out-of-range block %d\n", blk);
        return -1;
    }
    BLOCK.read_sectors(disk, start + blk * sect_per_block, buf,
                       sect_per_block);
    return 0;
}

static const uint8_t *ext4_bcache_peek(uint32_t blk) {
    if (s_bc_data == NULL) {
        return NULL;
    }
    uint32_t slot = blk % s_bc_slots;
    if (s_bc_valid[slot] && s_bc_tag[slot] == blk) {
        return s_bc_data + slot * bs;
    }
    return NULL;
}

static void ext4_bcache_inval(uint32_t blk) {
    if (s_bc_data == NULL) {
        return;
    }
    uint32_t slot = blk % s_bc_slots;
    if (s_bc_valid[slot] && s_bc_tag[slot] == blk) {
        s_bc_valid[slot] = 0;
    }
}

static const uint8_t *ext4_bcache_get(uint32_t blk) {
    const uint8_t *hit = ext4_bcache_peek(blk);
    if (hit != NULL || s_bc_data == NULL) {
        return hit;
    }
    uint32_t slot = blk % s_bc_slots;
    uint8_t *dst = s_bc_data + slot * bs;
    if (ext4_disk_read(blk, dst) != 0) {
        return NULL;
    }
    s_bc_tag[slot] = blk;
    s_bc_valid[slot] = 1;
    return dst;
}

static int ext4_read_block(uint32_t blk, void *buf) {
    const uint8_t *c = ext4_bcache_get(blk);
    if (c != NULL) {
        memcpy(buf, c, bs);
        return 0;
    }
    return ext4_disk_read(blk, buf);
}

static int ext4_raw_write(uint32_t blk, const void *buf) {
    if (disk == NULL) {
        return -1;
    }
    BLOCK.write_sectors(disk, start + blk * sect_per_block, (void *)buf,
                        sect_per_block);
    ext4_bcache_inval(blk);
    return 0;
}

static int ext4_read_blocks(uint32_t blk, uint32_t cnt, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    if (blk >= total_blocks && total_blocks != 0) {
        return -1;
    }
    if (cnt == 0) {
        return 0;
    }
    if (blk + cnt > total_blocks && total_blocks != 0) {
        cnt = total_blocks - blk;
    }
    BLOCK.read_sectors(disk, start + blk * sect_per_block, buf,
                       cnt * sect_per_block);
    return 0;
}

static int jsb_read(uint32_t blk, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    BLOCK.read_sectors(disk, start + blk * sect_per_block, buf, 2);
    return 0;
}

static int jsb_write(uint32_t blk, const void *buf) {
    if (disk == NULL) {
        return -1;
    }
    BLOCK.write_sectors(disk, start + blk * sect_per_block, (void *)buf, 2);
    return 0;
}

#define EXT4_TXN_MAX 8u
static uint32_t t_blk[EXT4_TXN_MAX];
static uint8_t *t_buf[EXT4_TXN_MAX];
static uint32_t t_cnt = 0;

static void txn_reset(void) {
    t_cnt = 0;
}

static void txn_drop(void) {
    for (uint32_t i = 0; i < t_cnt; i++) {
        if (t_buf[i] != NULL) {
            free_kernel_page((uint32_t)t_buf[i]);
            t_buf[i] = NULL;
        }
    }
    t_cnt = 0;
}

static int txn_add(uint32_t blk, const void *buf) {
    if (t_cnt >= EXT4_TXN_MAX) {
        return -1;
    }
    uint8_t *d = (uint8_t *)get_kernel_pages(1);
    if (d == NULL) {
        return -1;
    }
    memset(d, 0, 4096);
    memcpy(d, buf, bs);
    t_blk[t_cnt] = blk;
    t_buf[t_cnt] = d;
    t_cnt++;
    return 0;
}

static uint32_t jrnl_phys(uint32_t jblk);

static int ext4_jsb_write(uint32_t s_start, uint32_t s_seq) {
    wr_be32(g_jsb + 0x1C, s_start);
    wr_be32(g_jsb + 0x18, s_seq);
    uint32_t pblk = jrnl_phys(0);
    if (pblk == 0) {
        return -1;
    }
    return jsb_write(pblk, g_jsb);
}

static int txn_commit(void) {
    if (t_cnt == 0) {
        return 0;
    }
    if (!has_journal) {
        int rc = 0;
        for (uint32_t i = 0; i < t_cnt; i++) {
            if (ext4_raw_write(t_blk[i], t_buf[i])) {
                rc = -1;
            }
        }
        txn_drop();
        return rc;
    }
    uint8_t *jb = (uint8_t *)get_kernel_pages(1);
    if (jb == NULL) {
        txn_drop();
        return -1;
    }
    memset(jb, 0, 4096);
    uint8_t *db = (uint8_t *)get_kernel_pages(1);
    if (db == NULL) {
        free_kernel_page((uint32_t)jb);
        txn_drop();
        return -1;
    }
    uint32_t seq = j_seq;
    wr_be32(jb + 0, EXT4_JBD2_MAGIC);
    wr_be32(jb + 4, EXT4_JT_DESCRIPTOR);
    wr_be32(jb + 8, seq);
    uint32_t off = 12;
    for (uint32_t i = 0; i < t_cnt; i++) {
        uint16_t flags = (uint16_t)EXT4_JF_SAME_UUID;
        if (i + 1 == t_cnt) {
            flags |= (uint16_t)EXT4_JF_LAST_TAG;
        }
        if (ld32(t_buf[i]) == EXT4_JBD2_MAGIC) {
            flags |= (uint16_t)EXT4_JF_ESCAPE;
        }
        uint8_t *tag = jb + off;
        wr_be32(tag, t_blk[i]);
        tag[4] = 0;
        tag[5] = 0;
        tag[6] = (uint8_t)(flags >> 8);
        tag[7] = (uint8_t)(flags & 0xFFu);
        off += 8;
    }
    uint32_t log = j_first;
    if (ext4_raw_write(jrnl_phys(log), jb) != 0) {
        free_kernel_page((uint32_t)jb);
        free_kernel_page((uint32_t)db);
        txn_drop();
        return -1;
    }
    for (uint32_t i = 0; i < t_cnt; i++) {
        memcpy(db, t_buf[i], bs);
        if (ld32(db) == EXT4_JBD2_MAGIC) {
            st32(db, 0);
        }
        if (ext4_raw_write(jrnl_phys(log + 1 + i), db) != 0) {
            free_kernel_page((uint32_t)jb);
            free_kernel_page((uint32_t)db);
            txn_drop();
            return -1;
        }
    }
    memset(jb, 0, 4096);
    wr_be32(jb + 0, EXT4_JBD2_MAGIC);
    wr_be32(jb + 4, EXT4_JT_COMMIT);
    wr_be32(jb + 8, seq);
    if (ext4_raw_write(jrnl_phys(log + 1 + t_cnt), jb) != 0) {
        free_kernel_page((uint32_t)jb);
        free_kernel_page((uint32_t)db);
        txn_drop();
        return -1;
    }
    free_kernel_page((uint32_t)jb);
    free_kernel_page((uint32_t)db);
    ext4_jsb_write(log, seq);
    for (uint32_t i = 0; i < t_cnt; i++) {
        ext4_raw_write(t_blk[i], t_buf[i]);
    }
    ext4_jsb_write(0, seq + 1);
    j_seq = seq + 1;
    txn_drop();
    return 0;
}

static int ext4_store_block(uint32_t blk, const void *buf) {
    txn_reset();
    if (txn_add(blk, buf) != 0) {
        txn_drop();
        return -1;
    }
    return txn_commit();
}

static int ext4_store_data(uint32_t blk, const void *buf) {
    return ext4_raw_write(blk, buf);
}

static uint8_t *gdt_desc(uint32_t g) {
    return g_gdt + g * desc_size;
}

static uint32_t gd_get32(uint32_t g, uint32_t lo_off, uint32_t hi_off) {
    uint8_t *d = gdt_desc(g);
    uint32_t v = ld32(d + lo_off);
    if (desc_size > 32u) {
        v |= ld32(d + hi_off);
    }
    return v;
}

static uint32_t gd_get16(uint32_t g, uint32_t lo_off, uint32_t hi_off) {
    uint8_t *d = gdt_desc(g);
    uint32_t v = ld16(d + lo_off);
    if (desc_size > 32u) {
        v |= ((uint32_t)ld16(d + hi_off) << 16);
    }
    return v;
}

static void gd_set16(uint32_t g, uint32_t lo_off, uint32_t hi_off,
                     uint32_t v) {
    uint8_t *d = gdt_desc(g);
    st16(d + lo_off, (uint16_t)(v & 0xFFFFu));
    if (desc_size > 32u) {
        st16(d + hi_off, (uint16_t)(v >> 16));
    }
}

static uint32_t gd_block_bitmap(uint32_t g) {
    return gd_get32(g, 0x00, 0x20);
}

static uint32_t gd_inode_bitmap(uint32_t g) {
    return gd_get32(g, 0x04, 0x24);
}

static uint32_t gd_inode_table(uint32_t g) {
    return gd_get32(g, 0x08, 0x28);
}

static uint32_t gd_free_blocks(uint32_t g) {
    return gd_get16(g, 0x0C, 0x2C);
}

static uint32_t gd_free_inodes(uint32_t g) {
    return gd_get16(g, 0x0E, 0x2E);
}

static uint32_t gd_used_dirs(uint32_t g) {
    return gd_get16(g, 0x10, 0x30);
}

static uint32_t gd_flags(uint32_t g) {
    return ld16(gdt_desc(g) + 0x12);
}

static uint32_t gdt_block_of(uint32_t g) {
    return gdt_blk + (g * desc_size) / bs;
}

static uint32_t group_first_block(uint32_t g) {
    return first_data_block + g * blocks_per_group;
}

static uint32_t group_block_count(uint32_t g) {
    uint32_t first = group_first_block(g);
    if (first >= total_blocks) {
        return 0;
    }
    uint32_t n = total_blocks - first;
    return n > blocks_per_group ? blocks_per_group : n;
}

static void gdt_csum_set(uint32_t g) {
    if (!has_csum) {
        return;
    }
    uint8_t *d = gdt_desc(g);
    uint32_t c = crc32c(csum_seed, "\0\0\0\0", 0);
    uint8_t gb[4];
    st32(gb, g);
    c = crc32c(csum_seed, gb, 4);
    c = crc32c(c, d, 0x1E);
    uint8_t z[2];
    z[0] = 0;
    z[1] = 0;
    c = crc32c(c, z, 2);
    c = crc32c(c, d + 0x20, desc_size - 0x20);
    st16(d + 0x1E, (uint16_t)(c & 0xFFFFu));
}

static int gdt_flush(uint32_t g) {
    gdt_csum_set(g);
    uint32_t blk = gdt_block_of(g);
    uint32_t off = blk - gdt_blk;
    return ext4_store_block(blk, g_gdt + off * bs);
}

static void bitmap_csum_set(uint32_t g, const uint8_t *bm, int inode_bm) {
    if (!has_csum) {
        return;
    }
    if (inode_bm) {
        if (gd_flags(g) & EXT4_BG_INODE_UNINIT) {
            st16(gdt_desc(g) + 0x1A, 0);
            if (desc_size > 32u) {
                st16(gdt_desc(g) + 0x3A, 0);
            }
            return;
        }
        uint32_t len = (inodes_per_group + 7u) / 8u;
        uint32_t c = crc32c(csum_seed, bm, len);
        st16(gdt_desc(g) + 0x1A, (uint16_t)(c & 0xFFFFu));
        if (desc_size > 32u) {
            st16(gdt_desc(g) + 0x3A, (uint16_t)(c >> 16));
        }
        return;
    }
    if (gd_flags(g) & EXT4_BG_BLOCK_UNINIT) {
        st16(gdt_desc(g) + 0x18, 0);
        if (desc_size > 32u) {
            st16(gdt_desc(g) + 0x38, 0);
        }
        return;
    }
    uint32_t len = blocks_per_group / 8u;
    uint32_t c = crc32c(csum_seed, bm, len);
    st16(gdt_desc(g) + 0x18, (uint16_t)(c & 0xFFFFu));
    if (desc_size > 32u) {
        st16(gdt_desc(g) + 0x38, (uint16_t)(c >> 16));
    }
}

static uint32_t sb_off_in_block(void) {
    return 1024u - first_data_block * bs;
}

static void ext4_sb_fill(uint8_t *dst) {
    memset(dst, 0, bs);
    ext4_read_block(first_data_block, dst);
    memcpy(dst + sb_off_in_block(), g_sb, 1024);
}

static void ext4_sb_sync(void) {
    st32(g_sb + 0x0C, free_blocks);
    st32(g_sb + 0x158, 0);
    st32(g_sb + 0x10, free_inodes);
    st32(g_sb + 0x04, total_blocks);
    st32(g_sb + 0x150, 0);
    st32(g_sb + 0x44, (uint32_t)rtc_unix_time());
    if (has_csum) {
        st32(g_sb + 0x3FC, crc32c(0xFFFFFFFFu, g_sb, 0x3FC));
    }
}

static int ext4_group_alloc_block(uint32_t g) {
    uint32_t bm_blk = gd_block_bitmap(g);
    uint8_t *bm = (uint8_t *)get_kernel_pages(1);
    if (bm == NULL) {
        return 0;
    }
    memset(bm, 0, 4096);
    if (ext4_read_block(bm_blk, bm) != 0) {
        free_kernel_page((uint32_t)bm);
        return 0;
    }
    uint32_t cnt = group_block_count(g);
    uint32_t found = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < cnt; i++) {
        if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7)))) {
            bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            found = i;
            break;
        }
    }
    if (found == 0xFFFFFFFFu) {
        free_kernel_page((uint32_t)bm);
        return 0;
    }
    bitmap_csum_set(g, bm, 0);
    gd_set16(g, 0x0C, 0x2C, gd_free_blocks(g) - 1);
    gdt_csum_set(g);
    free_blocks--;
    txn_reset();
    int rc = txn_add(bm_blk, bm) == 0 && txn_add(gdt_block_of(g), g_gdt +
                      (gdt_block_of(g) - gdt_blk) * bs) == 0;
    if (rc) {
        uint8_t *sb = (uint8_t *)get_kernel_pages(1);
        if (sb != NULL) {
            ext4_sb_sync();
            ext4_sb_fill(sb);
            if (txn_add(first_data_block, sb) != 0) {
                rc = 0;
            }
            free_kernel_page((uint32_t)sb);
        }
    }
    free_kernel_page((uint32_t)bm);
    if (!rc) {
        txn_drop();
        return 0;
    }
    if (txn_commit() != 0) {
        return 0;
    }
    return group_first_block(g) + found;
}

static uint32_t ext4_alloc_block(void) {
    if (disk == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < n_groups; g++) {
        if (gd_flags(g) & EXT4_BG_BLOCK_UNINIT) {
            continue;
        }
        if (gd_free_blocks(g) == 0) {
            continue;
        }
        uint32_t b = ext4_group_alloc_block(g);
        if (b != 0) {
            return b;
        }
    }
    return 0;
}

static void ext4_free_block(uint32_t blk) {
    if (blk == 0 || blk >= total_blocks || blk < first_data_block) {
        return;
    }
    uint32_t g = (blk - first_data_block) / blocks_per_group;
    if (g >= n_groups) {
        return;
    }
    uint32_t bit = blk - group_first_block(g);
    uint32_t bm_blk = gd_block_bitmap(g);
    uint8_t *bm = (uint8_t *)get_kernel_pages(1);
    if (bm == NULL) {
        return;
    }
    memset(bm, 0, 4096);
    if (ext4_read_block(bm_blk, bm) != 0) {
        free_kernel_page((uint32_t)bm);
        return;
    }
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    if (!(bm[bit >> 3] & mask)) {
        free_kernel_page((uint32_t)bm);
        return;
    }
    bm[bit >> 3] &= (uint8_t)~mask;
    bitmap_csum_set(g, bm, 0);
    gd_set16(g, 0x0C, 0x2C, gd_free_blocks(g) + 1);
    gdt_csum_set(g);
    free_blocks++;
    txn_reset();
    int rc = txn_add(bm_blk, bm) == 0 &&
             txn_add(gdt_block_of(g),
                     g_gdt + (gdt_block_of(g) - gdt_blk) * bs) == 0;
    if (rc) {
        uint8_t *sb = (uint8_t *)get_kernel_pages(1);
        if (sb != NULL) {
            ext4_sb_sync();
            ext4_sb_fill(sb);
            if (txn_add(first_data_block, sb) != 0) {
                rc = 0;
            }
            free_kernel_page((uint32_t)sb);
        }
    }
    free_kernel_page((uint32_t)bm);
    if (!rc) {
        txn_drop();
        return;
    }
    txn_commit();
}

static uint32_t itable_unused_of(const uint8_t *bm) {
    for (uint32_t i = inodes_per_group; i > 0; i--) {
        if (bm[(i - 1) >> 3] & (uint8_t)(1u << ((i - 1) & 7))) {
            return inodes_per_group - i;
        }
    }
    return inodes_per_group;
}

static uint32_t ext4_group_alloc_inode(uint32_t g) {
    uint32_t bm_blk = gd_inode_bitmap(g);
    uint8_t *bm = (uint8_t *)get_kernel_pages(1);
    if (bm == NULL) {
        return 0;
    }
    memset(bm, 0, 4096);
    if (ext4_read_block(bm_blk, bm) != 0) {
        free_kernel_page((uint32_t)bm);
        return 0;
    }
    uint32_t found = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < inodes_per_group; i++) {
        if (!(bm[i >> 3] & (uint8_t)(1u << (i & 7)))) {
            bm[i >> 3] |= (uint8_t)(1u << (i & 7));
            found = i;
            break;
        }
    }
    if (found == 0xFFFFFFFFu) {
        free_kernel_page((uint32_t)bm);
        return 0;
    }
    bitmap_csum_set(g, bm, 1);
    gd_set16(g, 0x0E, 0x2E, gd_free_inodes(g) - 1);
    gd_set16(g, 0x1C, 0x32, itable_unused_of(bm));
    gdt_csum_set(g);
    free_inodes--;
    txn_reset();
    int rc = txn_add(bm_blk, bm) == 0 &&
             txn_add(gdt_block_of(g),
                     g_gdt + (gdt_block_of(g) - gdt_blk) * bs) == 0;
    if (rc) {
        uint8_t *sb = (uint8_t *)get_kernel_pages(1);
        if (sb != NULL) {
            ext4_sb_sync();
            ext4_sb_fill(sb);
            if (txn_add(first_data_block, sb) != 0) {
                rc = 0;
            }
            free_kernel_page((uint32_t)sb);
        }
    }
    free_kernel_page((uint32_t)bm);
    if (!rc) {
        txn_drop();
        return 0;
    }
    if (txn_commit() != 0) {
        return 0;
    }
    return g * inodes_per_group + found + 1;
}

static uint32_t ext4_alloc_inode(void) {
    if (disk == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < n_groups; g++) {
        if (gd_flags(g) & EXT4_BG_INODE_UNINIT) {
            continue;
        }
        if (gd_free_inodes(g) == 0) {
            continue;
        }
        uint32_t ino = ext4_group_alloc_inode(g);
        if (ino != 0) {
            return ino;
        }
    }
    return 0;
}

static void ext4_free_inode_impl(uint32_t ino) {
    if (disk == NULL || ino == 0) {
        return;
    }
    uint32_t g = (ino - 1) / inodes_per_group;
    if (g >= n_groups) {
        return;
    }
    uint32_t bit = (ino - 1) % inodes_per_group;
    struct FS_INODE node;
    int is_dir = 0;
    if (ext4_read_inode_impl(ino, &node) == 0) {
        is_dir = (node.i_mode & 0xF000u) == 0x4000u;
        node.i_links_count = 0;
        node.i_dtime = (uint32_t)rtc_unix_time();
        ext4_write_inode_impl(ino, &node);
    }
    uint32_t bm_blk = gd_inode_bitmap(g);
    uint8_t *bm = (uint8_t *)get_kernel_pages(1);
    if (bm == NULL) {
        return;
    }
    memset(bm, 0, 4096);
    if (ext4_read_block(bm_blk, bm) != 0) {
        free_kernel_page((uint32_t)bm);
        return;
    }
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    if (!(bm[bit >> 3] & mask)) {
        free_kernel_page((uint32_t)bm);
        return;
    }
    bm[bit >> 3] &= (uint8_t)~mask;
    bitmap_csum_set(g, bm, 1);
    gd_set16(g, 0x0E, 0x2E, gd_free_inodes(g) + 1);
    gd_set16(g, 0x1C, 0x32, itable_unused_of(bm));
    if (is_dir && gd_used_dirs(g) > 0) {
        gd_set16(g, 0x10, 0x30, gd_used_dirs(g) - 1);
    }
    gdt_csum_set(g);
    free_inodes++;
    txn_reset();
    int rc = txn_add(bm_blk, bm) == 0 &&
             txn_add(gdt_block_of(g),
                     g_gdt + (gdt_block_of(g) - gdt_blk) * bs) == 0;
    if (rc) {
        uint8_t *sb = (uint8_t *)get_kernel_pages(1);
        if (sb != NULL) {
            ext4_sb_sync();
            ext4_sb_fill(sb);
            if (txn_add(first_data_block, sb) != 0) {
                rc = 0;
            }
            free_kernel_page((uint32_t)sb);
        }
    }
    free_kernel_page((uint32_t)bm);
    if (!rc) {
        txn_drop();
        return;
    }
    txn_commit();
}

static struct EXT4_EXTENT_HEADER *ext_hdr(uint8_t *p) {
    return (struct EXT4_EXTENT_HEADER *)p;
}

static struct EXT4_EXTENT *ext_ent(uint8_t *p, uint32_t i) {
    return (struct EXT4_EXTENT *)(p + 12 + i * 12);
}

static struct EXT4_EXTENT_IDX *ext_idx(uint8_t *p, uint32_t i) {
    return (struct EXT4_EXTENT_IDX *)(p + 12 + i * 12);
}

static uint32_t node_max(int is_root) {
    if (is_root) {
        return EXT4_EXTENT_ROOT_MAX;
    }
    return (bs - 12u - 4u) / 12u;
}

static uint32_t node_tail_off(uint32_t max) {
    return 12u + max * 12u;
}

static void ext_node_csum(const struct FS_INODE *ino, uint8_t *p, uint32_t max) {
    if (!has_csum) {
        return;
    }
    uint32_t t = node_tail_off(max);
    if (t + 4u > bs) {
        return;
    }
    uint8_t gb[4];
    st32(gb, ino->i_generation);
    uint8_t ib[4];
    st32(ib, ino->i_no);
    uint32_t c = crc32c(csum_seed, ib, 4);
    c = crc32c(c, gb, 4);
    c = crc32c(c, p, t);
    st32(p + t, c);
}

static void ext_leaf_init(struct FS_INODE *ino) {
    uint8_t *p = (uint8_t *)ino->i_block;
    struct EXT4_EXTENT_HEADER *h = ext_hdr(p);
    h->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    h->eh_entries = 0;
    h->eh_max = (uint16_t)EXT4_EXTENT_ROOT_MAX;
    h->eh_depth = 0;
    h->eh_generation = 0;
    ino->i_flags |= EXT4_EXTENTS_FL;
}

static int ext_lookup(const struct FS_INODE *ino, uint32_t fblk,
                      uint32_t *out) {
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }
    memcpy(blk, ino->i_block, 60);
    struct EXT4_EXTENT_HEADER *h = ext_hdr(blk);
    if (h->eh_magic != EXT4_EXTENT_MAGIC || h->eh_entries == 0) {
        free_kernel_page((uint32_t)blk);
        return -1;
    }
    for (;;) {
        if (h->eh_depth == 0) {
            for (uint32_t i = 0; i < h->eh_entries; i++) {
                struct EXT4_EXTENT *e = ext_ent(blk, i);
                uint32_t len = e->ee_len;
                if (len > 32768u) {
                    len -= 32768u;
                }
                if (fblk >= e->ee_block && fblk < e->ee_block + len) {
                    uint32_t s = e->ee_start_lo |
                                 ((uint32_t)e->ee_start_hi << 16);
                    *out = s + (fblk - e->ee_block);
                    free_kernel_page((uint32_t)blk);
                    return 0;
                }
            }
            free_kernel_page((uint32_t)blk);
            return -1;
        }
        uint32_t ci = 0;
        int have = 0;
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            struct EXT4_EXTENT_IDX *x = ext_idx(blk, i);
            if (x->ei_block <= fblk) {
                ci = i;
                have = 1;
            }
        }
        if (!have) {
            free_kernel_page((uint32_t)blk);
            return -1;
        }
        struct EXT4_EXTENT_IDX *x = ext_idx(blk, ci);
        uint32_t nb = x->ei_leaf_lo | ((uint32_t)x->ei_leaf_hi << 16);
        memset(blk, 0, 4096);
        if (ext4_read_block(nb, blk) != 0) {
            free_kernel_page((uint32_t)blk);
            return -1;
        }
        h = ext_hdr(blk);
        if (h->eh_magic != EXT4_EXTENT_MAGIC) {
            free_kernel_page((uint32_t)blk);
            return -1;
        }
    }
}

static uint32_t ext_first_key(const uint8_t *p) {
    const struct EXT4_EXTENT_HEADER *h =
        (const struct EXT4_EXTENT_HEADER *)p;
    if (h->eh_entries == 0) {
        return 0;
    }
    if (h->eh_depth == 0) {
        return ((const struct EXT4_EXTENT *)((const uint8_t *)p + 12))[0]
            .ee_block;
    }
    return ((const struct EXT4_EXTENT_IDX *)((const uint8_t *)p + 12))[0]
        .ei_block;
}

static int leaf_insert(struct FS_INODE *ino, uint8_t *p, int is_root,
                       uint32_t owner, uint32_t fblk, uint32_t pblk,
                       uint32_t *skey, uint32_t *sblk) {
    struct EXT4_EXTENT_HEADER *h = ext_hdr(p);
    uint32_t n = h->eh_entries;
    uint32_t max = node_max(is_root);
    uint32_t pos = 0;
    while (pos < n && ext_ent(p, pos)->ee_block <= fblk) {
        pos++;
    }
    if (pos > 0) {
        struct EXT4_EXTENT *pr = ext_ent(p, pos - 1);
        uint32_t len = pr->ee_len;
        if (len > 32768u) {
            len -= 32768u;
        }
        uint32_t st = pr->ee_start_lo | ((uint32_t)pr->ee_start_hi << 16);
        if (fblk < pr->ee_block + len) {
            return 0;
        }
        if (pr->ee_block + len == fblk && st + len == pblk && len < 32768u) {
            pr->ee_len = (uint16_t)(len + 1u);
            if (!is_root) {
                ext_node_csum(ino, p, max);
                ext4_store_block(owner, p);
            }
            return 0;
        }
    }
    if (pos < n) {
        struct EXT4_EXTENT *nx = ext_ent(p, pos);
        uint32_t len = nx->ee_len;
        if (len > 32768u) {
            len -= 32768u;
        }
        uint32_t st = nx->ee_start_lo | ((uint32_t)nx->ee_start_hi << 16);
        if (nx->ee_block == fblk + 1u && st == pblk + 1u && len < 32768u) {
            nx->ee_block = fblk;
            nx->ee_start_lo = pblk & 0xFFFFu;
            nx->ee_start_hi = (uint16_t)(pblk >> 16);
            nx->ee_len = (uint16_t)(len + 1u);
            if (!is_root) {
                ext_node_csum(ino, p, max);
                ext4_store_block(owner, p);
            }
            return 0;
        }
    }

    uint8_t *tmp = NULL;
    uint32_t total = n + 1u;
    if (n >= max) {
        tmp = (uint8_t *)get_kernel_pages(1);
        if (tmp == NULL) {
            return -1;
        }
        memset(tmp, 0, 4096);
        struct EXT4_EXTENT_HEADER *th = ext_hdr(tmp);
        th->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
        th->eh_entries = (uint16_t)total;
        th->eh_max = (uint16_t)total;
        th->eh_depth = 0;
        for (uint32_t i = 0; i < pos; i++) {
            *ext_ent(tmp, i) = *ext_ent(p, i);
        }
        struct EXT4_EXTENT *ne = ext_ent(tmp, pos);
        ne->ee_block = fblk;
        ne->ee_len = 1;
        ne->ee_start_lo = pblk & 0xFFFFu;
        ne->ee_start_hi = (uint16_t)(pblk >> 16);
        for (uint32_t i = pos; i < n; i++) {
            *ext_ent(tmp, i + 1) = *ext_ent(p, i);
        }
    } else {
        for (uint32_t i = n; i > pos; i--) {
            *ext_ent(p, i) = *ext_ent(p, i - 1);
        }
        struct EXT4_EXTENT *ne = ext_ent(p, pos);
        ne->ee_block = fblk;
        ne->ee_len = 1;
        ne->ee_start_lo = pblk & 0xFFFFu;
        ne->ee_start_hi = (uint16_t)(pblk >> 16);
        h->eh_entries = (uint16_t)total;
        if (!is_root) {
            ext_node_csum(ino, p, max);
            ext4_store_block(owner, p);
        }
        return 0;
    }

    uint32_t left = (total + 1u) / 2u;
    uint32_t nblk = ext4_alloc_block();
    if (nblk == 0) {
        free_kernel_page((uint32_t)tmp);
        return -1;
    }
    if (is_root) {
        uint32_t la = ext4_alloc_block();
        if (la == 0) {
            ext4_free_block(nblk);
            free_kernel_page((uint32_t)tmp);
            return -1;
        }
        uint8_t *p1 = (uint8_t *)get_kernel_pages(1);
        if (p1 == NULL) {
            ext4_free_block(nblk);
            ext4_free_block(la);
            free_kernel_page((uint32_t)tmp);
            return -1;
        }
        memset(p1, 0, 4096);
        uint32_t m1 = node_max(0);
        struct EXT4_EXTENT_HEADER *h1 = ext_hdr(p1);
        h1->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
        h1->eh_entries = (uint16_t)left;
        h1->eh_max = (uint16_t)m1;
        h1->eh_depth = 0;
        for (uint32_t i = 0; i < left; i++) {
            *ext_ent(p1, i) = *ext_ent(tmp, i);
        }
        ext_node_csum(ino, p1, m1);
        ext4_store_block(la, p1);
        uint8_t *p2 = (uint8_t *)get_kernel_pages(1);
        if (p2 == NULL) {
            ext4_free_block(la);
            ext4_free_block(nblk);
            free_kernel_page((uint32_t)p1);
            free_kernel_page((uint32_t)tmp);
            return -1;
        }
        memset(p2, 0, 4096);
        struct EXT4_EXTENT_HEADER *h2 = ext_hdr(p2);
        h2->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
        h2->eh_entries = (uint16_t)(total - left);
        h2->eh_max = (uint16_t)m1;
        h2->eh_depth = 0;
        for (uint32_t i = left; i < total; i++) {
            *ext_ent(p2, i - left) = *ext_ent(tmp, i);
        }
        ext_node_csum(ino, p2, m1);
        ext4_store_block(nblk, p2);
        uint32_t k1 = ext_first_key(p1);
        uint32_t k2 = ext_first_key(p2);
        memset(ino->i_block, 0, 60);
        struct EXT4_EXTENT_HEADER *rh = ext_hdr((uint8_t *)ino->i_block);
        rh->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
        rh->eh_entries = 2;
        rh->eh_max = (uint16_t)EXT4_EXTENT_ROOT_MAX;
        rh->eh_depth = 1;
        struct EXT4_EXTENT_IDX *x0 =
            ext_idx((uint8_t *)ino->i_block, 0);
        x0->ei_block = k1;
        x0->ei_leaf_lo = la;
        x0->ei_leaf_hi = 0;
        struct EXT4_EXTENT_IDX *x1 =
            ext_idx((uint8_t *)ino->i_block, 1);
        x1->ei_block = k2;
        x1->ei_leaf_lo = nblk;
        x1->ei_leaf_hi = 0;
        free_kernel_page((uint32_t)p1);
        free_kernel_page((uint32_t)p2);
        free_kernel_page((uint32_t)tmp);
        return 0;
    }

    uint32_t m1 = node_max(0);
    memset(p, 0, 4096);
    h = ext_hdr(p);
    h->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    h->eh_entries = (uint16_t)left;
    h->eh_max = (uint16_t)m1;
    h->eh_depth = 0;
    for (uint32_t i = 0; i < left; i++) {
        *ext_ent(p, i) = *ext_ent(tmp, i);
    }
    uint8_t *p2 = (uint8_t *)get_kernel_pages(1);
    if (p2 == NULL) {
        free_kernel_page((uint32_t)tmp);
        return -1;
    }
    memset(p2, 0, 4096);
    struct EXT4_EXTENT_HEADER *h2 = ext_hdr(p2);
    h2->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    h2->eh_entries = (uint16_t)(total - left);
    h2->eh_max = (uint16_t)m1;
    h2->eh_depth = 0;
    for (uint32_t i = left; i < total; i++) {
        *ext_ent(p2, i - left) = *ext_ent(tmp, i);
    }
    ext_node_csum(ino, p2, m1);
    ext4_store_block(nblk, p2);
    ext_node_csum(ino, p, m1);
    ext4_store_block(owner, p);
    *skey = ext_first_key(p2);
    *sblk = nblk;
    free_kernel_page((uint32_t)p2);
    free_kernel_page((uint32_t)tmp);
    return 1;
}

static int ext_ins_node(struct FS_INODE *ino, uint8_t *p, int is_root,
                        uint32_t owner, uint32_t fblk, uint32_t pblk,
                        uint32_t *skey, uint32_t *sblk);

static int ext_root_grow(struct FS_INODE *ino) {
    uint8_t *r = (uint8_t *)ino->i_block;
    struct EXT4_EXTENT_HEADER *rh = ext_hdr(r);
    uint32_t nblk = ext4_alloc_block();
    if (nblk == 0) {
        return -1;
    }
    uint8_t *np = (uint8_t *)get_kernel_pages(1);
    if (np == NULL) {
        ext4_free_block(nblk);
        return -1;
    }
    memset(np, 0, 4096);
    uint32_t m = node_max(0);
    struct EXT4_EXTENT_HEADER *nh = ext_hdr(np);
    nh->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    nh->eh_entries = rh->eh_entries;
    nh->eh_max = (uint16_t)m;
    nh->eh_depth = rh->eh_depth;
    nh->eh_generation = rh->eh_generation;
    memcpy(np + 12, r + 12, (uint32_t)rh->eh_entries * 12u);
    ext_node_csum(ino, np, m);
    if (ext4_store_block(nblk, np) != 0) {
        free_kernel_page((uint32_t)np);
        ext4_free_block(nblk);
        return -1;
    }
    uint32_t k = ext_first_key(np);
    uint32_t depth = rh->eh_depth;
    memset(r, 0, 60);
    rh = ext_hdr(r);
    rh->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    rh->eh_entries = 1;
    rh->eh_max = (uint16_t)EXT4_EXTENT_ROOT_MAX;
    rh->eh_depth = (uint16_t)(depth + 1u);
    struct EXT4_EXTENT_IDX *x = ext_idx(r, 0);
    x->ei_block = k;
    x->ei_leaf_lo = nblk;
    x->ei_leaf_hi = 0;
    x->ei_unused = 0;
    free_kernel_page((uint32_t)np);
    return 0;
}

static int ext_ins_node(struct FS_INODE *ino, uint8_t *p, int is_root,
                        uint32_t owner, uint32_t fblk, uint32_t pblk,
                        uint32_t *skey, uint32_t *sblk) {
    struct EXT4_EXTENT_HEADER *h = ext_hdr(p);
    if (h->eh_depth == 0) {
        return leaf_insert(ino, p, is_root, owner, fblk, pblk, skey, sblk);
    }
    uint32_t ci = 0;
    uint32_t n = h->eh_entries;
    for (uint32_t i = 0; i < n; i++) {
        if (ext_idx(p, i)->ei_block <= fblk) {
            ci = i;
        }
    }
    struct EXT4_EXTENT_IDX *x = ext_idx(p, ci);
    uint32_t cblk = x->ei_leaf_lo | ((uint32_t)x->ei_leaf_hi << 16);
    uint8_t *cb = (uint8_t *)get_kernel_pages(1);
    if (cb == NULL) {
        return -1;
    }
    memset(cb, 0, 4096);
    if (ext4_read_block(cblk, cb) != 0) {
        free_kernel_page((uint32_t)cb);
        return -1;
    }
    uint32_t cskey = 0;
    uint32_t csblk = 0;
    int rc = ext_ins_node(ino, cb, 0, cblk, fblk, pblk, &cskey, &csblk);
    if (rc < 0) {
        free_kernel_page((uint32_t)cb);
        return -1;
    }
    struct EXT4_EXTENT_HEADER *ch = ext_hdr(cb);
    if (ch->eh_entries > 0) {
        struct EXT4_EXTENT_IDX *cx = x;
        uint32_t nk = ext_first_key(cb);
        if (cx->ei_block != nk) {
            cx->ei_block = nk;
        }
    }
    free_kernel_page((uint32_t)cb);
    if (rc == 0) {
        if (!is_root) {
            ext_node_csum(ino, p, node_max(is_root));
            ext4_store_block(owner, p);
        }
        return 0;
    }
    uint32_t max = node_max(is_root);
    if (h->eh_entries < max) {
        for (uint32_t i = h->eh_entries; i > ci + 1u; i--) {
            *ext_idx(p, i) = *ext_idx(p, i - 1);
        }
        struct EXT4_EXTENT_IDX *nx = ext_idx(p, ci + 1u);
        nx->ei_block = cskey;
        nx->ei_leaf_lo = csblk;
        nx->ei_leaf_hi = 0;
        nx->ei_unused = 0;
        h->eh_entries = (uint16_t)(h->eh_entries + 1u);
        if (!is_root) {
            ext_node_csum(ino, p, max);
            ext4_store_block(owner, p);
        }
        return 0;
    }
    if (is_root) {
        if (ext_root_grow(ino) != 0) {
            return -1;
        }
        p = (uint8_t *)ino->i_block;
        h = ext_hdr(p);
        struct EXT4_EXTENT_IDX *x0 = ext_idx(p, 0);
        uint32_t nb0 = x0->ei_leaf_lo | ((uint32_t)x0->ei_leaf_hi << 16);
        uint8_t *nb = (uint8_t *)get_kernel_pages(1);
        if (nb == NULL) {
            return -1;
        }
        memset(nb, 0, 4096);
        if (ext4_read_block(nb0, nb) != 0) {
            free_kernel_page((uint32_t)nb);
            return -1;
        }
        uint32_t m = node_max(0);
        struct EXT4_EXTENT_HEADER *bh = ext_hdr(nb);
        uint32_t bn = bh->eh_entries;
        for (uint32_t i = bn; i > ci + 1u; i--) {
            *ext_idx(nb, i) = *ext_idx(nb, i - 1);
        }
        struct EXT4_EXTENT_IDX *nx = ext_idx(nb, ci + 1u);
        nx->ei_block = cskey;
        nx->ei_leaf_lo = csblk;
        nx->ei_leaf_hi = 0;
        nx->ei_unused = 0;
        bh->eh_entries = (uint16_t)(bn + 1u);
        ext_node_csum(ino, nb, m);
        ext4_store_block(nb0, nb);
        x0->ei_block = ext_first_key(nb);
        free_kernel_page((uint32_t)nb);
        return 0;
    }

    uint8_t *tmp = (uint8_t *)get_kernel_pages(1);
    if (tmp == NULL) {
        return -1;
    }
    memset(tmp, 0, 4096);
    uint32_t total = n + 1u;
    struct EXT4_EXTENT_HEADER *th = ext_hdr(tmp);
    th->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    th->eh_entries = (uint16_t)total;
    th->eh_max = (uint16_t)total;
    th->eh_depth = h->eh_depth;
    for (uint32_t i = 0; i <= ci; i++) {
        *ext_idx(tmp, i) = *ext_idx(p, i);
    }
    struct EXT4_EXTENT_IDX *nx = ext_idx(tmp, ci + 1u);
    nx->ei_block = cskey;
    nx->ei_leaf_lo = csblk;
    nx->ei_leaf_hi = 0;
    nx->ei_unused = 0;
    for (uint32_t i = ci + 1u; i < n; i++) {
        *ext_idx(tmp, i + 1u) = *ext_idx(p, i);
    }
    uint32_t left = (total + 1u) / 2u;
    uint32_t nb2 = ext4_alloc_block();
    if (nb2 == 0) {
        free_kernel_page((uint32_t)tmp);
        return -1;
    }
    uint32_t m = node_max(0);
    uint16_t depth = h->eh_depth;
    memset(p, 0, 4096);
    h = ext_hdr(p);
    h->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    h->eh_entries = (uint16_t)left;
    h->eh_max = (uint16_t)m;
    h->eh_depth = depth;
    for (uint32_t i = 0; i < left; i++) {
        *ext_idx(p, i) = *ext_idx(tmp, i);
    }
    uint8_t *p2 = (uint8_t *)get_kernel_pages(1);
    if (p2 == NULL) {
        free_kernel_page((uint32_t)tmp);
        return -1;
    }
    memset(p2, 0, 4096);
    struct EXT4_EXTENT_HEADER *h2 = ext_hdr(p2);
    h2->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
    h2->eh_entries = (uint16_t)(total - left);
    h2->eh_max = (uint16_t)m;
    h2->eh_depth = depth;
    for (uint32_t i = left; i < total; i++) {
        *ext_idx(p2, i - left) = *ext_idx(tmp, i);
    }
    ext_node_csum(ino, p2, m);
    ext4_store_block(nb2, p2);
    ext_node_csum(ino, p, m);
    ext4_store_block(owner, p);
    uint32_t k2 = ext_first_key(p2);
    free_kernel_page((uint32_t)p2);
    free_kernel_page((uint32_t)tmp);
    *skey = k2;
    *sblk = nb2;
    return 1;
}

static int ext_insert(struct FS_INODE *ino, uint32_t fblk, uint32_t pblk) {
    uint8_t *r = (uint8_t *)ino->i_block;
    struct EXT4_EXTENT_HEADER *h = ext_hdr(r);
    if (!(ino->i_flags & EXT4_EXTENTS_FL) ||
        h->eh_magic != EXT4_EXTENT_MAGIC) {
        ext_leaf_init(ino);
        h = ext_hdr(r);
    }
    if (h->eh_entries == 0) {
        struct EXT4_EXTENT *e = ext_ent(r, 0);
        e->ee_block = fblk;
        e->ee_len = 1;
        e->ee_start_lo = pblk & 0xFFFFu;
        e->ee_start_hi = (uint16_t)(pblk >> 16);
        h->eh_entries = 1;
        return 0;
    }
    uint32_t skey = 0;
    uint32_t sblk = 0;
    return ext_ins_node(ino, r, 1, 0, fblk, pblk, &skey, &sblk);
}

static void ext_free_tree(uint32_t blk, uint8_t *buf) {
    memset(buf, 0, 4096);
    if (ext4_read_block(blk, buf) != 0) {
        return;
    }
    struct EXT4_EXTENT_HEADER *h = ext_hdr(buf);
    if (h->eh_magic != EXT4_EXTENT_MAGIC) {
        return;
    }
    if (h->eh_depth == 0) {
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            struct EXT4_EXTENT *e = ext_ent(buf, i);
            uint32_t len = e->ee_len;
            if (len > 32768u) {
                len -= 32768u;
            }
            uint32_t s = e->ee_start_lo | ((uint32_t)e->ee_start_hi << 16);
            for (uint32_t k = 0; k < len; k++) {
                ext4_free_block(s + k);
            }
        }
    } else {
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            struct EXT4_EXTENT_IDX *x = ext_idx(buf, i);
            uint32_t nb = x->ei_leaf_lo | ((uint32_t)x->ei_leaf_hi << 16);
            ext_free_tree(nb, buf);
        }
    }
    ext4_free_block(blk);
}

static void ext_count(uint32_t blk, uint8_t *buf, uint32_t depth,
                      uint32_t *data, uint32_t *index) {
    memset(buf, 0, 4096);
    if (ext4_read_block(blk, buf) != 0) {
        return;
    }
    struct EXT4_EXTENT_HEADER *h = ext_hdr(buf);
    if (h->eh_magic != EXT4_EXTENT_MAGIC) {
        return;
    }
    if (depth == 0) {
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            struct EXT4_EXTENT *e = ext_ent(buf, i);
            uint32_t len = e->ee_len;
            if (len > 32768u) {
                len -= 32768u;
            }
            *data += len;
        }
        return;
    }
    *index += h->eh_entries;
    for (uint32_t i = 0; i < h->eh_entries; i++) {
        struct EXT4_EXTENT_IDX *x = ext_idx(buf, i);
        uint32_t nb = x->ei_leaf_lo | ((uint32_t)x->ei_leaf_hi << 16);
        ext_count(nb, buf, depth - 1u, data, index);
    }
}

static void ext_inode_blocks(const uint32_t *ib, uint32_t *data,
                             uint32_t *index) {
    const uint8_t *r = (const uint8_t *)ib;
    const struct EXT4_EXTENT_HEADER *h =
        (const struct EXT4_EXTENT_HEADER *)r;
    if (h->eh_magic != EXT4_EXTENT_MAGIC) {
        return;
    }
    if (h->eh_depth == 0) {
        for (uint32_t i = 0; i < h->eh_entries; i++) {
            const struct EXT4_EXTENT *e =
                (const struct EXT4_EXTENT *)(r + 12 + i * 12);
            uint32_t len = e->ee_len;
            if (len > 32768u) {
                len -= 32768u;
            }
            *data += len;
        }
        return;
    }
    *index += h->eh_entries;
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < h->eh_entries; i++) {
        const struct EXT4_EXTENT_IDX *x =
            (const struct EXT4_EXTENT_IDX *)(r + 12 + i * 12);
        uint32_t nb = x->ei_leaf_lo | ((uint32_t)x->ei_leaf_hi << 16);
        ext_count(nb, buf, h->eh_depth - 1u, data, index);
    }
    free_kernel_page((uint32_t)buf);
}

static int ext4_read_inode_impl(uint32_t ino, struct FS_INODE *out) {
    if (disk == NULL || ino == 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / inode_size;
    if (per_block == 0) {
        free_kernel_page((uint32_t)buf);
        return -1;
    }
    uint32_t g = (ino - 1) / inodes_per_group;
    uint32_t idx = (ino - 1) % inodes_per_group;
    uint32_t blk = gd_inode_table(g) + idx / per_block;
    uint32_t off = (idx % per_block) * inode_size;
    if (ext4_read_block(blk, buf) != 0) {
        free_kernel_page((uint32_t)buf);
        return -1;
    }
    uint8_t *p = buf + off;
    memset(out, 0, sizeof(struct FS_INODE));
    out->i_no = ino;
    out->i_mode = ld16(p + 0x00);
    out->i_uid = ld16(p + 0x02);
    out->i_size = ld32(p + 0x04);
    out->i_atime = ld32(p + 0x08);
    out->i_ctime = ld32(p + 0x0C);
    out->i_mtime = ld32(p + 0x10);
    out->i_dtime = ld32(p + 0x14);
    out->i_gid = ld16(p + 0x18);
    out->i_links_count = ld16(p + 0x1A);
    out->i_blocks = ld32(p + 0x1C);
    out->i_flags = ld32(p + 0x20);
    for (uint32_t i = 0; i < 15; i++) {
        out->i_block[i] = ld32(p + 0x28 + i * 4u);
    }
    out->i_generation = ld32(p + 0x64);
    free_kernel_page((uint32_t)buf);
    return 0;
}

static void inode_csum_set(uint8_t *p, uint32_t ino, uint32_t gen) {
    if (!has_csum) {
        return;
    }
    uint8_t b[4];
    st32(b, ino);
    uint32_t c = crc32c(csum_seed, b, 4);
    st32(b, gen);
    c = crc32c(c, b, 4);
    uint8_t z[2];
    z[0] = 0;
    z[1] = 0;
    st16(p + 0x7C, 0);
    st16(p + 0x82, 0);
    c = crc32c(c, p, 0x7C);
    c = crc32c(c, z, 2);
    c = crc32c(c, p + 0x7E, 0x04);
    c = crc32c(c, z, 2);
    c = crc32c(c, p + 0x84, inode_size - 0x84);
    st16(p + 0x7C, (uint16_t)(c & 0xFFFFu));
    st16(p + 0x82, (uint16_t)(c >> 16));
}

static int ext4_write_inode_impl(uint32_t ino, const struct FS_INODE *in) {
    if (disk == NULL || ino == 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / inode_size;
    if (per_block == 0) {
        free_kernel_page((uint32_t)buf);
        return -1;
    }
    uint32_t g = (ino - 1) / inodes_per_group;
    uint32_t idx = (ino - 1) % inodes_per_group;
    uint32_t blk = gd_inode_table(g) + idx / per_block;
    uint32_t off = (idx % per_block) * inode_size;
    if (ext4_read_block(blk, buf) != 0) {
        free_kernel_page((uint32_t)buf);
        return -1;
    }
    uint8_t *p = buf + off;
    uint32_t fmt = in->i_mode & 0xF000u;
    uint32_t flags = in->i_flags;
    if (fmt == 0x8000u || fmt == 0x4000u) {
        flags |= EXT4_EXTENTS_FL;
    } else if (fmt != 0xA000u) {
        flags &= ~EXT4_EXTENTS_FL;
    }
    uint32_t ib[15];
    for (uint32_t i = 0; i < 15; i++) {
        ib[i] = in->i_block[i];
    }
    if (flags & EXT4_EXTENTS_FL) {
        struct EXT4_EXTENT_HEADER *ih = (struct EXT4_EXTENT_HEADER *)ib;
        if (ih->eh_magic != (uint16_t)EXT4_EXTENT_MAGIC) {
            memset(ib, 0, sizeof(ib));
            ih->eh_magic = (uint16_t)EXT4_EXTENT_MAGIC;
            ih->eh_entries = 0;
            ih->eh_max = (uint16_t)EXT4_EXTENT_ROOT_MAX;
            ih->eh_depth = 0;
            ih->eh_generation = 0;
        }
    }
    uint32_t nblocks = 0;
    if (flags & EXT4_EXTENTS_FL) {
        uint32_t data = 0;
        uint32_t index = 0;
        ext_inode_blocks(ib, &data, &index);
        nblocks = (data + index) * (bs / 512u);
    }
    st16(p + 0x00, (uint16_t)in->i_mode);
    st16(p + 0x02, (uint16_t)in->i_uid);
    st32(p + 0x04, in->i_size);
    st32(p + 0x08, in->i_atime);
    st32(p + 0x0C, in->i_ctime);
    st32(p + 0x10, in->i_mtime);
    st32(p + 0x14, in->i_dtime);
    st16(p + 0x18, (uint16_t)in->i_gid);
    st16(p + 0x1A, (uint16_t)in->i_links_count);
    st32(p + 0x1C, nblocks);
    st32(p + 0x20, flags);
    for (uint32_t i = 0; i < 15; i++) {
        st32(p + 0x28 + i * 4u, ib[i]);
    }
    st32(p + 0x64, in->i_generation);
    st32(p + 0x6C, 0);
    if (inode_size > 128u && ld16(p + 0x80) == 0) {
        st16(p + 0x80, (uint16_t)EXT4_INODE_EXTRA_ISIZE);
    }
    inode_csum_set(p, ino, in->i_generation);
    int rc = ext4_store_block(blk, buf);
    free_kernel_page((uint32_t)buf);
    return rc;
}

static int ext4_ensure_block(struct FS_INODE *ino, uint32_t fblk,
                             uint32_t *out) {
    uint32_t b = 0;
    if (ext_lookup(ino, fblk, &b) == 0) {
        *out = b;
        return 0;
    }
    b = ext4_alloc_block();
    if (b == 0) {
        return -1;
    }
    if (ext_insert(ino, fblk, b) != 0) {
        ext4_free_block(b);
        return -1;
    }
    *out = b;
    return 0;
}

int ext4_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                        uint32_t count) {
    rwlock_write_acquire(&ext4_lock);
    int rc = ext4_write_to_inode_impl(ino, off, buf, count);
    rwlock_write_release(&ext4_lock);
    return rc;
}

static int ext4_write_to_inode_impl(struct FS_INODE *ino, uint32_t off,
                                    const void *buf, uint32_t count) {
    if (disk == NULL || ino == NULL || ino->i_no == 0) {
        return 0;
    }
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return 0;
    }
    uint32_t done = 0;
    while (done < count) {
        uint32_t fblk = (off + done) / bs;
        uint32_t within = (off + done) % bs;
        uint32_t addr = 0;
        if (ext4_ensure_block(ino, fblk, &addr) != 0) {
            break;
        }
        memset(blk, 0, 4096);
        ext4_read_block(addr, blk);
        uint32_t chunk = bs - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        memcpy(blk + within, (const uint8_t *)buf + done, chunk);
        ext4_store_data(addr, blk);
        done += chunk;
    }
    free_kernel_page((uint32_t)blk);
    if (off + done > ino->i_size) {
        ino->i_size = off + done;
    }
    if (done > 0) {
        uint32_t now = (uint32_t)rtc_unix_time();
        ino->i_mtime = now;
        ino->i_ctime = now;
        ext4_write_inode_impl(ino->i_no, ino);
    }
    return (int)done;
}

void ext4_truncate_inode(struct FS_INODE *ino) {
    rwlock_write_acquire(&ext4_lock);
    ext4_truncate_inode_impl(ino);
    rwlock_write_release(&ext4_lock);
}

static void ext4_truncate_inode_impl(struct FS_INODE *ino) {
    if (ino == NULL) {
        return;
    }
    const uint8_t *r = (const uint8_t *)ino->i_block;
    const struct EXT4_EXTENT_HEADER *h =
        (const struct EXT4_EXTENT_HEADER *)r;
    if ((ino->i_flags & EXT4_EXTENTS_FL) &&
        h->eh_magic == EXT4_EXTENT_MAGIC) {
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf != NULL) {
            if (h->eh_depth == 0) {
                for (uint32_t i = 0; i < h->eh_entries; i++) {
                    const struct EXT4_EXTENT *e =
                        (const struct EXT4_EXTENT *)(r + 12 + i * 12);
                    uint32_t len = e->ee_len;
                    if (len > 32768u) {
                        len -= 32768u;
                    }
                    uint32_t s = e->ee_start_lo |
                                 ((uint32_t)e->ee_start_hi << 16);
                    for (uint32_t k = 0; k < len; k++) {
                        ext4_free_block(s + k);
                    }
                }
            } else {
                for (uint32_t i = 0; i < h->eh_entries; i++) {
                    const struct EXT4_EXTENT_IDX *x =
                        (const struct EXT4_EXTENT_IDX *)(r + 12 + i * 12);
                    uint32_t nb = x->ei_leaf_lo |
                                  ((uint32_t)x->ei_leaf_hi << 16);
                    ext_free_tree(nb, buf);
                }
            }
            free_kernel_page((uint32_t)buf);
        }
    }
    memset(ino->i_block, 0, sizeof(ino->i_block));
    ino->i_flags &= ~EXT4_EXTENTS_FL;
    ino->i_size = 0;
}

int ext4_new_inode(uint32_t mode, struct FS_INODE *out) {
    rwlock_write_acquire(&ext4_lock);
    uint32_t rc = ext4_new_inode_impl(mode, out);
    rwlock_write_release(&ext4_lock);
    return (int)rc;
}

static uint32_t ext4_new_inode_impl(uint32_t mode, struct FS_INODE *out) {
    uint32_t ino = ext4_alloc_inode();
    if (ino == 0) {
        return 0;
    }
    memset(out, 0, sizeof(struct FS_INODE));
    out->i_no = ino;
    out->i_mode = mode;
    out->i_size = 0;
    out->i_atime = (uint32_t)rtc_unix_time();
    out->i_ctime = out->i_atime;
    out->i_mtime = out->i_atime;
    uint32_t fmt = mode & 0xF000u;
    out->i_links_count = fmt == 0x4000u ? 2u : 1u;
    if (fmt == 0x8000u || fmt == 0x4000u) {
        ext_leaf_init(out);
    }
    uint32_t g = (ino - 1) / inodes_per_group;
    if (fmt == 0x4000u) {
        gd_set16(g, 0x10, 0x30, gd_used_dirs(g) + 1);
        gdt_flush(g);
    }
    if (ext4_write_inode_impl(ino, out)) {
        ext4_free_inode_impl(ino);
        return 0;
    }
    return ino;
}

void ext4_free_inode(uint32_t ino) {
    rwlock_write_acquire(&ext4_lock);
    ext4_free_inode_impl(ino);
    rwlock_write_release(&ext4_lock);
}

int ext4_write_inode(uint32_t ino, const struct FS_INODE *in) {
    rwlock_write_acquire(&ext4_lock);
    int rc = ext4_write_inode_impl(ino, in);
    rwlock_write_release(&ext4_lock);
    return rc;
}

int ext4_read_inode(uint32_t ino, struct FS_INODE *out) {
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_read_inode_impl(ino, out);
    rwlock_read_release(&ext4_lock);
    return rc;
}

static uint32_t dir_limit(void) {
    return has_csum ? bs - 12u : bs;
}

static void dir_tail_fix(const struct FS_INODE *dino, uint8_t *blk) {
    if (!has_csum) {
        return;
    }
    struct EXT4_DIR_TAIL *t = (struct EXT4_DIR_TAIL *)(blk + bs - 12);
    t->det_reserved_zero1 = 0;
    t->det_rec_len = 12;
    t->det_reserved_zero2 = 0;
    t->det_reserved_ft = 0xDEu;
    uint8_t b[4];
    st32(b, dino->i_no);
    uint32_t c = crc32c(csum_seed, b, 4);
    st32(b, dino->i_generation);
    c = crc32c(c, b, 4);
    c = crc32c(c, blk, bs - 12u);
    st32(blk + bs - 4, c);
}

int ext4_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                   int is_dir) {
    return ext4_add_entry_dt(dino, ino, name,
                             is_dir ? (uint8_t)EXT4_DT_DIR : 1u);
}

int ext4_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                      uint8_t dtype) {
    rwlock_write_acquire(&ext4_lock);
    int rc = ext4_add_entry_impl(dino, ino, name, dtype);
    rwlock_write_release(&ext4_lock);
    return rc;
}

static void dino_link_inc(struct FS_INODE *dino, uint8_t dtype,
                          const char *name) {
    if (dtype != (uint8_t)EXT4_DT_DIR) {
        return;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    dino->i_links_count++;
    ext4_write_inode_impl(dino->i_no, dino);
}

static int ext4_add_entry_impl(struct FS_INODE *dino, uint32_t ino,
                               const char *name, uint8_t dtype) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= EXT4_DIRENT_NAME_MAX) {
        return -1;
    }
    uint32_t need = (8u + nl + 3u) & ~3u;
    uint32_t limit = dir_limit();
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }

    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext_lookup(dino, fblk, &addr) != 0) {
            break;
        }
        memset(blk, 0, 4096);
        ext4_read_block(addr, blk);
        uint32_t off = 0;
        uint32_t target = 0xFFFFFFFFu;
        uint32_t slot_rec = 0;
        while (off < limit) {
            struct EXT4_DIRENT *de = (struct EXT4_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u || off + rl > limit) {
                if (limit - off >= need) {
                    target = off;
                    slot_rec = limit - off;
                }
                break;
            }
            if (de->inode == 0) {
                if (rl >= need) {
                    target = off;
                    slot_rec = rl;
                    break;
                }
            } else {
                uint32_t used = (8u + (uint32_t)de->name_len + 3u) & ~3u;
                if (used < rl && rl - used >= need) {
                    de->rec_len = (uint16_t)used;
                    struct EXT4_DIRENT *nd =
                        (struct EXT4_DIRENT *)(blk + off + used);
                    nd->inode = ino;
                    nd->rec_len = (uint16_t)(rl - used);
                    nd->name_len = (uint8_t)nl;
                    nd->file_type = dtype;
                    memcpy(nd->name, name, nl);
                    dir_tail_fix(dino, blk);
                    ext4_store_block(addr, blk);
                    free_kernel_page((uint32_t)blk);
                    dino_link_inc(dino, dtype, name);
                    return 0;
                }
            }
            off += rl;
        }
        if (target != 0xFFFFFFFFu) {
            struct EXT4_DIRENT *de = (struct EXT4_DIRENT *)(blk + target);
            de->inode = ino;
            de->rec_len = (uint16_t)slot_rec;
            de->name_len = (uint8_t)nl;
            de->file_type = dtype;
            memcpy(de->name, name, nl);
            dir_tail_fix(dino, blk);
            ext4_store_block(addr, blk);
            free_kernel_page((uint32_t)blk);
            dino_link_inc(dino, dtype, name);
            return 0;
        }
    }

    uint32_t nfblk = dino->i_size / bs;
    uint32_t naddr = 0;
    if (ext4_ensure_block(dino, nfblk, &naddr) != 0) {
        free_kernel_page((uint32_t)blk);
        return -1;
    }
    memset(blk, 0, 4096);
    struct EXT4_DIRENT *de = (struct EXT4_DIRENT *)blk;
    de->inode = ino;
    de->rec_len = (uint16_t)limit;
    de->name_len = (uint8_t)nl;
    de->file_type = dtype;
    memcpy(de->name, name, nl);
    dir_tail_fix(dino, blk);
    ext4_store_block(naddr, blk);
    dino->i_size += bs;
    ext4_write_inode_impl(dino->i_no, dino);
    free_kernel_page((uint32_t)blk);
    dino_link_inc(dino, dtype, name);
    return 0;
}

int ext4_remove_entry(struct FS_INODE *dino, const char *name) {
    rwlock_write_acquire(&ext4_lock);
    int rc = ext4_remove_entry_impl(dino, name);
    rwlock_write_release(&ext4_lock);
    return rc;
}

static int ext4_remove_entry_impl(struct FS_INODE *dino, const char *name) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= EXT4_DIRENT_NAME_MAX) {
        return -1;
    }
    uint32_t limit = dir_limit();
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }
    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext_lookup(dino, fblk, &addr) != 0) {
            break;
        }
        memset(blk, 0, 4096);
        ext4_read_block(addr, blk);
        uint32_t off = 0;
        while (off < limit) {
            struct EXT4_DIRENT *de = (struct EXT4_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u || off + rl > limit) {
                break;
            }
            if (de->inode != 0 && de->name_len == nl &&
                memcmp(de->name, name, nl) == 0) {
                uint8_t dt = de->file_type;
                de->inode = 0;
                de->name_len = 0;
                dir_tail_fix(dino, blk);
                ext4_store_block(addr, blk);
                free_kernel_page((uint32_t)blk);
                if (dt == (uint8_t)EXT4_DT_DIR &&
                    strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                    dino->i_links_count > 0) {
                    dino->i_links_count--;
                    ext4_write_inode_impl(dino->i_no, dino);
                }
                return 0;
            }
            off += rl;
        }
    }
    free_kernel_page((uint32_t)blk);
    return -1;
}

int ext4_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                         uint32_t count) {
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_read_from_inode_impl(ino, off, buf, count);
    rwlock_read_release(&ext4_lock);
    return rc;
}

#define EXT4_READ_BLOCKS 32u
#define EXT4_STREAM_MIN 65536u
#define EXT4_STREAM_BLOCKS 128u

static int ext4_read_from_inode_impl(const struct FS_INODE *ino, uint32_t off,
                                     void *buf, uint32_t count) {
    uint32_t done = 0;
    if (ino->i_no == 0 || off >= ino->i_size) {
        return 0;
    }
    if (off + count > ino->i_size) {
        count = ino->i_size - off;
    }
    if (count >= EXT4_STREAM_MIN && (off % bs) == 0) {
        uint32_t limit = count - (count % bs);
        while (done < limit) {
            uint32_t fblk = (off + done) / bs;
            uint32_t maxb = (limit - done) / bs;
            uint32_t addr = 0;
            uint32_t run = 1;
            if (maxb > EXT4_STREAM_BLOCKS) {
                maxb = EXT4_STREAM_BLOCKS;
            }
            if (ext_lookup(ino, fblk, &addr) != 0) {
                uint32_t z = limit - done;
                if (z > bs) {
                    z = bs;
                }
                memset((uint8_t *)buf + done, 0, z);
                done += z;
                continue;
            }
            while (run < maxb) {
                uint32_t next = 0;
                if (ext_lookup(ino, fblk + run, &next) != 0) {
                    break;
                }
                if (next != addr + run) {
                    break;
                }
                run++;
            }
            ext4_read_blocks(addr, run, (uint8_t *)buf + done);
            done += run * bs;
        }
        if (done >= count) {
            return (int)done;
        }
    }
    uint32_t pages = DIV_ROUND_UP(EXT4_READ_BLOCKS * bs, PAGE_SIZE);
    uint8_t *blk = (uint8_t *)get_kernel_pages(pages);
    if (blk == NULL) {
        return (int)done;
    }
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t fblk = pos / bs;
        uint32_t within = pos % bs;
        uint32_t addr = 0;
        if (ext_lookup(ino, fblk, &addr) != 0) {
            uint32_t z = bs - within;
            if (z > count - done) {
                z = count - done;
            }
            memset((uint8_t *)buf + done, 0, z);
            done += z;
            continue;
        }
        uint32_t run = 1;
        while (run < EXT4_READ_BLOCKS && within + run * bs < count) {
            uint32_t next = 0;
            if (ext_lookup(ino, fblk + run, &next) != 0) {
                break;
            }
            if (next != addr + run) {
                break;
            }
            run++;
        }
        uint32_t cached = 1;
        for (uint32_t i = 0; i < run; i++) {
            if (ext4_bcache_peek(addr + i) == NULL) {
                cached = 0;
                break;
            }
        }
        if (cached) {
            for (uint32_t i = 0; i < run; i++) {
                memcpy(blk + i * bs, ext4_bcache_peek(addr + i), bs);
            }
        } else {
            ext4_read_blocks(addr, run, blk);
        }
        uint32_t avail = run * bs - within;
        uint32_t chunk = count - done;
        if (chunk > avail) {
            chunk = avail;
        }
        memcpy((uint8_t *)buf + done, blk + within, chunk);
        done += chunk;
    }
    for (uint32_t i = 0; i < pages; i++) {
        free_kernel_page((uint32_t)blk + i * PAGE_SIZE);
    }
    return (int)done;
}

int ext4_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                  struct FS_DIRENT *out) {
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_dir_next_impl(dino, pos, out);
    rwlock_read_release(&ext4_lock);
    return rc;
}

static int ext4_dir_next_impl(const struct FS_INODE *dino, uint32_t *pos,
                              struct FS_DIRENT *out) {
    uint32_t limit = dir_limit();
    while (*pos < dino->i_size) {
        uint32_t fblk = *pos / bs;
        uint32_t addr = 0;
        if (ext_lookup(dino, fblk, &addr) != 0) {
            *pos = (fblk + 1) * bs;
            continue;
        }
        uint8_t *blk = (uint8_t *)get_kernel_pages(1);
        if (blk == NULL) {
            return -1;
        }
        memset(blk, 0, 4096);
        ext4_read_block(addr, blk);
        uint32_t off = 0;
        while (off < limit) {
            struct EXT4_DIRENT *de = (struct EXT4_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u || off + rl > limit) {
                break;
            }
            uint32_t abs = fblk * bs + off;
            if (de->inode != 0 && de->name_len > 0 &&
                de->name_len <= EXT4_DIRENT_NAME_MAX && abs >= *pos) {
                uint32_t nl = de->name_len;
                if (nl >= MAX_FILE_NAME_LEN) {
                    nl = MAX_FILE_NAME_LEN - 1;
                }
                memset(out->filename, 0, MAX_FILE_NAME_LEN);
                memcpy(out->filename, de->name, nl);
                out->i_no = de->inode;
                out->f_type = de->file_type == EXT4_DT_DIR ? FT_DIRECTORY
                             : de->file_type == EXT4_DT_CHR ? FT_CHARDEVICE
                             : de->file_type == EXT4_DT_LNK ? FT_SYMLINK
                                                           : FT_REGULAR;
                *pos = abs + rl;
                free_kernel_page((uint32_t)blk);
                return 0;
            }
            off += rl;
        }
        free_kernel_page((uint32_t)blk);
        *pos = (fblk + 1) * bs;
    }
    return -1;
}

static int ext4_find_in_dir(const struct FS_INODE *dino, const char *name,
                            uint32_t *child, int *ftype) {
    uint32_t pos = 0;
    struct FS_DIRENT de;
    while (ext4_dir_next_impl(dino, &pos, &de) == 0) {
        if (strcmp(de.filename, name) == 0) {
            *child = de.i_no;
            *ftype = (int)de.f_type;
            return 0;
        }
    }
    return -1;
}

static int ext4_read_target(uint32_t ino, char *buf, uint32_t cap) {
    struct FS_INODE node;
    if (ext4_read_inode_impl(ino, &node) ||
        (node.i_mode & 0xF000u) != 0xA000u) {
        return -1;
    }
    uint32_t len = node.i_size < cap - 1 ? node.i_size : cap - 1;
    if (node.i_size < 60u) {
        memcpy(buf, &node.i_block[0], len);
    } else if (ext4_read_from_inode_impl(&node, 0, buf, len) != (int)len) {
        return -1;
    }
    buf[len] = 0;
    return (int)len;
}

int ext4_abs_path(const char *path, char *out, uint32_t cap) {
    if (path == NULL || path[0] == 0) {
        return -1;
    }
    if (path[0] == '/') {
        if (strlen(path) >= cap) {
            return -1;
        }
        strcpy(out, path);
        return 0;
    }
    char pre[MAX_PATH_LEN];
    if (fs_cwd_abs_prefix(pre, sizeof(pre)) != 0) {
        return -1;
    }
    uint32_t pl = (uint32_t)strlen(pre);
    uint32_t rl = (uint32_t)strlen(path);
    if (pl + 1 + rl >= cap) {
        return -1;
    }
    memcpy(out, pre, pl);
    if (pl == 0 || pre[pl - 1] != '/') {
        out[pl++] = '/';
    }
    memcpy(out + pl, path, rl + 1);
    return 0;
}

int ext4_lookup(const char *path, uint32_t *ino, int *is_dir) {
    char abs[MAX_PATH_LEN];
    if (path != NULL && path[0] != '/') {
        if (ext4_abs_path(path, abs, sizeof(abs)) != 0) {
            return -1;
        }
        path = abs;
    }
    int ft = 0;
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_lookup_depth(path, ino, &ft, 1, 8);
    rwlock_read_release(&ext4_lock);
    *is_dir = (ft == FT_DIRECTORY);
    return rc;
}

int ext4_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow) {
    char abs[MAX_PATH_LEN];
    if (path != NULL && path[0] != '/') {
        if (ext4_abs_path(path, abs, sizeof(abs)) != 0) {
            return -1;
        }
        path = abs;
    }
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_lookup_depth(path, ino, ftype, follow, 8);
    rwlock_read_release(&ext4_lock);
    return rc;
}

int ext4_read_link_target(uint32_t ino, char *buf, uint32_t cap) {
    rwlock_read_acquire(&ext4_lock);
    int rc = ext4_read_target(ino, buf, cap);
    rwlock_read_release(&ext4_lock);
    return rc;
}

static int ext4_lookup_depth(const char *path, uint32_t *ino, int *ftype,
                             int follow, int depth) {
    if (disk == NULL || path == NULL || path[0] != '/') {
        return -1;
    }
    char cur[MAX_PATH_LEN];
    uint32_t clen = (uint32_t)strlen(path);
    if (clen >= MAX_PATH_LEN) {
        return -1;
    }
    memcpy(cur, path, clen + 1);
    for (;;) {
        if (--depth < 0) {
            return -1;
        }
        uint32_t cino = 2;
        int cdir = 1;
        *ino = 2;
        *ftype = FT_DIRECTORY;
        const char *p = cur;
        while (*p == '/') {
            p++;
        }
        if (*p == 0) {
            return 0;
        }
        struct FS_INODE node;
        if (ext4_read_inode_impl(2, &node)) {
            return -1;
        }
        for (;;) {
            const char *cstart = p;
            char comp[MAX_FILE_NAME_LEN];
            uint32_t ci = 0;
            while (*p && *p != '/' && ci < MAX_FILE_NAME_LEN - 1) {
                comp[ci++] = *p++;
            }
            comp[ci] = 0;
            if (ci == 0) {
                break;
            }
            uint32_t child = 0;
            int ft = 0;
            if (ext4_find_in_dir(&node, comp, &child, &ft)) {
                return -1;
            }
            if (ft == FT_SYMLINK) {
                char tgt[MAX_PATH_LEN];
                if (!follow || ext4_read_target(child, tgt, MAX_PATH_LEN) < 0) {
                    *ino = child;
                    *ftype = ft;
                    cdir = 0;
                    return 0;
                }
                char np[MAX_PATH_LEN];
                uint32_t plen = (uint32_t)(cstart - cur);
                uint32_t tlen = (uint32_t)strlen(tgt);
                if (tgt[0] == '/') {
                    plen = 0;
                } else {
                    while (plen > 1 && cur[plen - 1] == '/') {
                        plen--;
                    }
                }
                if (plen + tlen + 2 > MAX_PATH_LEN) {
                    return -1;
                }
                memcpy(np, cur, plen);
                if (plen > 1 || (plen == 1 && cur[0] != '/')) {
                    np[plen++] = '/';
                } else if (plen == 0) {
                    np[plen++] = '/';
                }
                memcpy(np + plen, tgt, tlen + 1);
                memcpy(cur, np, plen + tlen + 1);
                break;
            }
            cino = child;
            cdir = (ft == FT_DIRECTORY);
            while (*p == '/') {
                p++;
            }
            if (*p == 0) {
                *ino = cino;
                *ftype = ft;
                return 0;
            }
            if (!cdir || ext4_read_inode_impl(child, &node)) {
                return -1;
            }
        }
        *ino = cino;
        *ftype = cdir ? FT_DIRECTORY : FT_REGULAR;
    }
}

void ext4_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree) {
    rwlock_read_acquire(&ext4_lock);
    if (bsize) *bsize = bs;
    if (blocks) *blocks = total_blocks;
    if (bfree) *bfree = free_blocks;
    if (files) *files = inodes_per_group;
    if (ffree) *ffree = free_inodes;
    rwlock_read_release(&ext4_lock);
}

struct DISK_PARTITION *ext4_partition(void) {
    return part;
}

static int ext4_load_gdt(void) {
    gdt_blocks = (n_groups * desc_size + bs - 1) / bs;
    if (n_groups > EXT4_GDT_MAX || gdt_blocks * bs > sizeof(g_gdt)) {
        return -1;
    }
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        if (ext4_read_block(gdt_blk + i, g_gdt + i * bs) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ext4_journal_map_init(uint32_t jinum) {
    j_nmap = 0;
    struct FS_INODE jino;
    if (ext4_read_inode_impl(jinum, &jino)) {
        return -1;
    }
    const uint8_t *r = (const uint8_t *)jino.i_block;
    const struct EXT4_EXTENT_HEADER *h =
        (const struct EXT4_EXTENT_HEADER *)r;
    if (h->eh_magic != EXT4_EXTENT_MAGIC || h->eh_depth != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < h->eh_entries && j_nmap < 2u; i++) {
        const struct EXT4_EXTENT *e =
            (const struct EXT4_EXTENT *)(r + 12 + i * 12);
        if (e->ee_len == 0 || e->ee_len > 32768u) {
            return -1;
        }
        j_map_log[j_nmap] = e->ee_block;
        j_map_phys[j_nmap] =
            e->ee_start_lo | ((uint32_t)e->ee_start_hi << 16);
        j_nmap++;
    }
    if (j_nmap == 0 || j_map_log[0] != 0) {
        j_nmap = 0;
        return -1;
    }
    return 0;
}

static uint32_t jrnl_phys(uint32_t jblk) {
    for (uint32_t i = 0; i < j_nmap; i++) {
        if (jblk >= j_map_log[i] && jblk < j_map_log[i] + 4096u) {
            return j_map_phys[i] + (jblk - j_map_log[i]);
        }
    }
    return 0;
}

static void ext4_journal_replay(void) {
    uint32_t pblk = jrnl_phys(0);
    if (pblk == 0) {
        has_journal = 0;
        return;
    }
    memset(g_jsb, 0, 1024);
    if (jsb_read(pblk, g_jsb) != 0) {
        has_journal = 0;
        return;
    }
    if (rd_be32(g_jsb + 0) != EXT4_JBD2_MAGIC ||
        rd_be32(g_jsb + 4) != 4) {
        has_journal = 0;
        return;
    }
    j_maxlen = rd_be32(g_jsb + 0x10);
    j_first = rd_be32(g_jsb + 0x14);
    j_seq = rd_be32(g_jsb + 0x18);
    uint32_t s_start = rd_be32(g_jsb + 0x1C);
    if (j_first == 0) {
        j_first = 1;
    }
    if (s_start == 0) {
        return;
    }
    uint8_t *jb = (uint8_t *)get_kernel_pages(1);
    uint8_t *db = (uint8_t *)get_kernel_pages(1);
    if (jb == NULL || db == NULL) {
        if (jb) free_kernel_page((uint32_t)jb);
        if (db) free_kernel_page((uint32_t)db);
        return;
    }
    uint32_t log = s_start;
    uint32_t seq = j_seq;
    for (;;) {
        if (log >= j_maxlen) {
            break;
        }
        memset(jb, 0, 4096);
        uint32_t pb = jrnl_phys(log);
        if (pb == 0 || ext4_disk_read(pb, jb) != 0) {
            break;
        }
        if (rd_be32(jb + 0) != EXT4_JBD2_MAGIC) {
            break;
        }
        if (rd_be32(jb + 4) != EXT4_JT_DESCRIPTOR) {
            break;
        }
        if (rd_be32(jb + 8) != seq) {
            break;
        }
        uint32_t off = 12;
        uint32_t n = 0;
        for (;;) {
            if (off + 8u > bs) {
                break;
            }
            uint32_t tgt = rd_be32(jb + off);
            uint16_t fl = (uint16_t)(((uint16_t)jb[off + 6] << 8) |
                                     (uint16_t)jb[off + 7]);
            off += 8;
            if (!(fl & (uint16_t)EXT4_JF_SAME_UUID)) {
                off += 16;
            }
            uint32_t dpb2 = jrnl_phys(log + 1u + n);
            if (dpb2 == 0) {
                break;
            }
            memset(db, 0, 4096);
            ext4_disk_read(dpb2, db);
            if (fl & (uint16_t)EXT4_JF_ESCAPE) {
                wr_be32(db, EXT4_JBD2_MAGIC);
            }
            if (!(fl & (uint16_t)EXT4_JF_DELETED)) {
                ext4_raw_write(tgt, db);
            }
            n++;
            if (fl & (uint16_t)EXT4_JF_LAST_TAG) {
                break;
            }
        }
        log += 1u + n;
        if (log >= j_maxlen) {
            break;
        }
        memset(jb, 0, 4096);
        uint32_t cb = jrnl_phys(log);
        if (cb == 0 || ext4_disk_read(cb, jb) != 0) {
            break;
        }
        if (rd_be32(jb + 0) != EXT4_JBD2_MAGIC ||
            rd_be32(jb + 4) != EXT4_JT_COMMIT || rd_be32(jb + 8) != seq) {
            break;
        }
        seq++;
        log++;
    }
    free_kernel_page((uint32_t)jb);
    free_kernel_page((uint32_t)db);
    j_seq = seq;
    ext4_jsb_write(0, seq);
}

int ext4_init(void) {
    if (!crc_ready) {
        ext4_crc_init();
    }
    rwlock_init(&ext4_lock);
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *p = list_entry(e, struct DISK_PARTITION, part_tag);
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf == NULL) {
            return -1;
        }
        memset(buf, 0, 4096);
        BLOCK.read_sectors(p->my_disk, p->start_lba, buf, 4);
        uint8_t *sb = buf + 1024;
        if (ld16(sb + 0x38) != EXT4_SUPER_MAGIC) {
            free_kernel_page((uint32_t)buf);
            e = e->next;
            continue;
        }
        uint32_t incompat = ld32(sb + 0x60);
        uint32_t ro = ld32(sb + 0x64);
        if ((incompat & ~EXT4_FEATURE_SUPPORTED_INCOMPAT) ||
            (ro & ~EXT4_FEATURE_SUPPORTED_RO)) {
            free_kernel_page((uint32_t)buf);
            e = e->next;
            continue;
        }
        disk = p->my_disk;
        part = p;
        start = p->start_lba;
        memcpy(g_sb, sb, 1024);
        bs = 1024u << ld32(g_sb + 0x18);
        sect_per_block = bs / 512u;
        first_data_block = ld32(g_sb + 0x14);
        blocks_per_group = ld32(g_sb + 0x20);
        inodes_per_group = ld32(g_sb + 0x28);
        total_blocks = ld32(g_sb + 0x04) | (ld32(g_sb + 0x150) << 16);
        free_blocks = ld32(g_sb + 0x0C);
        free_inodes = ld32(g_sb + 0x10);
        desc_size = ld16(g_sb + 0xFE);
        if (desc_size == 0) {
            desc_size = 32;
        }
        inode_size = ld16(g_sb + 0x58);
        if (inode_size == 0) {
            inode_size = 128;
        }
        has_64bit = (incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
        has_csum = (ro & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
        memcpy(uuid, g_sb + 0x68, 16);
        if (incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) {
            csum_seed = ld32(g_sb + 0x270);
        } else {
            csum_seed = crc32c(0xFFFFFFFFu, uuid, 16);
        }
        if (bs < 1024u || bs > 4096u || blocks_per_group == 0 ||
            inodes_per_group == 0 || inode_size < 128u || inode_size > bs) {
            free_kernel_page((uint32_t)buf);
            return -1;
        }
        uint64_t tb = (uint64_t)total_blocks;
        n_groups = (uint32_t)((tb - first_data_block + blocks_per_group - 1) /
                              blocks_per_group);
        gdt_blk = first_data_block + 1;
        free_kernel_page((uint32_t)buf);
        if (n_groups > EXT4_GDT_MAX) {
            return -1;
        }
        if (ext4_load_gdt() != 0) {
            return -1;
        }
        if (s_bc_data == NULL) {
            s_bc_slots = EXT4_BCACHE_SLOTS;
            while (s_bc_slots >= 64u) {
                uint32_t need = DIV_ROUND_UP(s_bc_slots * bs, PAGE_SIZE);
                s_bc_data = (uint8_t *)get_kernel_pages(need);
                if (s_bc_data != NULL) {
                    break;
                }
                s_bc_slots /= 2u;
            }
            if (s_bc_data == NULL) {
                kprintf("ext4: block cache disabled (no memory)\n");
            }
        }
        has_journal = ((ld32(g_sb + 0x5C) & 0x4u) != 0) &&
                      (ld32(g_sb + 0xE0) != 0);
        if (has_journal) {
            if (ext4_journal_map_init(ld32(g_sb + 0xE0)) != 0) {
                has_journal = 0;
            } else {
                ext4_journal_replay();
            }
        }
        kprintf("ext4 mounted on %s, block_size=%d, inodes_per_group=%d, "
                "groups=%d, journal=%s\n",
                p->name, (int)bs, (int)inodes_per_group, (int)n_groups,
                has_journal ? "on" : "off");
        return 0;
    }
    kprintf("ext4: no ext4 filesystem found\n");
    return -1;
}