#include "kernel/fs/ext2.h"
#include "kernel/fs/fs.h"
#include "kernel/sched/thread.h"

#include "ops/block_ops.h"
#include "kernel/sched/sync.h"
#include "drivers/char/console/io.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/fs/dir.h"
#include "drivers/char/rtc.h"

/**
 * ext2 元数据读写锁。
 *
 * @remarks
 * 取代原先"一把可重入全局锁串行化所有元数据操作"的做法：
 *  - 只读路径（读 inode / 读文件 / 目录遍历 / 路径解析 / 读链接 / statfs）
 *    取共享锁，彼此可并行；
 *  - 会改动磁盘元数据的路径（写 inode、写文件、truncate、分配与释放 inode、
 *    增删目录项）取排他锁。
 * 单分区单块组共用一份位图与超级块，写者之间天然仍需串行，但读多写少的负载
 * （查路径、读文件）不再被互相阻塞
 *
 * @warning
 * 不可重入：持锁路径中的内部互相调用必须走 *_impl 版本，不得调用包装函数
 */
static struct SCHED_RWLOCK ext2_lock;

static int ext2_read_inode_impl(uint32_t ino, struct FS_INODE *out);
static int ext2_write_inode_impl(uint32_t ino, const struct FS_INODE *in);
static int ext2_write_to_inode_impl(struct FS_INODE *ino, uint32_t off,
                                    const void *buf, uint32_t count);
static void ext2_truncate_inode_impl(struct FS_INODE *ino);
static uint32_t ext2_new_inode_impl(uint32_t mode, struct FS_INODE *out);
static int ext2_add_entry_impl(struct FS_INODE *dino, uint32_t ino,
                               const char *name, uint8_t dtype);
static int ext2_remove_entry_impl(struct FS_INODE *dino, const char *name);
static int ext2_read_from_inode_impl(const struct FS_INODE *ino, uint32_t off,
                                     void *buf, uint32_t count);
static int ext2_dir_next_impl(const struct FS_INODE *dino, uint32_t *pos,
                              struct FS_DIRENT *out);
static int ext2_lookup_depth(const char *path, uint32_t *ino, int *ftype,
                             int follow, int depth);
static struct DISK *disk = NULL;
static struct DISK_PARTITION *part = NULL;
static uint32_t start = 0;
static uint32_t bs = 1024;
static uint32_t sect_per_block = 2;
static uint32_t inodes_per_group = 0;
static uint32_t inode_table_blk = 0;
static uint32_t first_block = 0;

static uint32_t block_bitmap_blk = 0;
static uint32_t inode_bitmap_blk = 0;
static uint32_t total_blocks = 0;
static uint32_t data_start = 0;
static uint32_t free_blocks = 0;
static uint32_t free_inodes = 0;

#define EXT2_BCACHE_SLOTS 1024u

static uint8_t *s_bc_data;
static uint32_t s_bc_tag[EXT2_BCACHE_SLOTS];
static uint8_t s_bc_valid[EXT2_BCACHE_SLOTS];
static uint32_t s_bc_slots;

static int ext2_disk_read(uint32_t blk, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    if (blk >= total_blocks && total_blocks != 0) {
        kprintf("[ext2] read out-of-range block %d (total %d)\n", blk,
                total_blocks);
        return -1;
    }
    BLOCK.read_sectors(disk, start + blk * sect_per_block, buf,
                       sect_per_block);
    return 0;
}

static const uint8_t *ext2_bcache_peek(uint32_t blk) {
    if (s_bc_data == NULL) {
        return NULL;
    }
    uint32_t slot = blk % s_bc_slots;
    if (s_bc_valid[slot] && s_bc_tag[slot] == blk) {
        return s_bc_data + slot * bs;
    }
    return NULL;
}

static void ext2_bcache_inval(uint32_t blk) {
    if (s_bc_data == NULL) {
        return;
    }
    uint32_t slot = blk % s_bc_slots;
    if (s_bc_valid[slot] && s_bc_tag[slot] == blk) {
        s_bc_valid[slot] = 0;
    }
}

