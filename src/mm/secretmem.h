/*
 * secretmem.h — memfd_secret(447). (GPLv2)
 */
#ifndef GNUCOS_SECRETMEM_H
#define GNUCOS_SECRETMEM_H

#include <stdint.h>

int64_t sys_memfd_secret(uint64_t flags);

#endif
