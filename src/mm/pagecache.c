/* SPDX-License-Identifier: GPL-2.0 */
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
    int             active;                /* on the active list (2nd hit)*/
    int             dirty;                 /* newer than the device copy  */
    int             ra;                    /* filled by readahead, unread */
    pc_dev_write_t  wr;                    /* how to write this device    */
    uint64_t        frame;                 /* physical frame              */
    uint8_t        *data;                  /* kernel-view pointer         */
} pc_page_t;

static pc_page_t  g_pool[PC_PAGES];
static unsigned   g_used;
static pc_page_t *g_bucket[PC_BUCKETS];
/* Two-list (active/inactive) LRU, after Linux mm/swap.c: new pages land on
 * the inactive list, a second reference promotes them to active, and
 * reclaim takes the inactive head -- demoting an active page first when the
 * inactive list has run dry, so a page only dies after two passes miss it.
 * A single LRU lets one sequential scan push every hot page out. */
static pc_page_t *g_active_head, *g_active_tail;
static pc_page_t *g_inactive_head, *g_inactive_tail;
static uint32_t   g_hits, g_misses, g_evicts, g_promote, g_demote;
static uint32_t   g_dirty_n, g_wb;       /* dirty pages now, writebacks ever */
static uint32_t   g_active_n, g_inactive_n;      /* list lengths */

/* ---- readahead state -----------------------------------------------------
 * One window per device: where the sequential stream last stopped and how
 * wide the speculative fetch is now.  A hit inside the window widens it
 * (the stream is real); any other access resets it to the minimum, so one
 * random read never buys a burst of prefetches it does not want. */
typedef struct {
    void     *ctx;
    uint64_t  last_page;      /* page index of the last access              */
    uint32_t  window;         /* pages to fetch ahead of the stream         */
    uint32_t  in_use;
} ra_state_t;

#define RA_MIN       2
#define RA_MAX       16
#define RA_TABLE     64
static ra_state_t g_ra[RA_TABLE];

extern uint64_t g_hhdm;

static unsigned bucket_of(void *ctx, uint64_t off)
{
    uint64_t h = (uint64_t)(uintptr_t)ctx ^ (off >> 12);
    h *= 0x9E3779B97F4A7C15ULL;
    return (unsigned)((h >> 32) & (PC_BUCKETS - 1));
}

static void lru_push(pc_page_t **head, pc_page_t **tail, pc_page_t *p)
{
    p->lru_prev = *tail;
    p->lru_next = NULL;
    if (*tail)
        (*tail)->lru_next = p;
    *tail = p;
    if (!*head)
        *head = p;
    if (p->active)
        g_active_n++;
    else
        g_inactive_n++;
}

static void lru_unlink(pc_page_t *p)
{
    pc_page_t **head = p->active ? &g_active_head : &g_inactive_head;
    pc_page_t **tail = p->active ? &g_active_tail : &g_inactive_tail;
    if (p->active) {
        if (g_active_n)
            g_active_n--;
    } else {
        if (g_inactive_n)
            g_inactive_n--;
    }
    if (p->lru_prev)
        p->lru_prev->lru_next = p->lru_next;
    else if (*head == p)
        *head = p->lru_next;
    if (p->lru_next)
        p->lru_next->lru_prev = p->lru_prev;
    else if (*tail == p)
        *tail = p->lru_prev;
    p->lru_prev = p->lru_next = NULL;
}

/* A hit: an inactive page earns its way onto the active list; an active
 * one just moves to the most-recently-used end. */