static const uint8_t *ext2_bcache_get(uint32_t blk) {
    const uint8_t *hit = ext2_bcache_peek(blk);
    if (hit != NULL || s_bc_data == NULL) {
        return hit;
    }
    uint32_t slot = blk % s_bc_slots;
    uint8_t *dst = s_bc_data + slot * bs;
    if (ext2_disk_read(blk, dst) != 0) {
        return NULL;
    }
    s_bc_tag[slot] = blk;
    s_bc_valid[slot] = 1;
    return dst;
}

static void ext2_bcache_store(uint32_t blk, const void *src) {
    if (s_bc_data == NULL) {
        return;
    }
    uint32_t slot = blk % s_bc_slots;
    memcpy(s_bc_data + slot * bs, src, bs);
    s_bc_tag[slot] = blk;
    s_bc_valid[slot] = 1;
}

static int ext2_read_block(uint32_t blk, void *buf) {
    const uint8_t *c = ext2_bcache_get(blk);
    if (c != NULL) {
        memcpy(buf, c, bs);
        return 0;
    }
    return ext2_disk_read(blk, buf);
}

static int ext2_write_block(uint32_t blk, const void *buf) {
    if (disk == NULL) {
        return -1;
    }
    BLOCK.write_sectors(disk, start + blk * sect_per_block, (void *)buf,
                        sect_per_block);
    ext2_bcache_inval(blk);
    return 0;
}

