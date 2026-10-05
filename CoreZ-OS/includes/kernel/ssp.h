#ifndef SSP_H
#define SSP_H

#include <stdint.h>

extern uintptr_t __stack_chk_guard;

void stack_canary_init(void);
void __stack_chk_fail(void) __attribute__((noreturn));

#endif
