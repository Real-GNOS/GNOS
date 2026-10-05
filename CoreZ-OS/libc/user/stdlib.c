#include "libc/user/stdlib.h"

#include <stddef.h>
#include <stdint.h>

#include "libc/user/syscall.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

#define WSIZE (sizeof(size_t))
#define DSIZE (2 * WSIZE)
#define CHUNK ((1u << 12) / WSIZE)

#define ALLOC_BIT ((size_t)0x1)

static inline size_t get_u(void *p) {
    return *(size_t *)p;
}

static inline void put_u(void *p, size_t v) {
    *(size_t *)p = v;
}

static inline size_t blk_size(void *hdr) {
    return get_u(hdr) & ~(size_t)ALLOC_BIT;
}

static inline int blk_alloc(void *hdr) {
    return (int)(get_u(hdr) & ALLOC_BIT);
}

static inline void *hdr_of(void *bp) {
    return (char *)bp - WSIZE;
}

static inline void *ftr_of(void *bp) {
    return (char *)bp + blk_size(hdr_of(bp)) - DSIZE;
}

static inline void *next_blk(void *bp) {
    return (char *)bp + blk_size(hdr_of(bp));
}

static inline void *prev_blk(void *bp) {
    size_t psize = get_u((char *)bp - DSIZE) & ~(size_t)ALLOC_BIT;
    return (char *)bp - psize;
}

static char *heap = NULL;

static void *more_heap(size_t words) {
    size_t size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    char *bp = (char *)sbrk((intptr_t)size);
    if (bp == (void *)-1) {
        return NULL;
    }
    put_u(hdr_of(bp), size | 0);
    put_u(ftr_of(bp), size | 0);
    put_u(hdr_of(next_blk(bp)), 0 | ALLOC_BIT);
    return bp;
}

static void *coalesce(void *bp) {
    size_t prev_alloc = (size_t)blk_alloc(hdr_of(prev_blk(bp)));
    size_t next_alloc = (size_t)blk_alloc(hdr_of(next_blk(bp)));
    size_t size = blk_size(hdr_of(bp));

    if (prev_alloc && next_alloc) {
        return bp;
    } else if (prev_alloc && !next_alloc) {
        size += blk_size(hdr_of(next_blk(bp)));
        put_u(hdr_of(bp), size | 0);
        put_u(ftr_of(bp), size | 0);
    } else if (!prev_alloc && next_alloc) {
        size += blk_size(hdr_of(prev_blk(bp)));
        void *pp = prev_blk(bp);
        put_u(hdr_of(pp), size | 0);
        put_u(ftr_of(pp), size | 0);
        bp = pp;
    } else {
        size += blk_size(hdr_of(prev_blk(bp))) + blk_size(hdr_of(next_blk(bp)));
        void *pp = prev_blk(bp);
        put_u(hdr_of(pp), size | 0);
        put_u(ftr_of(pp), size | 0);
        bp = pp;
    }
    return bp;
}

static void place(void *bp, size_t asize) {
    size_t csize = blk_size(hdr_of(bp));
    if (csize - asize >= (2 * DSIZE)) {
        put_u(hdr_of(bp), asize | ALLOC_BIT);
        put_u(ftr_of(bp), asize | ALLOC_BIT);
        void *nbp = next_blk(bp);
        put_u(hdr_of(nbp), (csize - asize) | 0);
        put_u(ftr_of(nbp), (csize - asize) | 0);
    } else {
        put_u(hdr_of(bp), csize | ALLOC_BIT);
        put_u(ftr_of(bp), csize | ALLOC_BIT);
    }
}

static void *first_block(void) {
    return next_blk(heap);
}

static void *find_fit(size_t asize) {
    for (void *bp = first_block();; bp = next_blk(bp)) {
        size_t sz = blk_size(hdr_of(bp));
        if (sz == 0) {
            break;
        }
        if (!blk_alloc(hdr_of(bp)) && sz >= asize) {
            return bp;
        }
    }
    return NULL;
}

static int heap_init(void) {
    char *p = (char *)sbrk((intptr_t)(4 * WSIZE));
    if (p == (void *)-1) {
        return -1;
    }
    put_u(p, 0);
    put_u(p + 1 * WSIZE, (DSIZE | ALLOC_BIT));
    put_u(p + 2 * WSIZE, (DSIZE | ALLOC_BIT));
    put_u(p + 3 * WSIZE, (0 | ALLOC_BIT));
    heap = (char *)(p + 2 * WSIZE);
    if (more_heap(CHUNK) == NULL) {
        return -1;
    }
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    if (heap == NULL) {
        if (heap_init() != 0) {
            return NULL;
        }
    }

    size_t asize = (size <= DSIZE)
                       ? (2 * DSIZE)
                       : (DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE));

    void *bp = find_fit(asize);
    if (bp != NULL) {
        place(bp, asize);
        return bp;
    }

    size_t extendsize = (asize > CHUNK * WSIZE) ? asize : CHUNK * WSIZE;
    bp = more_heap(extendsize / WSIZE);
    if (bp == NULL) {
        return NULL;
    }
    place(bp, asize);
    return bp;
}

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    size_t size = blk_size(hdr_of(ptr));
    put_u(hdr_of(ptr), size | 0);
    put_u(ftr_of(ptr), size | 0);
    coalesce(ptr);
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    size_t total = nmemb * size;
    if (total / nmemb != size) {
        return NULL;
    }
    void *p = malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    size_t old = blk_size(hdr_of(ptr));
    size_t asize = (size <= DSIZE)
                       ? (2 * DSIZE)
                       : (DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE));
    if (asize <= old) {
        return ptr;
    }
    void *n = malloc(size);
    if (n == NULL) {
        return NULL;
    }
    size_t cpy = old - DSIZE;
    if (cpy > size) {
        cpy = size;
    }
    memcpy(n, ptr, cpy);
    free(ptr);
    return n;
}