static int ext2_read_blocks(uint32_t blk, uint32_t cnt, void *buf) {
    if (disk == NULL) {
        return -1;
    }
    if (blk >= total_blocks && total_blocks != 0) {
        kprintf("[ext2] read out-of-range block %d (total %d)\n", blk,
                total_blocks);
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

struct DISK_PARTITION *ext2_partition(void) {
    return part;
}

int ext2_init(void) {
    rwlock_init(&ext2_lock);
    struct LIST_ELEM *e = partition_list.head.next;
    while (e != &partition_list.tail) {
        struct DISK_PARTITION *p = list_entry(e, struct DISK_PARTITION, part_tag);
        uint8_t *buf = (uint8_t *)get_kernel_pages(1);
        if (buf == NULL) {
            return -1;
        }
        memset(buf, 0, 4096);
        BLOCK.read_sectors(p->my_disk, p->start_lba, buf, 4);
        struct EXT2_SURPER *sb = (struct EXT2_SURPER *)(buf + 1024);
        if (sb->s_magic == EXT2_SUPER_MAGIC) {
            disk = p->my_disk;
            part = p;
            start = part->start_lba;
            bs = 1024u << sb->s_log_block_size;
            sect_per_block = bs / 512u;
            inodes_per_group = sb->s_inodes_per_group;
            first_block = sb->s_first_data_block;
            total_blocks = sb->s_blocks_count;
            free_blocks = sb->s_free_blocks_count;
            free_inodes = sb->s_free_inodes_count;
            uint8_t *gb = (uint8_t *)get_kernel_pages(1);
            if (gb == NULL) {
                free_kernel_page((uint32_t)buf);
                return -1;
            }
            ext2_read_block(first_block + 1, gb);
            block_bitmap_blk = *(uint32_t *)(gb + 0);
            inode_bitmap_blk = *(uint32_t *)(gb + 4);
            inode_table_blk = *(uint32_t *)(gb + 8);
            uint32_t itable_blocks =
                (inodes_per_group * EXT2_INODE_SIZE + bs - 1) / bs;
            data_start = inode_table_blk + itable_blocks;
            free_kernel_page((uint32_t)gb);
            free_kernel_page((uint32_t)buf);
            if (s_bc_data == NULL) {
                s_bc_slots = EXT2_BCACHE_SLOTS;
                while (s_bc_slots >= 64u) {
                    uint32_t need = DIV_ROUND_UP(s_bc_slots * bs, PAGE_SIZE);
                    s_bc_data = (uint8_t *)get_kernel_pages(need);
                    if (s_bc_data != NULL) {
                        break;
                    }
                    s_bc_slots /= 2u;
                }
                if (s_bc_data == NULL) {
                    kprintf("ext2: block cache disabled (no memory)\n");
                }
            }
            kprintf("ext2 mounted on %s, block_size=%d, inodes_per_group=%d, "
                    "data_start=%d\n",
                    p->name, (int)bs, (int)inodes_per_group, (int)data_start);
            return 0;
        }
        free_kernel_page((uint32_t)buf);
        e = e->next;
    }
    kprintf("ext2: no ext2 filesystem found\n");
    return -1;
}

int ext2_read_inode(uint32_t ino, struct FS_INODE *out) {
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_read_inode_impl(ino, out);
    rwlock_read_release(&ext2_lock);
    return rc;
}

static int ext2_read_inode_impl(uint32_t ino, struct FS_INODE *out) {
    if (disk == NULL || ino == 0) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / EXT2_INODE_SIZE;
    uint32_t blk = inode_table_blk + (ino - 1) / per_block;
    uint32_t off = ((ino - 1) % per_block) * EXT2_INODE_SIZE;
    ext2_read_block(blk, buf);
    uint8_t *p = buf + off;
    memset(out, 0, sizeof(struct FS_INODE));
    out->i_no = ino;
    out->i_mode = (uint32_t)(*(uint16_t *)(p + 0));
    out->i_uid = *(uint16_t *)(p + 2);
    out->i_size = *(uint32_t *)(p + 4);
    out->i_gid = *(uint16_t *)(p + 24);
    out->i_atime = *(uint32_t *)(p + 8);
    out->i_ctime = *(uint32_t *)(p + 12);
    out->i_mtime = *(uint32_t *)(p + 16);
    uint32_t bi = 0;
    for (bi = 0; bi < 15; bi++) {
        out->i_block[bi] = *(uint32_t *)(p + 40 + 4 * bi);
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

static uint32_t ext2_alloc_block(void) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return 0;
    }
    for (uint32_t bb = 0; bb * bs * 8 < total_blocks; bb++) {
        uint32_t bbitblk = block_bitmap_blk + bb;
        memset(buf, 0, 4096);
        ext2_read_block(bbitblk, buf);
        uint32_t base = bb * bs * 8;
        uint32_t lo = data_start > base ? data_start - base : 0;
        uint32_t hi = total_blocks - base;
        if (hi > bs * 8) {
            hi = bs * 8;
        }
        if (lo >= hi) {
            continue;
        }
        uint32_t first = lo >> 3;
        uint32_t last = (hi - 1) >> 3;
        for (uint32_t i = first; i <= last; i++) {
            uint8_t inv = (uint8_t)~buf[i];
            if (i == first && (lo & 7)) {
                inv &= (uint8_t)(0xffu >> (lo & 7));
            }
            if (i == last && (hi & 7)) {
                inv &= (uint8_t)(0xffu << (8 - (hi & 7)));
            }
            if (inv == 0) {
                continue;
            }
            uint32_t bit = (uint32_t)__builtin_clz(inv) - 24;
            buf[i] |= (uint8_t)(0x80u >> bit);
            ext2_write_block(bbitblk, buf);
            free_blocks--;
            free_kernel_page((uint32_t)buf);
            return base + (i << 3) + bit;
        }
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

static int ext2_bitmap_clear(uint32_t blk, uint32_t bit) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    uint32_t byte = bit >> 3;
    uint8_t mask = (uint8_t)(0x80u >> (bit & 7));
    memset(buf, 0, 4096);
    ext2_read_block(blk, buf);
    int was_set = (buf[byte] & mask) != 0;
    if (was_set) {
        buf[byte] &= (uint8_t)~mask;
        ext2_write_block(blk, buf);
    }
    free_kernel_page((uint32_t)buf);
    return was_set ? 0 : 1;
}

static void ext2_free_block(uint32_t blk) {
    if (blk == 0 || blk >= total_blocks) {
        return;
    }
    uint32_t per = bs * 8;
    if (ext2_bitmap_clear(block_bitmap_blk + blk / per, blk % per) == 0) {
        free_blocks++;
    }
}

static uint32_t ext2_alloc_inode(void) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return 0;
    }
    memset(buf, 0, 4096);
    ext2_read_block(inode_bitmap_blk, buf);
    for (uint32_t i = 0; i < bs; i++) {
        uint8_t inv = (uint8_t)~buf[i];
        if (inv == 0) {
            continue;
        }
        uint32_t bit = (uint32_t)__builtin_clz(inv) - 24;
        uint32_t idx = (i << 3) + bit + 1;
        if (idx > inodes_per_group) {
            break;
        }
        buf[i] |= (uint8_t)(0x80u >> bit);
        ext2_write_block(inode_bitmap_blk, buf);
        free_inodes--;
        free_kernel_page((uint32_t)buf);
        return idx;
    }
    free_kernel_page((uint32_t)buf);
    return 0;
}

static void ext2_free_inode_impl(uint32_t ino) {
    if (ino == 0 || ino > inodes_per_group) {
        return;
    }
    if (ext2_bitmap_clear(inode_bitmap_blk, ino - 1) == 0) {
        free_inodes++;
    }
}

void ext2_free_inode(uint32_t ino) {
    rwlock_write_acquire(&ext2_lock);
    ext2_free_inode_impl(ino);
    rwlock_write_release(&ext2_lock);
}

int ext2_write_inode(uint32_t ino, const struct FS_INODE *in) {
    rwlock_write_acquire(&ext2_lock);
    int rc = ext2_write_inode_impl(ino, in);
    rwlock_write_release(&ext2_lock);
    return rc;
}

static int ext2_write_inode_impl(uint32_t ino, const struct FS_INODE *in) {
    if (disk == NULL || ino == 0 || ino > inodes_per_group) {
        return -1;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, 4096);
    uint32_t per_block = bs / EXT2_INODE_SIZE;
    uint32_t blk = inode_table_blk + (ino - 1) / per_block;
    uint32_t off = ((ino - 1) % per_block) * EXT2_INODE_SIZE;
    ext2_read_block(blk, buf);
    uint8_t *p = buf + off;
    *(uint16_t *)(p + 0) = (uint16_t)in->i_mode;
    *(uint16_t *)(p + 2) = (uint16_t)in->i_uid;
    *(uint16_t *)(p + 24) = (uint16_t)in->i_gid;
    *(uint32_t *)(p + 4) = in->i_size;
    *(uint32_t *)(p + 8) = in->i_atime;
    *(uint32_t *)(p + 12) = in->i_ctime;
    *(uint32_t *)(p + 16) = in->i_mtime;
    for (uint32_t bi = 0; bi < 15; bi++) {
        *(uint32_t *)(p + 40 + 4 * bi) = in->i_block[bi];
    }
    ext2_write_block(blk, buf);
    free_kernel_page((uint32_t)buf);
    return 0;
}

static uint32_t ext2_walk_cached(uint32_t root, uint32_t fblk, uint32_t span) {
    const uint8_t *p = ext2_bcache_get(root);
    if (p == NULL) {
        return 0;
    }
    uint32_t child = *(const uint32_t *)(p + 4 * (fblk / span));
    if (child == 0) {
        return 0;
    }
    if (span == 1) {
        return child;
    }
    return ext2_walk_cached(child, fblk % span, span / (bs / 4u));
}

static uint32_t ext2_walk(uint32_t root, uint32_t fblk, uint32_t span,
                          uint8_t *buf, int alloc) {
    memset(buf, 0, 4096);
    ext2_read_block(root, buf);
    uint32_t idx = fblk / span;
    uint32_t child = *(uint32_t *)(buf + 4 * idx);
    if (child == 0) {
        if (!alloc) {
            return 0;
        }
        child = ext2_alloc_block();
        if (child == 0) {
            return 0;
        }
        *(uint32_t *)(buf + 4 * idx) = child;
        ext2_write_block(root, buf);
        if (span > 1) {
            memset(buf, 0, 4096);
            ext2_write_block(child, buf);
        }
    }
    if (span == 1) {
        return child;
    }
    return ext2_walk(child, fblk % span, span / (bs / 4u), buf, alloc);
}

static int ext2_block_of(struct FS_INODE *ino, uint32_t fblk, int alloc,
                         uint32_t *out) {
    uint32_t addrs = bs / 4u;
    if (fblk >= 12 + addrs + addrs * addrs) {
        return -1;
    }
    if (fblk < 12) {
        uint32_t b = ino->i_block[fblk];
        if (b == 0 && alloc) {
            b = ext2_alloc_block();
            ino->i_block[fblk] = b;
        }
        if (b == 0) {
            return -1;
        }
        *out = b;
        return 0;
    }
    fblk -= 12;
    uint32_t slot = 12;
    uint32_t span = 1;
    if (fblk >= addrs) {
        fblk -= addrs;
        slot = 13;
        span = addrs;
    }
    uint32_t root = ino->i_block[slot];
    int fresh = 0;
    if (root == 0 && alloc) {
        root = ext2_alloc_block();
        ino->i_block[slot] = root;
        fresh = 1;
    }
    if (root == 0) {
        return -1;
    }
    if (!alloc) {
        uint32_t b = ext2_walk_cached(root, fblk, span);
        if (b == 0) {
            return -1;
        }
        *out = b;
        return 0;
    }
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return -1;
    }
    if (fresh) {
        memset(buf, 0, 4096);
        ext2_write_block(root, buf);
    }
    uint32_t b = ext2_walk(root, fblk, span, buf, alloc);
    free_kernel_page((uint32_t)buf);
    if (b == 0) {
        return -1;
    }
    *out = b;
    return 0;
}

