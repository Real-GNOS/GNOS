#ifndef RAND_H
#define RAND_H

#include <stdint.h>

void rand_init(void);
uint32_t rand_u32(void);
uint64_t rand_u64(void);
void rand_bytes(void *buf, uint32_t len);

#endif
