/* SPDX-License-Identifier: GPL-2.0 */
/*
 * process_vm_access.h — process_vm_readv(310)/writev(311). (GPLv2)
 */
#ifndef GNUCOS_PROCESS_VM_ACCESS_H
#define GNUCOS_PROCESS_VM_ACCESS_H

#include <stdint.h>

int64_t sys_process_vm_readv(uint64_t pid, uint64_t liov, uint64_t liovcnt,
                             uint64_t riov, uint64_t riovcnt, uint64_t flags);
int64_t sys_process_vm_writev(uint64_t pid, uint64_t liov, uint64_t liovcnt,
                              uint64_t riov, uint64_t riovcnt, uint64_t flags);

#endif