static uint32_t ext2_ensure_block(struct FS_INODE *ino, uint32_t fblk) {
    uint32_t b = 0;
    ext2_block_of(ino, fblk, 1, &b);
    return b;
}

int ext2_write_to_inode(struct FS_INODE *ino, uint32_t off, const void *buf,
                        uint32_t count) {
    rwlock_write_acquire(&ext2_lock);
    int rc = ext2_write_to_inode_impl(ino, off, buf, count);
    rwlock_write_release(&ext2_lock);
    return rc;
}

static int ext2_write_to_inode_impl(struct FS_INODE *ino, uint32_t off,
                                    const void *buf, uint32_t count) {
    if (disk == NULL || ino == 0 || ino->i_no == 0) {
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
        uint32_t addr = ext2_ensure_block((struct FS_INODE *)ino, fblk);
        if (addr == 0) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t chunk = bs - within;
        if (chunk > count - done) {
            chunk = count - done;
        }
        memcpy(blk + within, (const uint8_t *)buf + done, chunk);
        ext2_write_block(addr, blk);
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
        ext2_write_inode_impl(ino->i_no, (struct FS_INODE *)ino);
    }
    return (int)done;
}

void ext2_truncate_inode(struct FS_INODE *ino) {
    rwlock_write_acquire(&ext2_lock);
    ext2_truncate_inode_impl(ino);
    rwlock_write_release(&ext2_lock);
}

