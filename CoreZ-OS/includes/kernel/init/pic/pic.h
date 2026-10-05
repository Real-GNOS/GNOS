#include <stdint.h>

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10
#define ICW1_INIT_NEED_ICW4 (ICW1_INIT | ICW1_ICW4)

#define ICW4_8086 0x01

int pic_init(void);
void pic_send_eoi(uint8_t irq);
