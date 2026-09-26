/*
 * pagecache.c — the block-device page cache. (GPLv2)
 *
 * Every byte the filesystems read from a block device used to go straight
 * to the disk, twice if the filesystem asked for the same block twice.
 * This layer sits between them: 4 KiB pages keyed by (device, byte
 * offset >> 12), kept in a fixed pool of frames and recycled least-
 * recently-used first.
 *
 * Writes are WRITE-THROUGH: the device is updated immediately and a
 * cached page is refreshed in place.  That is deliberately weaker than a
 * real writeback cache (there is no flusher thread and no dirty list to
 * order), but it can never lose a write the caller already saw succeed.
 */
#include <stdint.h>

#include "pagecache.h"
#include "pmm.h"
#include "kstring.h"
#include "debugcon.h"

#define PC_PAGE      4096
#define PC_PAGES     4096                  /* 16 MiB of cached device data */
#define PC_BUCKETS   1024

typedef struct pc_page {
    struct pc_page *next;                  /* hash chain                  */
    struct pc_page *lru_prev, *lru_next;   /* recency list                */
    void           *ctx;                   /* the device it belongs to    */
    uint64_t        off;                   /* device offset, page-aligned */
    uint64_t        frame;                 /* physical frame              */
    uint8_t        *data;                  /* kernel-view pointer         */
} pc_page_t;

static pc_page_t  g_pool[PC_PAGES];
static unsigned   g_used;
static pc_page_t *g_bucket[PC_BUCKETS];
static pc_page_t *g_lru_head, *g_lru_tail;
static uint32_t   g_hits, g_misses, g_evicts;

extern uint64_t g_hhdm;

static unsigned bucket_of(void *ctx, uint64_t off)
{
    uint64_t h = (uint64_t)(uintptr_t)ctx ^ (off >> 12);
    h *= 0x9E3779B97F4A7C15ULL;
    return (unsigned)((h >> 32) & (PC_BUCKETS - 1));
}

/* Move `p` to the most-recently-used end of the list. */
static void lru_touch(pc_page_t *p)
{
    if (g_lru_tail == p)
        return;
    if (p->lru_prev)
        p->lru_prev->lru_next = p->lru_next;
    else if (g_lru_head == p)
        g_lru_head = p->lru_next;
    if (p->lru_next)
        p->lru_next->lru_prev = p->lru_prev;
    p->lru_prev = g_lru_tail;
    p->lru_next = NULL;
    if (g_lru_tail)
        g_lru_tail->lru_next = p;
    g_lru_tail = p;
    if (!g_lru_head)
        g_lru_head = p;
}

static void lru_unlink(pc_page_t *p)
{
    if (p->lru_prev)
        p->lru_prev->lru_next = p->lru_next;
    if (p->lru_next)
        p->lru_next->lru_prev = p->lru_prev;
    if (g_lru_head == p)
        g_lru_head = p->lru_next;
    if (g_lru_tail == p)
        g_lru_tail = p->lru_prev;
    p->lru_prev = p->lru_next = NULL;
}

static void hash_insert(pc_page_t *p)
{
    unsigned b = bucket_of(p->ctx, p->off);
    p->next = g_bucket[b];
    g_bucket[b] = p;
}

static void hash_remove(pc_page_t *p)
{
    unsigned b = bucket_of(p->ctx, p->off);
    pc_page_t **pp = &g_bucket[b];
    while (*pp && *pp != p)
        pp = &(*pp)->next;
    if (*pp == p)
        *pp = p->next;
}

static pc_page_t *hash_find(void *ctx, uint64_t off)
{
    for (pc_page_t *p = g_bucket[bucket_of(ctx, off)]; p; p = p->next)
        if (p->ctx == ctx && p->off == off)
            return p;
    return NULL;
}

/* Take the least recently used page: its frame becomes the new page's. */
static pc_page_t *page_alloc(void *ctx, uint64_t off)
{
    pc_page_t *p;
    if (g_used < PC_PAGES) {
        p = &g_pool[g_used++];
        p->frame = pmm_alloc_zeroed();
        if (!p->frame)
            return NULL;
        p->data = (uint8_t *)(uintptr_t)(p->frame + g_hhdm);
    } else {
        p = g_lru_head;
        if (!p)
            return NULL;
        g_evicts++;
        hash_remove(p);
        lru_unlink(p);
    }
    p->ctx = ctx;
    p->off = off;
    p->next = NULL;
    hash_insert(p);
    lru_touch(p);
    return p;
}

void pagecache_init(void)
{
    memset(g_bucket, 0, sizeof g_bucket);
    g_used = g_hits = g_misses = g_evicts = 0;
    g_lru_head = g_lru_tail = NULL;
}

int pagecache_read(void *ctx, pc_dev_read_t rd, uint64_t off, void *buf,
                   uint32_t len)
{
    if (!ctx || !rd)
        return -1;
    uint32_t done = 0;
    while (done < len) {
        uint64_t poff = (off + done) & ~(uint64_t)(PC_PAGE - 1);
        uint32_t in_page = (uint32_t)((off + done) - poff);
        uint32_t n = len - done;
        if (n > PC_PAGE - in_page)
            n = PC_PAGE - in_page;

        pc_page_t *p = hash_find(ctx, poff);
        if (p) {
            g_hits++;
            lru_touch(p);
        } else {
            g_misses++;
            p = page_alloc(ctx, poff);
            if (!p)
                return -1;
            if (rd(ctx, poff, p->data, PC_PAGE) < 0) {
                hash_remove(p);
                lru_unlink(p);
                return -1;
            }
        }
        memcpy((uint8_t *)buf + done, p->data + in_page, n);
        done += n;
    }
    return (int)done;
}

