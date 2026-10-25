/* SPDX-License-Identifier: GPL-2.0 */
/* asmtest.c — x86-64 instruction semantics, checked from user space.
 *
 * The kernel is built -mgeneral-regs-only and the userland too, so the
 * checks below stick to integer instructions: bswap endianness, rotate
 * through carry-free shifts, 128-bit multiply-divide pairs, cpuid feature
 * reporting and rdtsc monotonicity.  Each case pins a value the ISA
 * manual guarantees; a compiler or CPU that disagrees fails the build's
 * assumptions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-38s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static uint64_t bswap64(uint64_t x)
{
    __asm__ volatile("bswap %0" : "+r"(x));
    return x;
}

static uint32_t bswap32(uint32_t x)
{
    __asm__ volatile("bswap %0" : "+r"(x));
    return x;
}

/* 128-bit signed divide: rdx:rax / divisor -> rax, remainder in rdx. */
static uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem)
{
    uint64_t q, r;
    __asm__ volatile("divq %4" : "=a"(q), "=d"(r) : "d"(hi), "a"(lo), "rm"(d));
    *rem = r;
    return q;
}

static void test_bswap(void)
{
    check("bswap64 reverses bytes", bswap64(0x0123456789ABCDEFull) == 0xEFCDAB8967452301ull);
    check("bswap32 reverses bytes", bswap32(0x12345678u) == 0x78563412u);
    check("bswap is an involution", bswap64(bswap64(0xDEADBEEFCAFEBABEull)) == 0xDEADBEEFCAFEBABEull);
}

static void test_rotate_shift(void)
{
    uint64_t x = 0x8000000000000001ull;
    __asm__ volatile("rolq $1, %0" : "+r"(x));
    check("rolq $1 wraps bit 63 into bit 0", x == 0x0000000000000003ull);

    uint32_t y = 0x12345678u;
    __asm__ volatile("roll $8, %0" : "+r"(y));
    check("roll $8 rotates within word", y == 0x34567812u);

    uint64_t s = 0xF000000000000000ull;
    __asm__ volatile("shrq $60, %0" : "+r"(s));
    check("shrq shifts in zeros", s == 0xFull);
}

static void test_muldiv(void)
{
    /* 0x1_0000_0000 * 2 = 0x2_0000_0000: high word must be set. */
    uint64_t hi, lo;
    __asm__ volatile("mulq %3" : "=d"(hi), "=a"(lo) : "a"(0x100000000ull), "rm"(2ull));
    check("mulq high word set", hi == 2 && lo == 0);

    uint64_t rem = 0;
    uint64_t q = div128(1, 0, 3, &rem); /* (1<<64)/3 */
    check("divq 128/32 quotient", q == 0x5555555555555555ull);
    check("divq remainder", rem == 1);
}

static void test_cpuid(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    char vend[13] = {0};
    memcpy(vend + 0, &b, 4);
    memcpy(vend + 4, &d, 4);
    memcpy(vend + 8, &c, 4);
    check("cpuid vendor string non-empty", vend[0] != 0);

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    check("cpuid family is x86-64-ish", ((a >> 8) & 0xF) != 0);
}

static void test_rdtsc(void)
{
    uint32_t lo1, hi1, lo2, hi2;
    __asm__ volatile("rdtsc" : "=a"(lo1), "=d"(hi1));
    for (volatile int i = 0; i < 100; i++)
        ;
    __asm__ volatile("rdtsc" : "=a"(lo2), "=d"(hi2));
    uint64_t t1 = ((uint64_t)hi1 << 32) | lo1;
    uint64_t t2 = ((uint64_t)hi2 << 32) | lo2;
    check("rdtsc is monotonic", t2 > t1);
}

int main(void)
{
    printf("asmtest: x86-64 instruction semantics\n");
    test_bswap();
    test_rotate_shift();
    test_muldiv();
    test_cpuid();
    test_rdtsc();
    printf("asmtest: %s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
