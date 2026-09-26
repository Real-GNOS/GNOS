/* SPDX-License-Identifier: GPL-2.0 */
/*
 * slab.c — kmem_cache object caches. (GPLv2)
 *
 * A cache owns a list of slabs.  Each slab is one run of pages carved out of
 * the kernel heap and divided into equal objects; the objects a slab does
 * not currently have on loan chain through their own first word (the classic
 * Bonwick freelist-in-the-objects, which costs no per-object metadata).
 *
 *   cache
 *    ├─ slabs_full     every object on loan
 *    ├─ slabs_partial  some on loan, some free
 *    └─ slabs_free     nothing on loan (kept for a while before destruction)
 *
 * Allocation order: this CPU's array cache → a partial slab → carve a new
 * slab.  Free order: this CPU's array cache (unless full → return a batch
 * to a partial slab) → the object's own slab.  Finding the slab for a free
 * object uses slab->objmask: because every slab run is page aligned and its
 * size is a power of two, the slab header lives at (obj & ~objmask).
 *
 * Colouring: successive slabs start their object area at increasing offsets
 * within SLAB_COLOUR_ALIGN (one cache line), so object N in slab A and
 * object N in slab B map to different lines in the same L1 set.  The wasted
 * bytes at the end of each slab are the price; SLAB_COLOUR_MAX bounds it.
 *
 * Concurrency: the kernel runs under a big kernel lock, so the slab lists
 * are only touched with it held -- kmalloc/kfree from interrupt context are
 * the exception, and they only touch the per-CPU array cache, which the
 * interrupted code cannot be inside of on the same CPU (the BKL guarantees
 * the interrupted context was not mid-array-op because the arrays are only
 * touched with interrupts enabled... except by IRQs on OTHER cores touching
 * THEIR arrays, which are separate).  slab lists take g_slab_lock.
 */
#include <stdint.h>
#include <stddef.h>

#include "slab.h"
#include "heap.h"
#include "kstring.h"
#include "debugcon.h"
#include "panic.h"
#include "pmm.h"

#define SLAB_ALIGN_MIN      16u
#define SLAB_COLOUR_ALIGN   64u      /* one cache line                    */
#define SLAB_COLOUR_MAX     16u      /* at most 16 distinct colour steps  */
#define SLAB_OBJ_MIN        16u      /* the freelist needs a pointer word */
#define SLAB_MAX_SLAB_ORDER 4u       /* a slab is at most 16 KiB          */
#define SLAB_AC_SIZE        16u      /* per-CPU array cache depth         */
#define SLAB_AC_BATCH       8u       /* refill/drain batch                */
#define SLAB_CPUS           4u       /* matches NR_RQ                     */

/* One run of objects.  Page aligned (we over-allocate and round up), power
 * of two in size, so the header is found by masking an object address. */
typedef struct kmem_slab {
    struct kmem_slab *next, *prev;
    uint64_t          objmask;   /* AND an object address with this -> slab */
    kmem_cache_t     *cache;
    void             *freelist;  /* chained through the free objects        */
    uint32_t          in_use;    /* objects on loan                         */
    uint32_t          colour;    /* this slab's colour step                 */
} kmem_slab_t;

/* Per-CPU array cache of hot object pointers. */
typedef struct {
    uint32_t avail;
    uint32_t limit;              /* SLAB_AC_SIZE for now                    */
    void    *entry[SLAB_AC_SIZE];
} kmem_ac_t;

struct kmem_cache {
    const char   *name;
    uint32_t      obj_size;      /* as the caller asked                     */
    uint32_t      buf_size;      /* rounded, freelist-ready stride          */
    uint32_t      align;
    uint32_t      flags;
    kmem_ctor_t   ctor;

    uint32_t      objs_per_slab;
    uint64_t      slab_size;     /* bytes per slab run (power of two)       */
    uint32_t      colour_next;   /* next slab's colour step                 */
    uint32_t      colour_max;    /* number of usable colour steps           */
    uint32_t      colour_off;    /* bytes per colour step                   */

    kmem_slab_t  *slabs_full;
    kmem_slab_t  *slabs_partial;
    kmem_slab_t  *slabs_free;
    uint32_t      num_full, num_partial, num_free;

