#ifndef DRIVERS_BLOCK_IDE_H
#define DRIVERS_BLOCK_IDE_H

#include <stdint.h>

#include "drivers/block/block.h"
#include "kernel/sched/sync.h"

struct IDE_CHANNEL {
    char name[8];
    uint16_t port_base;
    uint8_t irq_no;
    struct SCHED_LOCK lock;
    int expecting_intr;
    struct SCHED_SEMAPHORE disk_done;
    struct DISK devices[2];
};

void ide_read(struct DISK *hd, uint32_t lba, void *buf, uint32_t sec_cnt);
void ide_write(struct DISK *hd, uint32_t lba, const void *buf, uint32_t sec_cnt);
void intr_hd_handler(uint8_t irq_no);
void ide_init(void);

extern struct IDE_CHANNEL channels[2];

#endif
