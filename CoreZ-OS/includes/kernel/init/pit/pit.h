#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_CTRL 0x43
#define PIT_CNT0 0x40

#define PIT_HZ 100

void pit_init(uint32_t hz);
void mtime_sleep(uint32_t m_seconds);
int32_t mtime_sleep_interruptible(uint32_t m_seconds);

#endif