int pagecache_write(void *ctx, pc_dev_read_t rd, pc_dev_write_t wr,
                    uint64_t off, const void *buf, uint32_t len)
{
    if (!ctx || !wr)
        return -1;
    /* Write-through: the device sees it before we answer, so a crash
     * cannot lose a write the caller was told had succeeded. */
    if (wr(ctx, off, buf, len) < 0)
        return -1;

    /* Refresh whichever cached pages overlap, so the next read is served
     * from memory rather than from the disk we just wrote. */
    uint32_t done = 0;
    while (done < len) {
        uint64_t poff = (off + done) & ~(uint64_t)(PC_PAGE - 1);
        uint32_t in_page = (uint32_t)((off + done) - poff);
        uint32_t n = len - done;
        if (n > PC_PAGE - in_page)
            n = PC_PAGE - in_page;
        pc_page_t *p = hash_find(ctx, poff);
        if (p) {
            memcpy(p->data + in_page, (const uint8_t *)buf + done, n);
            lru_touch(p);
        }
        done += n;
    }
    (void)rd;
    return (int)done;
}

void pagecache_invalidate(void *ctx)
{
    for (unsigned b = 0; b < PC_BUCKETS; b++) {
        pc_page_t *p = g_bucket[b];
        while (p) {
            pc_page_t *next = p->next;
            if (p->ctx == ctx) {
                hash_remove(p);
                lru_unlink(p);
                p->ctx = NULL;
            }
            p = next;
        }
    }
}

void pagecache_stats(uint32_t *hits, uint32_t *misses, uint32_t *evicts,
                     uint32_t *pages)
{
    if (hits)  *hits = g_hits;
    if (misses) *misses = g_misses;
    if (evicts) *evicts = g_evicts;
    if (pages) *pages = g_used;
}

/* A driver with no backing store: every "device" access is a memcpy into
 * a kernel buffer, which is enough to exercise hits, misses and the LRU. */
static uint8_t  g_fake_dev[PC_PAGE * 8];
static uint32_t g_fake_reads;

static int fake_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    g_fake_reads++;
    memcpy(buf, g_fake_dev + off, len);
    return (int)len;
}

void pagecache_selftest(void)
{
    uint32_t h0, m0, e0, p0;
    pagecache_init();
    pagecache_stats(&h0, &m0, &e0, &p0);

    void *ctx = (void *)0x1234;
    uint8_t out[PC_PAGE];

    /* First read of a page must go to the device; the second must not. */
    uint32_t before = g_fake_reads;
    if (pagecache_read(ctx, fake_read, 0, out, PC_PAGE) != PC_PAGE ||
        pagecache_read(ctx, fake_read, 0, out, PC_PAGE) != PC_PAGE ||
        g_fake_reads != before + 1) {
        dbg_puts("PCACHE: FAIL (repeat read did not hit the cache)\r\n");
        return;
    }
    /* A read spanning two pages is served as two pages: the first is
     * already cached from the step above, so only the second is a fetch. */
    before = g_fake_reads;
    if (pagecache_read(ctx, fake_read, PC_PAGE - 8, out, 16) != 16 ||
        g_fake_reads != before + 1) {
        dbg_puts("PCACHE: FAIL (spanning read)\r\n");
        return;
    }
    if (pagecache_read(ctx, fake_read, PC_PAGE - 8, out, 16) != 16 ||
        g_fake_reads != before + 1) {
        dbg_puts("PCACHE: FAIL (repeat spanning read re-fetched)\r\n");
        return;
    }
    /* Fill the pool and check that recycling happens rather than failing.
     * The distinct keys come from the DEVICE, not from the offset: the fake
     * device is only eight pages long, and walking offsets past its end
     * would fault inside memcpy instead of testing anything. */
    for (unsigned i = 0; i < PC_PAGES + 64; i++) {
        void *c = (void *)(uintptr_t)(0x1000u + i * 0x10u);
        if (pagecache_read(c, fake_read, 0, out, 16) != 16) {
            dbg_puts("PCACHE: FAIL (allocation under pressure)\r\n");
            return;
        }
    }
    uint32_t hits, misses, evicts, pages;
    pagecache_stats(&hits, &misses, &evicts, &pages);
    if (!hits || !misses || !evicts || pages != PC_PAGES) {
        dbg_puts("PCACHE: FAIL (counters)\r\n");
        return;
    }
    dbg_puts("PCACHE: self-test PASS, ");
    dbg_puts_dec(pages);
    dbg_puts(" pages, ");
    dbg_puts_dec(hits);
    dbg_puts(" hits / ");
    dbg_puts_dec(misses);
    dbg_puts(" misses / ");
    dbg_puts_dec(evicts);
    dbg_puts(" evictions\r\n");
    pagecache_init();
}
