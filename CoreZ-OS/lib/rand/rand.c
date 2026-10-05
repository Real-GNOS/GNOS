#include "lib/rand/rand.h"
#include "arch/cpu.h"

static uint64_t rng_state;

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void rand_init(void) {
    uint64_t seed = cpu_rdtsc();

    for (int i = 0; i < 4; i++) {
        rng_state = splitmix64(&seed);
        seed = rng_state ^ cpu_rdtsc();
    }
}

uint64_t rand_u64(void) {
    uint64_t t = cpu_rdtsc();
    return splitmix64(&rng_state) ^ splitmix64(&t);
}

uint32_t rand_u32(void) {
    return (uint32_t)(rand_u64() >> 32);
}

void rand_bytes(void *buf, uint32_t len) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t i = 0;
    while (i < len) {
        uint64_t v = rand_u64();
        for (int b = 0; b < 8 && i < len; b++, i++) {
            p[i] = (uint8_t)(v >> (b * 8));
        }
    }
}