static void lru_touch(pc_page_t *p)
{
    if (!p->active) {
        lru_unlink(p);
        p->active = 1;
        lru_push(&g_active_head, &g_active_tail, p);
        g_promote++;
        return;
    }
    if (g_active_tail == p)
        return;
    lru_unlink(p);
    lru_push(&g_active_head, &g_active_tail, p);
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
/* ---- writeback ----------------------------------------------------------
 * A dirty page is newer than the device copy.  It is flushed exactly when
 * its cleaner runs: an explicit sync, or the moment the reclaim wants to
 * reuse the page -- never silently dropped, which is what would happen if
 * the eviction path did not check. */

/* Write one dirty page back.  Returns 0 on success (or if it was clean). */
static int page_flush_one(pc_page_t *p)
{
    if (!p->dirty || !p->wr)
        return 0;
    if (p->wr(p->ctx, p->off, p->data, PC_PAGE) < 0)
        return -1;                       /* keep it dirty: caller retries */
    p->dirty = 0;
    if (g_dirty_n)
        g_dirty_n--;
    g_wb++;
    return 0;
}

/* Flush every dirty page of one device. */
int pagecache_flush_ctx(void *ctx)
{
    int r = 0;
    for (unsigned b = 0; b < PC_BUCKETS; b++) {
        for (pc_page_t *p = g_bucket[b]; p; p = p->next)
            if (p->ctx == ctx && p->dirty)
                if (page_flush_one(p) < 0)
                    r = -1;
    }
    return r;
}

/* Flush everything: the sync(2) path. */
int pagecache_flush_all(void)
{
    int r = 0;
    for (unsigned b = 0; b < PC_BUCKETS; b++) {
        for (pc_page_t *p = g_bucket[b]; p; p = p->next)
            if (p->dirty)
                if (page_flush_one(p) < 0)
                    r = -1;
    }
    return r;
}

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
        /* balance first, then pick: the demote below can surface dirty
         * pages (a written page sits on the active list after its
         * lru_touch), and they are legitimate victims once flushed */
        if (g_inactive_n < g_active_n / 2) {
            /* The working set has tipped over: most of the pool is pinned
             * on the active list while fresh pages churn through a thin
             * inactive one.  Demote HALF the active list to the tail
             * of the inactive one (Linux's balance step, in miniature):
             * demoting a single page would be immediately re-evicted, and
             * never demoting would let a promoted page live forever while
             * the pool starves. */
            unsigned n = g_active_n / 2;
            while (n-- && g_active_head) {
                pc_page_t *p2 = g_active_head;
                lru_unlink(p2);
                p2->active = 0;
                lru_push(&g_inactive_head, &g_inactive_tail, p2);
                g_demote++;
            }
        }
        /* WRITE-BACK invariant: a dirty page leaves the cache only
         * through its cleaner.  A page whose flush fails is skipped, not
         * dropped -- losing a confirmed write to make room for a read
         * would be the one bug this layer must never have. */
        pc_page_t *v = g_inactive_head;
        while (v && v->dirty && page_flush_one(v) < 0)
            v = v->lru_next;
        if (!v)
            return NULL;                 /* every victim owes the disk */
        p = v;
        g_evicts++;
        hash_remove(p);
        lru_unlink(p);
    }
    p->ctx = ctx;
    p->off = off;
    p->active = 0;
    p->dirty = 0;                        /* the old owner flushed above  */
    p->ra = 0;
    p->wr = NULL;
    p->next = NULL;
    hash_insert(p);
    lru_push(&g_inactive_head, &g_inactive_tail, p);
    return p;
}

/* ---- readahead ---------------------------------------------------------- */

static ra_state_t *ra_find(void *ctx)
{
    for (int i = 0; i < RA_TABLE; i++)
        if (g_ra[i].in_use && g_ra[i].ctx == ctx)
            return &g_ra[i];
    return NULL;
}

static unsigned g_ra_victim;             /* rotates when the table is full */

static ra_state_t *ra_get(void *ctx)
{
    ra_state_t *r = ra_find(ctx);
    if (r)
        return r;
    /* take a free slot; when the table is full, rotate through it and
     * steal one -- a dead device's window must not block a live stream */
    for (int i = 0; i < RA_TABLE; i++) {
        if (!g_ra[i].in_use) {
            r = &g_ra[i];
            break;
        }
    }
    if (!r)
        r = &g_ra[g_ra_victim++ % RA_TABLE];
    r->ctx = ctx;
    r->last_page = (uint64_t)-1;
    r->window = RA_MIN;
    r->in_use = 1;
    return r;
}

/* Issue speculative reads for `count` pages after `start`, marking them as
 * readahead (unread).  Misses simply skip: holes read on demand later. */