static void ext2_free_tree(uint32_t root, uint32_t span, uint8_t *buf) {
    memset(buf, 0, 4096);
    ext2_read_block(root, buf);
    for (uint32_t i = 0; i < bs / 4u; i++) {
        uint32_t child = *(uint32_t *)(buf + 4 * i);
        if (child == 0) {
            continue;
        }
        if (span == 1) {
            ext2_free_block(child);
        } else {
            ext2_free_tree(child, span / (bs / 4u), buf);
        }
    }
    ext2_free_block(root);
}

static void ext2_truncate_inode_impl(struct FS_INODE *ino) {
    uint8_t *buf = (uint8_t *)get_kernel_pages(1);
    if (buf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 12; i++) {
        if (ino->i_block[i]) {
            ext2_free_block(ino->i_block[i]);
            ino->i_block[i] = 0;
        }
    }
    if (ino->i_block[12]) {
        ext2_free_tree(ino->i_block[12], 1, buf);
        ino->i_block[12] = 0;
    }
    if (ino->i_block[13]) {
        ext2_free_tree(ino->i_block[13], bs / 4u, buf);
        ino->i_block[13] = 0;
    }
    if (ino->i_block[14]) {
        ext2_free_block(ino->i_block[14]);
        ino->i_block[14] = 0;
    }
    free_kernel_page((uint32_t)buf);
    memset(ino->i_block, 0, sizeof(ino->i_block));
    ino->i_size = 0;
}

static int ext2_map_block(const struct FS_INODE *ino, uint32_t fblk,
                          uint32_t *out);

int ext2_new_inode(uint32_t mode, struct FS_INODE *out) {
    rwlock_write_acquire(&ext2_lock);
    uint32_t rc = ext2_new_inode_impl(mode, out);
    rwlock_write_release(&ext2_lock);
    return rc;
}

static uint32_t ext2_new_inode_impl(uint32_t mode, struct FS_INODE *out) {
    uint32_t ino = ext2_alloc_inode();
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
    memset(out->i_block, 0, sizeof(out->i_block));
    if (ext2_write_inode_impl(ino, out)) {
        ext2_free_inode_impl(ino);
        return 0;
    }
    return (int)ino;
}

