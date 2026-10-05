#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define AP_BOOT_INFO_ADDR 0x8000u
#define AP_TRAMPOLINE_ADDR 0x9000u
#define AP_TRAMPOLINE_VECTOR (AP_TRAMPOLINE_ADDR >> 12)

void smp_init(void);

extern unsigned char _binary_ap_trampoline_bin_start[];
extern unsigned char _binary_ap_trampoline_bin_end[];

#endif