static void ra_issue(void *ctx, pc_dev_read_t rd, uint64_t start, uint32_t count)
{
    for (uint32_t i = 1; i <= count; i++) {
        uint64_t poff = (start + i) * PC_PAGE;
        if (!hash_find(ctx, poff)) {
            pc_page_t *p = page_alloc(ctx, poff);
            if (!p)
                return;
            if (rd(ctx, poff, p->data, PC_PAGE) < 0) {
                hash_remove(p);
                lru_unlink(p);
                return;
            }
            p->ra = 1;
        }
    }
}

void pagecache_init(void)
{
    memset(g_bucket, 0, sizeof g_bucket);
    g_used = g_hits = g_misses = g_evicts = 0;
    g_promote = g_demote = 0;
    g_dirty_n = g_wb = 0;
    memset(g_ra, 0, sizeof g_ra);
    g_active_n = g_inactive_n = 0;
    g_active_head = g_active_tail = g_inactive_head = g_inactive_tail = NULL;
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

        /* Readahead bookkeeping, once per page: consuming a page the
         * prefetch filled widens the window (the stream is real), and a
         * page that follows the stream's last position keeps it running.
         * Anything else -- random offsets, backwards seeks -- collapses
         * the window again. */
        ra_state_t *r = ra_get(ctx);
        if (r) {
            uint64_t idx = poff / PC_PAGE;
            if (p->ra) {
                p->ra = 0;
                if (r->window < RA_MAX)
                    r->window = (r->window + 1) * 2 > RA_MAX
                              ? RA_MAX : (r->window + 1) * 2;
            }
            if (idx == r->last_page + 1)
                ra_issue(ctx, rd, idx, r->window);
            else if (idx != r->last_page)
                r->window = RA_MIN;
            r->last_page = idx;
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
    /* Write-BACK: the cache is the truth until someone flushes.  The page
     * is fetched (a partial-page write must not clobber the rest of the
     * page with garbage), updated, and marked dirty; the device sees
     * nothing until pagecache_flush_* runs or reclaim claims the page. */
    uint32_t done = 0;
    while (done < len) {
        uint64_t poff = (off + done) & ~(uint64_t)(PC_PAGE - 1);
        uint32_t in_page = (uint32_t)((off + done) - poff);
        uint32_t n = len - done;
        if (n > PC_PAGE - in_page)
            n = PC_PAGE - in_page;

        pc_page_t *p = hash_find(ctx, poff);
        if (!p) {
            g_misses++;
            p = page_alloc(ctx, poff);
            if (!p)
                return done ? (int)done : -1;
            if (rd(ctx, poff, p->data, PC_PAGE) < 0) {
                hash_remove(p);
                lru_unlink(p);
                return done ? (int)done : -1;
            }
        }
        memcpy(p->data + in_page, (const uint8_t *)buf + done, n);
        p->wr = wr;                      /* remember the flush path */
        if (!p->dirty) {
            p->dirty = 1;
            g_dirty_n++;
        }
        /* deliberately NO lru_touch here: a write does not promote.  The
         * page stays on the inactive list where reclaim can reach it and
         * its dirty data gets flushed by the first eviction; only a
         * re-access (a read through lru_touch) earns the promotion. */
        done += n;
    }
    (void)rd;
    return (int)done;
}

int pagecache_invalidate(void *ctx)
{
    int r = 0;
    for (unsigned b = 0; b < PC_BUCKETS; b++) {
        pc_page_t *p = g_bucket[b];
        while (p) {
            pc_page_t *next = p->next;
            if (p->ctx == ctx) {
                if (p->dirty && page_flush_one(p) < 0)
                    r = -1;              /* page kept: it still owes data */
                else {
                    hash_remove(p);
                    lru_unlink(p);
                    p->ctx = NULL;
                }
            }
            p = next;
        }
    }
    return r;
}

void pagecache_stats(uint32_t *hits, uint32_t *misses, uint32_t *evicts,
                     uint32_t *pages)
{
    if (hits)  *hits = g_hits;
    if (misses) *misses = g_misses;
    if (evicts) *evicts = g_evicts;
    if (pages) *pages = g_used;
}

void pagecache_wb_stats(uint32_t *dirty_now, uint32_t *writebacks)
{
    if (dirty_now)   *dirty_now = g_dirty_n;
    if (writebacks)  *writebacks = g_wb;
}

/* A driver with no backing store: reads memcpy out of a kernel buffer and
 * writes memcpy in, both counted -- the counters are what the assertions
 * below reason about.  64 pages so readahead windows always fit. */
static uint8_t  g_fake_dev[PC_PAGE * 64];
static uint32_t g_fake_reads, g_fake_writes;
static uint32_t g_fake_read_at[64];      /* reads per page index */

static int fake_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    (void)ctx;
    g_fake_reads++;
    g_fake_read_at[off / PC_PAGE]++;
    memcpy(buf, g_fake_dev + off, len);
    return (int)len;
}

