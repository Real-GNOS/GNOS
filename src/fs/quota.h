/*
 * quota.h — quotactl_fd(443). (GPLv2)
 */
#ifndef GNUCOS_QUOTA_H
#define GNUCOS_QUOTA_H

#include <stdint.h>

int64_t sys_quotactl_fd(uint64_t fd, uint32_t cmd, uint64_t id, uint64_t addr);

#endif
