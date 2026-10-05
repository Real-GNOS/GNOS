#ifndef NT_FUTEX_H
#define NT_FUTEX_H

#include <stdint.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

#define EAGAIN 11
#define EINVAL 22
#define ENOSYS 38
#define ETIMEDOUT 110

int32_t sys_futex(uint32_t uaddr, uint32_t op, uint32_t val, uint32_t timeout,
                  uint32_t uaddr2, uint32_t val3);
void futex_init(void);

#endif
