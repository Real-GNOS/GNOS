/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mem.c — memory character devices.  (GPL-2.0)
 *then made honest:
 *   - /dev/mem (1:1) is a real physical-memory device over the high-half
 *     direct mapping, bounded by the frame count pmm saw at boot;
 *   - the random devices keep a 256-bit entropy pool fed by RDRAND /
 *     RDSEED when the CPU has them and by TSC/pointer timing otherwise.
 *     /dev/urandom never blocks; /dev/random refuses (-EAGAIN) while the
 *     pool holds fewer than 128 credited bits, the way a non-blocking
 *     reader from a starved pool must behave.
 */

#include "chrdev.h"
#include "smp.h"
#include "vfs.h"            /* E_* */
#include "pmm.h"            /* pmm_virt, pmm_total_frames */
#include "drm_vsnprintf.h"  /* plogk */

#define MEM_MAJOR 1

typedef enum mem_memory_kind {
    MEM_MEM_NULL,
    MEM_MEM_PHYS,
    MEM_MEM_ZERO,
    MEM_MEM_FULL,
    MEM_MEM_RANDOM,
    MEM_MEM_URANDOM,
} mem_memory_kind_t;

typedef struct {
    mem_memory_kind_t kind;
} mem_memory_device_t;

static mem_memory_device_t mem_devices[] = {
    {.kind = MEM_MEM_NULL}, {.kind = MEM_MEM_PHYS}, {.kind = MEM_MEM_ZERO},
    {.kind = MEM_MEM_FULL}, {.kind = MEM_MEM_RANDOM}, {.kind = MEM_MEM_URANDOM},
};

static const struct {
    const char *name;
    uint8_t     minor;
    uint16_t    mode;
} mem_nodes[] = {
    {.name = "null",    .minor = 3, .mode = 0666},
    {.name = "mem",     .minor = 1, .mode = 0600},
    {.name = "zero",    .minor = 5, .mode = 0666},
    {.name = "full",    .minor = 7, .mode = 0666},
    {.name = "random",  .minor = 8, .mode = 0666},
    {.name = "urandom", .minor = 9, .mode = 0666},
};

/* ---- entropy pool ------------------------------------------------------
 *
 * 256 bits of state plus a credit of how many of them are real.  The
 * credit is deliberately conservative: RDRAND/RDSEED output is credited at
 * face value (the instruction spec is what it is), the TSC at 4 bits (it
 * advances, but its phase is guessable), and stack addresses at 8 (ASLR
 * gives a few real bits).  Pool mixing is splitmix64 over the lanes with a
 * rotation sweep -- not a cipher, but it spreads a single strong word
 * across the whole pool so one good RDRAND output heals 32 lanes. */

#define POOL_LANES     32
#define RAND_MIN_BITS  128

static spinlock_t   g_pool_lock;
static uint64_t     g_pool[POOL_LANES];
static uint32_t     g_entropy_bits;
static int          g_rdrand_have, g_rdseed_have;