int ext2_add_entry(struct FS_INODE *dino, uint32_t ino, const char *name,
                   int is_dir) {
    return ext2_add_entry_dt(dino, ino, name,
                             is_dir ? (uint8_t)EXT2_DT_DIR : 1u);
}

int ext2_add_entry_dt(struct FS_INODE *dino, uint32_t ino, const char *name,
                      uint8_t dtype) {
    rwlock_write_acquire(&ext2_lock);
    int rc = ext2_add_entry_impl(dino, ino, name, dtype);
    rwlock_write_release(&ext2_lock);
    return rc;
}

static int ext2_add_entry_impl(struct FS_INODE *dino, uint32_t ino,
                               const char *name, uint8_t dtype) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= 255u) {
        return -1;
    }
    uint32_t need = (8u + nl + 3u) & ~3u;
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }

    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext2_map_block((struct FS_INODE *)dino, fblk, &addr)) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        uint32_t target = 0xFFFFFFFFu;
        uint32_t slot_rec = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u) {
                if (bs - off >= need) {
                    target = off;
                    slot_rec = bs - off;
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
                    struct EXT2_DIRENT *nd =
                        (struct EXT2_DIRENT *)(blk + off + used);
                    nd->inode = ino;
                    nd->rec_len = (uint16_t)(rl - used);
                    nd->name_len = (uint8_t)nl;
                    nd->file_type = dtype;
                    memcpy(nd->name, name, nl);
                    ext2_write_block(addr, blk);
                    free_kernel_page((uint32_t)blk);
                    return 0;
                }
            }
            off += rl;
        }
        if (target != 0xFFFFFFFFu) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + target);
            de->inode = ino;
            de->rec_len = (uint16_t)slot_rec;
            de->name_len = (uint8_t)nl;
            de->file_type = dtype;
            memcpy(de->name, name, nl);
            ext2_write_block(addr, blk);
            free_kernel_page((uint32_t)blk);
            return 0;
        }
    }

    uint32_t nfblk = dino->i_size / bs;
    uint32_t naddr = ext2_ensure_block((struct FS_INODE *)dino, nfblk);
    if (naddr == 0) {
        free_kernel_page((uint32_t)blk);
        return -1;
    }
    memset(blk, 0, 4096);
    struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)blk;
    de->inode = ino;
    de->rec_len = (uint16_t)bs;
    de->name_len = (uint8_t)nl;
    de->file_type = dtype;
    memcpy(de->name, name, nl);
    ext2_write_block(naddr, blk);
    dino->i_size += bs;
    ext2_write_inode_impl(dino->i_no, (struct FS_INODE *)dino);
    free_kernel_page((uint32_t)blk);
    return 0;
}

int ext2_remove_entry(struct FS_INODE *dino, const char *name) {
    rwlock_write_acquire(&ext2_lock);
    int rc = ext2_remove_entry_impl(dino, name);
    rwlock_write_release(&ext2_lock);
    return rc;
}

static int ext2_remove_entry_impl(struct FS_INODE *dino, const char *name) {
    uint32_t nl = (uint32_t)strlen(name);
    if (nl == 0 || nl >= 255u) {
        return -1;
    }
    uint8_t *blk = (uint8_t *)get_kernel_pages(1);
    if (blk == NULL) {
        return -1;
    }
    for (uint32_t fblk = 0; fblk * bs < dino->i_size; fblk++) {
        uint32_t addr = 0;
        if (ext2_map_block((struct FS_INODE *)dino, fblk, &addr)) {
            break;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rl = de->rec_len;
            if (rl < 8u) {
                break;
            }
            if (de->inode != 0 && de->name_len == nl &&
                memcmp(de->name, name, nl) == 0) {
                de->inode = 0;
                de->name_len = 0;
                ext2_write_block(addr, blk);
                free_kernel_page((uint32_t)blk);
                return 0;
            }
            off += rl;
        }
        free_kernel_page((uint32_t)blk);
    }
    free_kernel_page((uint32_t)blk);
    return -1;
}

static int ext2_map_block(const struct FS_INODE *ino, uint32_t fblk,
                          uint32_t *out) {
    return ext2_block_of((struct FS_INODE *)ino, fblk, 0, out);
}