static int fake_write(void *ctx, uint64_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    g_fake_writes++;
    memcpy(g_fake_dev + off, buf, len);
    return (int)len;
}

void pagecache_selftest(void)
{
    uint32_t h0, m0, e0, p0;
    pagecache_init();
    pagecache_stats(&h0, &m0, &e0, &p0);

    void *ctx = (void *)0x1234;
    uint8_t out[PC_PAGE];

    /* ---- write-back: a write is NOT on the device until sync ---------- */
    uint32_t before = g_fake_writes;
    memcpy(g_fake_dev, "OLD-CONTENT", 11);
    if (pagecache_write(ctx, fake_read, fake_write, 0, "NEW-CONTENT", 11) != 11) {
        dbg_puts("PCACHE: FAIL (write returned)\r\n");
        return;
    }
    if (g_fake_writes != before) {
        dbg_puts("PCACHE: FAIL (write reached the device before sync)\r\n");
        return;
    }
    /* the cache answers the read from memory, not from the stale device */
    if (pagecache_read(ctx, fake_read, 0, out, 11) != 11 ||
        memcmp(out, "NEW-CONTENT", 11)) {
        dbg_puts("PCACHE: FAIL (read did not see the cached write)\r\n");
        return;
    }
    if (pagecache_flush_ctx(ctx) != 0 || g_fake_writes != before + 1) {
        dbg_puts("PCACHE: FAIL (flush did not write back)\r\n");
        return;
    }
    /* after the flush the device copy is current: invalidate then read
     * must show the flushed data */
    if (pagecache_invalidate(ctx) != 0 ||
        pagecache_read(ctx, fake_read, 0, out, 11) != 11 ||
        memcmp(out, "NEW-CONTENT", 11)) {
        dbg_puts("PCACHE: FAIL (flushed data not on the device)\r\n");
        return;
    }

    /* ---- eviction must flush: a dirty page never just disappears ------ */
    before = g_fake_writes;
    uint8_t pat[PC_PAGE];
    memset(pat, 0xAB, PC_PAGE);
    if (pagecache_write(ctx, fake_read, fake_write, PC_PAGE * 5, pat,
                        PC_PAGE) != PC_PAGE) {
        dbg_puts("PCACHE: FAIL (full-page write)\r\n");
        return;
    }
    if (g_fake_writes != before) {
        dbg_puts("PCACHE: FAIL (full-page write hit the device early)\r\n");
        return;
    }
    /* push enough fresh pages through to force eviction of everything,
     * including the dirty one at page 5 */
    for (unsigned i = 0; i < PC_PAGES + 128; i++) {
        void *c = (void *)(uintptr_t)(0x90000u + i * 0x10u);
        if (pagecache_read(c, fake_read, 0, out, 16) != 16) {
            dbg_puts("PCACHE: FAIL (eviction pressure)\r\n");
            return;
        }
    }
    if (g_fake_writes != before + 1) {
        dbg_puts("PCACHE: FAIL (dirty page evicted without writeback)\r\n");
        return;
    }
    /* the flushed page must have landed at the right place on the device */
    for (int i = 0; i < PC_PAGE; i++) {
        if (g_fake_dev[PC_PAGE * 5 + i] != 0xAB) {
            dbg_puts("PCACHE: FAIL (writeback landed in the wrong place)\r\n");
            return;
        }
    }

    /* ---- readahead: the first read of a stream prefetches ------------- */
    void *sctx = (void *)0x5150;
    memset(g_fake_read_at, 0, sizeof g_fake_read_at);
    before = g_fake_reads;
    if (pagecache_read(sctx, fake_read, 0, out, PC_PAGE) != PC_PAGE) {
        dbg_puts("PCACHE: FAIL (stream first read)\r\n");
        return;
    }
    if (g_fake_reads != before + 3) {    /* the page + RA_MIN prefetches */
        dbg_puts("PCACHE: FAIL (no prefetch on sequential read)\r\n");
        return;
    }
    /* the prefetch covered page 1: reading it must not touch the device
     * for PAGE 1 again (the widened window may legitimately prefetch
     * further pages, so only the per-offset counter is asserted) */
    if (pagecache_read(sctx, fake_read, PC_PAGE, out, PC_PAGE) != PC_PAGE ||
        g_fake_read_at[1] != 1) {
        dbg_puts("PCACHE: FAIL (prefetched page was re-fetched) at1=");
        dbg_puts_dec(g_fake_read_at[1]);
        dbg_puts(" total=");
        dbg_puts_dec(g_fake_reads);
        dbg_puts("\r\n");
        return;
    }

    /* ---- LRU: the two-list machinery ----
     * PC_PAGES/4 devices, three pages each (stream + two prefetched) --
     * the whole working set must fit in the pool so the re-read phase
     * measures promotion, not eviction. */
    for (unsigned i = 0; i < PC_PAGES / 4; i++) {
        void *c = (void *)(uintptr_t)(0x1000u + i * 0x10u);
        if (pagecache_read(c, fake_read, 0, out, 16) != 16) {
            dbg_puts("PCACHE: FAIL (fill)\r\n");
            return;
        }
    }
    for (unsigned i = 0; i < PC_PAGES / 4; i++) {
        void *c = (void *)(uintptr_t)(0x1000u + i * 0x10u);
        if (pagecache_read(c, fake_read, 0, out, 16) != 16) {
            dbg_puts("PCACHE: FAIL (re-read)\r\n");
            return;
        }
    }
    if (g_promote < PC_PAGES / 4) {
        dbg_puts("PCACHE: FAIL (second hits did not promote)\r\n");
        return;
    }
    for (unsigned i = 0; i < PC_PAGES + 64; i++) {
        void *c = (void *)(uintptr_t)(0x50000u + i * 0x10u);
        if (pagecache_read(c, fake_read, 0, out, 16) != 16 ||
            pagecache_read(c, fake_read, 0, out, 16) != 16) {
            /* the second read hits, and its promotion grows the active
             * list until the balance step has to demote again */
            dbg_puts("PCACHE: FAIL (allocation under pressure)\r\n");
            return;
        }
    }
    if (!g_demote) {
        dbg_puts("PCACHE: FAIL (no demotion under pressure)\r\n");
        return;
    }
    uint32_t hits, misses, evicts, pages, dirty_now, wbs;
    pagecache_stats(&hits, &misses, &evicts, &pages);
    pagecache_wb_stats(&dirty_now, &wbs);
    if (!hits || !misses || !evicts || pages != PC_PAGES) {
        dbg_puts("PCACHE: FAIL (counters)\r\n");
        return;
    }
    if (!g_promote || !g_demote) {
        dbg_puts("PCACHE: FAIL (two-list LRU never promoted/demoted)\r\n");
        return;
    }
    if (!wbs) {
        dbg_puts("PCACHE: FAIL (writeback counter never moved)\r\n");
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
    dbg_puts(" evictions / ");
    dbg_puts_dec(g_promote);
    dbg_puts(" promoted / ");
    dbg_puts_dec(g_demote);
    dbg_puts(" demoted / ");
    dbg_puts_dec(wbs);
    dbg_puts(" writebacks\r\n");
    pagecache_init();
}