static void cpuid_detect(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    g_rdrand_have = (ecx >> 30) & 1;
    /* RDSEED lives in leaf 7 EBX bit 18.  Leaf 1 ECX bit 31 is the
     * hypervisor-present flag -- reading RDSEED from there is what armed
     * the first rdseed under QEMU and #UD'd the boot. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    g_rdseed_have = (eax >= 7) ? ((ebx >> 18) & 1) : 0;
    plogk("mem: entropy sources: rdrand=%d rdseed=%d tsc=1\n",
          g_rdrand_have, g_rdseed_have);
}

/* splitmix64 one-step avalanche: the cheap bijection used to fold a source
 * word into a pool lane. */
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* One RDRAND word, or 0 with no credit when the CPU/refusal says no. */
static uint64_t rdrand_word(void)
{
    if (!g_rdrand_have)
        return 0;
    for (int i = 0; i < 8; i++) {
        uint8_t  ok;
        uint64_t v;
        /* setc captures the instruction's carry flag; the 'c' constraint
         * would allocate %rcx, which rdrand never writes. */
        __asm__ volatile("rdrand %0\n\tsetc %1" : "=q"(v), "=r"(ok));
        if (ok)
            return v;
    }
    return 0;
}

static uint64_t rdseed_word(void)
{
    if (!g_rdseed_have)
        return 0;
    for (int i = 0; i < 8; i++) {
        uint8_t  ok;
        uint64_t v;
        __asm__ volatile("rdseed %0\n\tsetc %1" : "=q"(v), "=r"(ok));
        if (ok)
            return v;
    }
    return 0;
}

/* Fold a source word into the pool and credit `bits`.  Called under
 * g_pool_lock. */
static void pool_mix(uint64_t src, uint32_t bits)
{
    if (src == 0 && bits == 0)
        return;
    for (int i = 0; i < POOL_LANES; i++) {
        g_pool[i] ^= splitmix64(src + (uint64_t)i * 0x9e3779b97f4a7c15ULL);
        /* rotation sweep so the low-bit changes of a source word reach
         * every lane's high bits over a few mixes */
        g_pool[i] = (g_pool[i] << 13) | (g_pool[i] >> 51);
    }
    g_entropy_bits += bits;
    if (g_entropy_bits > 64 * POOL_LANES)
        g_entropy_bits = 64 * POOL_LANES;
}

/* Gather whatever the CPU will give us right now and mix it in.  Called
 * under g_pool_lock. */
static void pool_feed(void)
{
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    pool_mix(tsc, 4);
    uint64_t r = rdrand_word();
    if (r)
        pool_mix(r, 64);
    uint64_t s = rdseed_word();
    if (s)
        pool_mix(s, 64);
    pool_mix((uint64_t)(uintptr_t)&tsc, 8);
}

/* Deterministic refill: stretch the current pool state back to a full
 * 256 bits of output material.  This is what /dev/urandom relies on --
 * like Linux's CRNGA refill, the state is strong only as the credit says.
 * Called under g_pool_lock. */
static void pool_refill(void)
{
    uint64_t seed = g_pool[0] ^ (g_pool[1] << 1);
    for (int i = 0; i < POOL_LANES; i++) {
        seed += 0x9e3779b97f4a7c15ULL;
        g_pool[i] = splitmix64(seed);
    }
}

/* Pull `size` bytes of output into the caller buffer, feeding first.
 * When `blocking` is false (urandom) always succeed; when true, return
 * -EAGAIN if the credit cannot cover a single 128-bit draw. */
static int64_t random_read(mem_memory_kind_t kind, void *buffer, size_t size)
{
    spin_lock(&g_pool_lock);
    pool_feed();
    if (kind == MEM_MEM_RANDOM && g_entropy_bits < RAND_MIN_BITS) {
        spin_unlock(&g_pool_lock);
        return -E_AGAIN;
    }
    uint8_t *out = buffer;
    size_t   total = size;
    while (size) {
        if (g_entropy_bits < RAND_MIN_BITS || size < sizeof(uint64_t)) {
            /* short tail: finish from the last partial lane without
             * over-crediting */
            uint64_t w = g_pool[0];
            size_t   n = size < sizeof(uint64_t) ? size : sizeof(uint64_t);
            memcpy(out, &w, n);
            out += n;
            size -= n;
            break;
        }
        uint64_t w = g_pool[0];
        g_pool[0] = g_pool[1];
        for (int i = 1; i < POOL_LANES - 1; i++)
            g_pool[i] = g_pool[i + 1];
        g_pool[POOL_LANES - 1] = splitmix64(g_pool[POOL_LANES - 2] + g_pool[7]);
        g_entropy_bits -= 64;
        memcpy(out, &w, sizeof(w));
        out += sizeof(w);
        size -= sizeof(w);
    }
    pool_refill();
    spin_unlock(&g_pool_lock);
    return (int64_t)total;
}

/* ---- /dev/mem ---------------------------------------------------------- */

static uint64_t phys_limit(void)
{
    return pmm_total_frames() * 4096ULL;
}

static int64_t phys_read(void *ctx, void *private_data, uint64_t flags, void *buffer,
                         size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;

    if (offset + size > phys_limit() || offset + size < offset)
        return -E_INVAL;
    memcpy(buffer, pmm_virt(offset), size);
    return (int64_t)size;
}

static int64_t phys_write(void *ctx, void *private_data, uint64_t flags, const void *buffer,
                          size_t offset, size_t size)
{
    (void)ctx;
    (void)private_data;
    (void)flags;

    if (offset + size > phys_limit() || offset + size < offset)
        return -E_INVAL;
    memcpy(pmm_virt(offset), buffer, size);
    return (int64_t)size;
}

/* ---- generic read/write dispatch --------------------------------------- */

static int64_t mem_read(void *ctx, void *private_data, uint64_t flags, void *buffer,
                        size_t offset, size_t size)
{
    mem_memory_device_t *device = ctx;
    (void)private_data;
    (void)flags;

    if (!device || (!buffer && size))
        return -E_INVAL;
    switch (device->kind) {
    case MEM_MEM_NULL:
        return 0;
    case MEM_MEM_PHYS:
        return phys_read(ctx, private_data, flags, buffer, offset, size);
    case MEM_MEM_ZERO:
    case MEM_MEM_FULL:
        memset(buffer, 0, size);
        return (int64_t)size;
    case MEM_MEM_RANDOM:
        return random_read(device->kind, buffer, size);
    case MEM_MEM_URANDOM:
        return random_read(device->kind, buffer, size);
    }
    return -E_INVAL;
}

static int64_t mem_write(void *ctx, void *private_data, uint64_t flags, const void *buffer,
                         size_t offset, size_t size)
{
    mem_memory_device_t *device = ctx;
    (void)private_data;
    (void)flags;
    (void)buffer;

    if (!device)
        return -E_INVAL;
    switch (device->kind) {
    case MEM_MEM_PHYS:
        return phys_write(ctx, private_data, flags, buffer, offset, size);
    case MEM_MEM_FULL:
        return -E_NOSPC;
    default:
        break;
    }
    return (int64_t)size;
}

/* Register the standard memory character devices. */
void memdev_init(void)
{
    cpuid_detect();
    spin_lock(&g_pool_lock);
    pool_feed();
    spin_unlock(&g_pool_lock);

    for (size_t i = 0; i < sizeof(mem_nodes) / sizeof(mem_nodes[0]); i++) {
        tmpfs_device_ops_t ops;
        memset(&ops, 0, sizeof(ops));
        ops.file_read  = mem_read;
        ops.file_write = mem_write;
        ops.ctx        = &mem_devices[i];
        if (cdev_add("", mem_nodes[i].name, MEM_MAJOR, mem_nodes[i].minor, 1,
                     (uint16_t)file_stream, mem_nodes[i].mode, &ops) != EOK)
            plogk("chrdev: Cannot register /dev/%s\n", mem_nodes[i].name);
    }
}
