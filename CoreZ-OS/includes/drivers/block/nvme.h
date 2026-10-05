#ifndef DRIVERS_BLOCK_NVME_H
#define DRIVERS_BLOCK_NVME_H

#include <stdint.h>

#include "drivers/block/block.h"

struct NVME_QUEUE {
    uint64_t phys;
    uint32_t *virt;
    uint32_t nr;
    uint32_t head;
    uint32_t tail;
    uint32_t phase;
};

struct NVME_CTRL {
    volatile uint32_t *regs;
    uint32_t doorbell_stride;
    uint32_t nsid;
    uint32_t lba_shift;
    struct NVME_QUEUE asq;
    struct NVME_QUEUE acq;
    struct NVME_QUEUE iosq;
    struct NVME_QUEUE iocq;
    uint8_t *bounce;
    uint32_t bounce_phys;
    uint32_t prp_list_phys;
    uint64_t *prp_list;
    uint8_t *identify;
    uint32_t identify_phys;
    uint32_t cid;
};

int nvme_init(void);
int nvme_read_sectors(struct DISK *hd, uint32_t lba, void *buf, uint32_t count);
int nvme_write_sectors(struct DISK *hd, uint32_t lba, const void *buf,
                       uint32_t count);

#endif
