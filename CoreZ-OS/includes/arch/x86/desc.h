#ifndef ARCH_X86_DESC_H
#define ARCH_X86_DESC_H

#include <stdint.h>

extern void asm_lgdt(uint64_t gdtr_ptr);
extern void asm_reload_segments(void);
extern void asm_ltr(uint16_t sel);
extern uint16_t asm_str(void);

extern int detect_64bit(void);

#endif
