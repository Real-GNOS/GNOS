/* SPDX-License-Identifier: GPL-2.0 */
/* gnoslibc/stdio.h — the host <stdio.h> ACPICA sees under
 * ACPI_USE_SYSTEM_CLIBRARY: vsnprintf/snprintf come from the kernel's own
 * formatter (the same one DRM uses).  No FILE, no console I/O. */
#ifndef GNUCOSLIBC_STDIO_H
#define GNUCOSLIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int  vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
int  sprintf(char *buf, const char *fmt, ...);

#endif
