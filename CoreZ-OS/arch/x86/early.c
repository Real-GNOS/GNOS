#include "arch/early.h"
#include "arch/mmu.h"

void arch_early_init(void) {
    asm_write_cr4(asm_read_cr4() | 0x600);
    asm_write_cr0(asm_read_cr0() | 0x10000);
}