    kmem_ac_t     ac[SLAB_CPUS];

    /* statistics, all relaxed */
    uint32_t      ac_hits, slab_hits, grown, reaped;

    kmem_cache_t *next;          /* registry chain for /proc/slabinfo       */
};

static kmem_cache_t *g_caches;          /* all live caches                  */
static int           g_slab_ready;      /* set after slab_init              */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(uint64_t)(a - 1);
}

static void slab_rehome(kmem_cache_t *c, kmem_slab_t *s);

static uint64_t is_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

/* ---- slab list plumbing ------------------------------------------------ */

static void slab_list_add(kmem_slab_t **head, kmem_slab_t *s)
{
    s->prev = NULL;
    s->next = *head;
    if (*head)
        (*head)->prev = s;
    *head = s;
}

static void slab_list_del(kmem_slab_t **head, kmem_slab_t *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else if (*head == s)
        *head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

/* ---- slab construction ------------------------------------------------- */

/* Carve one slab run out of the heap.  The run is page aligned and a power
 * of two so that obj & ~objmask finds the header; the object area starts
 * after the header and after this slab's colour offset. */
static kmem_slab_t *slab_grow(kmem_cache_t *c)
{
    /* alloc + alignment slack: alignment can cost up to slab_size more */
    uint8_t *raw = kmalloc((uint32_t)c->slab_size * 2);
    if (!raw)
        return NULL;
    uint64_t base = align_up((uint64_t)(uintptr_t)raw, c->slab_size);
    kmem_slab_t *s = (kmem_slab_t *)base;

    s->cache   = c;
    s->objmask = ~(c->slab_size - 1);
    s->in_use  = 0;

    /* colour this slab */
    s->colour = c->colour_next;
    c->colour_next = (c->colour_next + 1) % c->colour_max;

    uint8_t *objs = (uint8_t *)base + c->slab_size
                  - c->objs_per_slab * c->buf_size
                  + s->colour * c->colour_off;

    /* chain the objects; the constructor runs once per object ever */
    s->freelist = NULL;
    for (uint32_t i = c->objs_per_slab; i > 0; i--) {
        void *o = objs + (i - 1) * c->buf_size;
        if (c->ctor)
            c->ctor(o, c);
        else if (c->flags & SLAB_ZEROED)
            memset(o, 0, c->obj_size);
        *(void **)o = s->freelist;
        s->freelist = o;
    }

    slab_list_add(&c->slabs_partial, s);
    c->num_partial++;
    c->grown++;
    return s;
}

static void slab_destroy(kmem_cache_t *c, kmem_slab_t *s)
{
    (void)c;                         /* destructors would live here */
    /* destructors are not supported (no GNOS user needs one yet); the
     * memory simply goes back to the heap. */
    kfree(s);
}

/* ---- cache lifecycle --------------------------------------------------- */

kmem_cache_t *kmem_cache_create(const char *name, uint32_t size, uint32_t align,
                                uint32_t flags, kmem_ctor_t ctor)
{
    if (!name || !size || size > 64 * 1024)
        return NULL;

    kmem_cache_t *c = kmalloc(sizeof(kmem_cache_t));
    if (!c)
        return NULL;
    memset(c, 0, sizeof(*c));

    c->name  = name;
    c->flags = flags;
    c->ctor  = ctor;

    c->obj_size = size;
    c->align    = align ? align : SLAB_ALIGN_MIN;
    if (c->align < SLAB_ALIGN_MIN)
        c->align = SLAB_ALIGN_MIN;
    if (flags & SLAB_HWCACHE_ALIGN)
        c->align = c->align > SLAB_COLOUR_ALIGN ? c->align : SLAB_COLOUR_ALIGN;
    if (!is_pow2(c->align))
        c->align = SLAB_ALIGN_MIN;

    /* buffer stride: big enough for the object, a freelist pointer when the
     * object is smaller than one, and the requested alignment */
    uint32_t need = c->obj_size < SLAB_OBJ_MIN ? SLAB_OBJ_MIN : c->obj_size;
    c->buf_size = (uint32_t)align_up(need, c->align);

    /* slab geometry: one page for anything that fits, doubling while the
     * object stride demands more, capped at SLAB_MAX_SLAB_ORDER pages.
     * (SLAB_MAX_SLAB_ORDER counts pages here, not bytes.) */
    uint64_t pages = 1;
    while (c->buf_size + sizeof(kmem_slab_t) > pages * PAGE_SIZE &&
           pages < (1u << SLAB_MAX_SLAB_ORDER))
        pages <<= 1;
    c->slab_size = pages * PAGE_SIZE;

    /* how many objects fit behind the slab header; the tail waste is then
     * split into cache-line colour steps (at least one, meaning "no
     * colouring" for a geometry with no room for it) */
    uint64_t avail = c->slab_size - sizeof(kmem_slab_t);
    c->objs_per_slab = (uint32_t)(avail / c->buf_size);
    if (!c->objs_per_slab)
        c->objs_per_slab = 1;

    uint64_t waste = avail - (uint64_t)c->objs_per_slab * c->buf_size;
    uint64_t steps = waste / SLAB_COLOUR_ALIGN;
    c->colour_off = (uint32_t)SLAB_COLOUR_ALIGN;
    c->colour_max = (uint32_t)(steps > SLAB_COLOUR_MAX ? SLAB_COLOUR_MAX
                                                       : (steps ? steps : 1));

    for (uint32_t i = 0; i < SLAB_CPUS; i++) {
        c->ac[i].limit = SLAB_AC_SIZE;
        c->ac[i].avail = 0;
    }

    c->next = g_caches;
    g_caches = c;
    return c;
}

void kmem_cache_destroy(kmem_cache_t *c)
{
    if (!c)
        return;

    /* leave the registry first: /proc/slabinfo walks g_caches and must
     * never see a cache that is about to be freed */
    if (g_caches == c) {
        g_caches = c->next;
    } else {
        for (kmem_cache_t *p = g_caches; p; p = p->next)
            if (p->next == c) {
                p->next = c->next;
                break;
            }
    }

    /* drain every per-CPU array cache back into the slabs: objects parked
     * in an array cache are still counted as on loan, and destroy must not
     * leave a single one behind */
    for (uint32_t cpu = 0; cpu < SLAB_CPUS; cpu++) {
        kmem_ac_t *ac = &c->ac[cpu];
        while (ac->avail) {
            void *o = ac->entry[--ac->avail];
            kmem_slab_t *sl = (kmem_slab_t *)((uint64_t)o &
                              ~(c->slab_size - 1));
            if (sl->in_use == c->objs_per_slab) {
                slab_list_del(&c->slabs_full, sl);
                c->num_full--;
            } else {
                slab_list_del(&c->slabs_partial, sl);
                c->num_partial--;
            }
            *(void **)o = sl->freelist;
            sl->freelist = o;
            sl->in_use--;
            slab_rehome(c, sl);
        }
    }

    /* drain: every slab must be completely free */
    kmem_slab_t *s = c->slabs_partial;
    while (s) {
        kmem_slab_t *n = s->next;
        if (s->in_use)
            panic("kmem_cache_destroy: cache has active objects");
        slab_destroy(c, s);
        s = n;
    }
    s = c->slabs_full;
    while (s) {
        kmem_slab_t *n = s->next;
        if (s->in_use)
            panic("kmem_cache_destroy: cache has active objects");
        slab_destroy(c, s);
        s = n;
    }
    s = c->slabs_free;
    while (s) {
        kmem_slab_t *n = s->next;
        slab_destroy(c, s);
        s = n;
    }
    kfree(c);
}

/* ---- allocation -------------------------------------------------------- */

void *kmem_cache_alloc(kmem_cache_t *c)
{
    if (!c || !g_slab_ready)
        return NULL;

    /* 1. this CPU's array cache */
    unsigned cpu = 0;                    /* BKL serialises; cpu id unused yet */
    kmem_ac_t *ac = &c->ac[cpu];
    if (ac->avail) {
        ac->avail--;
        c->ac_hits++;
        return ac->entry[ac->avail];
    }

    /* refill the array cache from a partial slab (growing if needed), then
     * serve the request from it */
    kmem_slab_t *s = c->slabs_partial;
    if (!s) {
        s = slab_grow(c);
        if (!s) {
            if (c->flags & SLAB_PANIC)
                panic("kmem_cache_alloc: out of memory");
            return NULL;
        }
    }
    c->slab_hits++;

    uint32_t n = 0;
    while (s->freelist && ac->avail < ac->limit && n < SLAB_AC_BATCH) {
        void *o = s->freelist;
        s->freelist = *(void **)o;
        s->in_use++;
        ac->entry[ac->avail++] = o;
        n++;
        if (!s->freelist) {              /* slab just went full */
            slab_list_del(&c->slabs_partial, s);
            slab_list_add(&c->slabs_full, s);
            c->num_partial--;
            c->num_full++;
            break;
        }
    }

    /* a one-object slab or a shallow limit can leave the cache empty even
     * after the refill; the objects are in it -- pop and go */
    ac->avail--;
    c->ac_hits++;
    return ac->entry[ac->avail];
}

void *kmem_cache_zalloc(kmem_cache_t *c)
{
    void *o = kmem_cache_alloc(c);
    if (o && !(c->flags & SLAB_ZEROED) && !c->ctor)
        memset(o, 0, c->obj_size);
    return o;
}

/* Which list does a slab belong on, given its in_use count?  An emptied
 * slab goes back on the partial list: the next grow is a carve, and reuse
 * is the whole point. */
static void slab_rehome(kmem_cache_t *c, kmem_slab_t *s)
{
    if (s->in_use == c->objs_per_slab) {
        slab_list_add(&c->slabs_full, s);
        c->num_full++;
    } else {
        slab_list_add(&c->slabs_partial, s);
        c->num_partial++;
    }
}

/* Push every object an array cache holds back into its slab. */
static void ac_drain(kmem_cache_t *c, kmem_ac_t *ac)
{
    while (ac->avail) {
        void *o = ac->entry[--ac->avail];
        /* find the slab by mask: every run is page aligned and a power of
         * two in size, so its header sits at (obj & ~(slab_size - 1)) */
        kmem_slab_t *sl = (kmem_slab_t *)((uint64_t)o &
                          ~(c->slab_size - 1));
        if (sl->in_use == c->objs_per_slab) {
            slab_list_del(&c->slabs_full, sl);
            c->num_full--;
        } else {
            slab_list_del(&c->slabs_partial, sl);
            c->num_partial--;
        }
        *(void **)o = sl->freelist;
        sl->freelist = o;
        sl->in_use--;
        slab_rehome(c, sl);
    }
}

void kmem_cache_free(kmem_cache_t *c, void *obj)
{
    if (!c || !obj)
        return;

    /* 1. this CPU's array cache, if there is room */
    unsigned cpu = 0;
    kmem_ac_t *ac = &c->ac[cpu];
    if (ac->avail < ac->limit) {
        ac->entry[ac->avail++] = obj;
        return;
    }

    /* 2. drain the array cache back to their slabs, then the object goes
     * in afterwards */
    while (ac->avail) {
        void *o = ac->entry[--ac->avail];
        /* find the slab by mask: every run is page aligned and a power of
         * two in size, so its header sits at (obj & ~(slab_size - 1)) */
        kmem_slab_t *sl = (kmem_slab_t *)((uint64_t)o &
                          ~(c->slab_size - 1));
        if (sl->in_use == c->objs_per_slab) {
            slab_list_del(&c->slabs_full, sl);
            c->num_full--;
        } else if (sl->in_use == 0) {
            slab_list_del(&c->slabs_free, sl);
            c->num_free--;
        } else {
            slab_list_del(&c->slabs_partial, sl);
            c->num_partial--;
        }
        *(void **)o = sl->freelist;
        sl->freelist = o;
        sl->in_use--;
        slab_rehome(c, sl);
    }

    if (ac->avail < ac->limit) {
        ac->entry[ac->avail++] = obj;
        return;
    }
    panic("kmem_cache_free: array cache refuses to drain");
}

/* ---- /proc/slabinfo ---------------------------------------------------- */

int kmem_slabinfo_next(int *iter, kmem_slabinfo_t *out)
{
    kmem_cache_t *c = g_caches;
    int i = 0;
    while (c && i < *iter) {
        c = c->next;
        i++;
    }
    if (!c)
        return -1;
    *iter = i + 1;

    uint32_t active = 0, total = 0;
    for (kmem_slab_t *s = c->slabs_full; s; s = s->next) {
        active += s->in_use;
        total  += c->objs_per_slab;
    }
    for (kmem_slab_t *s = c->slabs_partial; s; s = s->next) {
        active += s->in_use;
        total  += c->objs_per_slab;
    }
    for (kmem_slab_t *s = c->slabs_free; s; s = s->next)
        total += c->objs_per_slab;

    out->name         = c->name;
    out->obj_size     = c->obj_size;
    out->objs_per_slab= c->objs_per_slab;
    out->active_objs  = active + c->ac[0].avail;
    out->num_objs     = total;
    out->num_full     = c->num_full;
    out->num_partial  = c->num_partial;
    out->num_slabs    = c->num_full + c->num_partial + c->num_free;
    out->ac_hits      = c->ac_hits;
    out->slab_hits    = c->slab_hits;
    return 0;
}

/* ---- init & self-test --------------------------------------------------- */

void slab_init(void)
{
    g_slab_ready = 1;
    dbg_puts("SLAB: object caches ready\r\n");
}

void slab_selftest(void)
{
    kmem_cache_t *c = kmem_cache_create("slabtest", 96, 0, 0, NULL);
    if (!c) {
        dbg_puts("SLAB: FAIL (create)\r\n");
        return;
    }

    /* alloc 4x objs_per_slab + a few: forces several grows, full+partial */
    enum { N = 200 };
    static void *objs[N];
    for (int i = 0; i < N; i++) {
        objs[i] = kmem_cache_alloc(c);
        if (!objs[i]) {
            dbg_puts("SLAB: FAIL (alloc)\r\n");
            return;
        }
        /* write every byte: catches overlapping objects */
        memset(objs[i], (uint8_t)(i & 0xFF), 96);
    }
    for (int i = 0; i < N; i++) {
        uint8_t *p = objs[i];
        for (int b = 0; b < 96; b++) {
            if (p[b] != (uint8_t)(i & 0xFF)) {
                dbg_puts("SLAB: FAIL (cross-object corruption)\r\n");
                return;
            }
        }
    }
    for (int i = 0; i < N; i++)
        kmem_cache_free(c, objs[i]);

    /* alloc/free churn: exercises the array cache paths */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 64; i++) {
            objs[i] = kmem_cache_zalloc(c);
            if (!objs[i]) {
                dbg_puts("SLAB: FAIL (churn alloc)\r\n");
                return;
            }
        }
        for (int i = 0; i < 64; i++)
            kmem_cache_free(c, objs[i]);
    }

    kmem_cache_destroy(c);

    /* colouring: a 1 KiB object leaves ~1 KiB of tail per page, enough for
     * several cache-line colour steps; a packed geometry must NOT be
     * forced into colouring, so only the roomy one is asserted */
    kmem_cache_t *cc = kmem_cache_create("slabtest-colour", 1024, 0, 0, NULL);
    if (!cc) {
        dbg_puts("SLAB: FAIL (colour create)\r\n");
        return;
    }
    if (cc->colour_max <= 1) {
        dbg_puts("SLAB: FAIL (no colour range)\r\n");
        return;
    }
    void *o1 = kmem_cache_alloc(cc);
    void *o2 = kmem_cache_alloc(cc);
    if (!o1 || !o2) {
        dbg_puts("SLAB: FAIL (colour alloc)\r\n");
        return;
    }
    kmem_cache_free(cc, o1);
    kmem_cache_free(cc, o2);
    uint32_t steps = cc->colour_max;
    kmem_cache_destroy(cc);

    dbg_puts("SLAB: self-test PASS (colour steps ");
    dbg_puts_dec(steps);
    dbg_puts(")\r\n");
}
