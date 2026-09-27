/* SPDX-License-Identifier: GPL-2.0 */
/* gnoslibc/stdlib.h — the host <stdlib.h> ACPICA sees under
 * ACPI_USE_SYSTEM_CLIBRARY.  Memory goes through AcpiOsAllocate (kmalloc),
 * so there is nothing to declare beyond the character helpers the core
 * may reference; those live in kstring/utnonansi. */
#ifndef GNUCOSLIBC_STDLIB_H
#define GNUCOSLIBC_STDLIB_H

#include <stddef.h>

#endif
