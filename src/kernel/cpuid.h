/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cpuid.h — SYS_cpuid: run the CPUID instruction on behalf of user space.
 * (GPLv2)
 */
#ifndef GNUCOS_CPUID_H
#define GNUCOS_CPUID_H

#include <stdint.h>

/* Executes CPUID(leaf, subleaf) and stores EAX/EBX/ECX/EDX through the
 * four user pointers (any of which may be 0 to skip). */
int64_t cpuid_syscall(uint64_t leaf, uint64_t subleaf, uint64_t a, uint64_t b,
                      uint64_t c, uint64_t d);

#endif
