/* SPDX-License-Identifier: GPL-2.0 */
/*
 * string.h — the shim FatFs needs: it includes <string.h> for memcpy,
 * memset and strlen, and a freestanding kernel has no libc headers to
 * offer.  Everything it asks for already exists in the kernel's own
 * kstring.h, so this file just points there.  It lives in the vendor
 * directory so it is only visible to FatFs itself.
 */
#ifndef GNUCOS_FATFS_STRING_SHIM_H
#define GNUCOS_FATFS_STRING_SHIM_H

#include <stddef.h>
#include "kstring.h"

#endif