int ext2_read_from_inode(const struct FS_INODE *ino, uint32_t off, void *buf,
                         uint32_t count) {
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_read_from_inode_impl(ino, off, buf, count);
    rwlock_read_release(&ext2_lock);
    return rc;
}

#define EXT2_READ_BLOCKS 32u
#define EXT2_STREAM_MIN 65536u
#define EXT2_STREAM_BLOCKS 128u

static int ext2_read_from_inode_impl(const struct FS_INODE *ino, uint32_t off,
                                     void *buf, uint32_t count) {
    uint32_t done = 0;

    if (ino->i_no == 0 || off >= ino->i_size) {
        return 0;
    }
    if (off + count > ino->i_size) {
        count = ino->i_size - off;
    }
    if (count >= EXT2_STREAM_MIN && (off % bs) == 0) {
        uint32_t limit = count - (count % bs);
        while (done < limit) {
            uint32_t fblk = (off + done) / bs;
            uint32_t maxb = (limit - done) / bs;
            uint32_t addr = 0;
            uint32_t run = 1;
            if (maxb > EXT2_STREAM_BLOCKS) {
                maxb = EXT2_STREAM_BLOCKS;
            }
            if (ext2_map_block(ino, fblk, &addr)) {
                break;
            }
            while (run < maxb) {
                uint32_t next = 0;
                if (ext2_map_block(ino, fblk + run, &next)) {
                    break;
                }
                if (next != addr + run) {
                    break;
                }
                run++;
            }
            ext2_read_blocks(addr, run, (uint8_t *)buf + done);
            done += run * bs;
        }
        if (done >= count) {
            return (int)done;
        }
    }
    uint32_t pages = DIV_ROUND_UP(EXT2_READ_BLOCKS * bs, PAGE_SIZE);
    uint8_t *blk = (uint8_t *)get_kernel_pages(pages);
    if (blk == NULL) {
        return (int)done;
    }
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t fblk = pos / bs;
        uint32_t within = pos % bs;
        uint32_t addr = 0;
        if (ext2_map_block(ino, fblk, &addr)) {
            break;
        }
        uint32_t run = 1;
        while (run < EXT2_READ_BLOCKS && within + run * bs < count) {
            uint32_t next = 0;
            if (ext2_map_block(ino, fblk + run, &next)) {
                break;
            }
            if (next != addr + run) {
                break;
            }
            run++;
        }
        uint32_t cached = 1;
        for (uint32_t i = 0; i < run; i++) {
            if (ext2_bcache_peek(addr + i) == NULL) {
                cached = 0;
                break;
            }
        }
        if (cached) {
            for (uint32_t i = 0; i < run; i++) {
                memcpy(blk + i * bs, ext2_bcache_peek(addr + i), bs);
            }
        } else {
            ext2_read_blocks(addr, run, blk);
            for (uint32_t i = 0; i < run; i++) {
                ext2_bcache_store(addr + i, blk + i * bs);
            }
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

int ext2_dir_next(const struct FS_INODE *dino, uint32_t *pos,
                  struct FS_DIRENT *out) {
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_dir_next_impl(dino, pos, out);
    rwlock_read_release(&ext2_lock);
    return rc;
}

static int ext2_dir_next_impl(const struct FS_INODE *dino, uint32_t *pos,
                              struct FS_DIRENT *out) {
    while (*pos < dino->i_size) {
        uint32_t fblk = *pos / bs;
        uint32_t addr = 0;
        if (ext2_map_block(dino, fblk, &addr)) {
            *pos = (fblk + 1) * bs;
            continue;
        }
        uint8_t *blk = (uint8_t *)get_kernel_pages(1);
        if (blk == NULL) {
            return -1;
        }
        memset(blk, 0, 4096);
        ext2_read_block(addr, blk);
        uint32_t off = 0;
        while (off < bs) {
            struct EXT2_DIRENT *de = (struct EXT2_DIRENT *)(blk + off);
            uint32_t rec_len = de->rec_len;
            if (rec_len < 8) {
                break;
            }
            uint32_t abs = fblk * bs + off;
            if (de->inode != 0 && de->name_len > 0 && de->name_len < 255 &&
                abs >= *pos) {
                uint32_t nl = de->name_len;
                if (nl >= MAX_FILE_NAME_LEN) {
                    nl = MAX_FILE_NAME_LEN - 1;
                }
                memset(out->filename, 0, MAX_FILE_NAME_LEN);
                memcpy(out->filename, de->name, nl);
                out->i_no = de->inode;
                out->f_type = de->file_type == EXT2_DT_DIR  ? FT_DIRECTORY
                              : de->file_type == 2u         ? FT_CHARDEVICE
                              : de->file_type == 7u         ? FT_SYMLINK
                                                            : FT_REGULAR;
                *pos = abs + rec_len;
                free_kernel_page((uint32_t)blk);
                return 0;
            }
            off += rec_len;
        }
        free_kernel_page((uint32_t)blk);
        *pos = (fblk + 1) * bs;
    }
    return -1;
}

static int ext2_find_in_dir(const struct FS_INODE *dino, const char *name,
                            uint32_t *child, int *ftype) {
    uint32_t pos = 0;
    struct FS_DIRENT de;
    while (ext2_dir_next_impl(dino, &pos, &de) == 0) {
        if (strcmp(de.filename, name) == 0) {
            *child = de.i_no;
            *ftype = (int)de.f_type;
            return 0;
        }
    }
    return -1;
}

static int ext2_read_target(uint32_t ino, char *buf, uint32_t cap) {
    struct FS_INODE node;
    if (ext2_read_inode_impl(ino, &node) ||
        (node.i_mode & 0xF000u) != 0xA000u) {
        return -1;
    }
    uint32_t len = node.i_size < cap - 1 ? node.i_size : cap - 1;
    if (node.i_size < 60u) {
        memcpy(buf, &node.i_block[0], len);
    } else if (ext2_read_from_inode_impl(&node, 0, buf, len) != (int)len) {
        return -1;
    }
    buf[len] = 0;
    return (int)len;
}

int ext2_abs_path(const char *path, char *out, uint32_t cap) {
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

int ext2_lookup(const char *path, uint32_t *ino, int *is_dir) {
    char abs[MAX_PATH_LEN];
    if (path != NULL && path[0] != '/') {
        if (ext2_abs_path(path, abs, sizeof(abs)) != 0) {
            return -1;
        }
        path = abs;
    }
    int ft = 0;
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_lookup_depth(path, ino, &ft, 1, 8);
    rwlock_read_release(&ext2_lock);
    *is_dir = (ft == FT_DIRECTORY);
    return rc;
}

int ext2_lookup_ftype(const char *path, uint32_t *ino, int *ftype, int follow) {
    char abs[MAX_PATH_LEN];
    if (path != NULL && path[0] != '/') {
        if (ext2_abs_path(path, abs, sizeof(abs)) != 0) {
            return -1;
        }
        path = abs;
    }
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_lookup_depth(path, ino, ftype, follow, 8);
    rwlock_read_release(&ext2_lock);
    return rc;
}

int ext2_read_link_target(uint32_t ino, char *buf, uint32_t cap) {
    rwlock_read_acquire(&ext2_lock);
    int rc = ext2_read_target(ino, buf, cap);
    rwlock_read_release(&ext2_lock);
    return rc;
}

static int ext2_lookup_depth(const char *path, uint32_t *ino, int *ftype,
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
        if (ext2_read_inode_impl(2, &node)) {
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
            if (ext2_find_in_dir(&node, comp, &child, &ft)) {
                return -1;
            }
            if (ft == FT_SYMLINK) {
                char tgt[MAX_PATH_LEN];
                if (!follow || ext2_read_target(child, tgt, MAX_PATH_LEN) < 0) {
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
            if (!cdir || ext2_read_inode_impl(child, &node)) {
                return -1;
            }
        }
        *ino = cino;
        *ftype = cdir ? FT_DIRECTORY : FT_REGULAR;
    }
}

void ext2_statfs_info(uint32_t *bsize, uint32_t *blocks, uint32_t *bfree,
                      uint32_t *files, uint32_t *ffree) {
    rwlock_read_acquire(&ext2_lock);
    if (bsize) *bsize = bs;
    if (blocks) *blocks = total_blocks;
    if (bfree) *bfree = free_blocks;
    if (files) *files = inodes_per_group;
    if (ffree) *ffree = free_inodes;
    rwlock_read_release(&ext2_lock);
}
