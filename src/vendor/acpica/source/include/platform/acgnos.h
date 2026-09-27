/******************************************************************************
 *
 * Module Name: acgnos.h - GNOS kernel platform header
 * (Port-layer file; the ACPI core in source/components is upstream
 *  ACPICA under BSD-3-Clause OR GPL-2.0-only.)
 *
 *****************************************************************************/

#ifndef __ACGNOS_H__
#define __ACGNOS_H__

/* The compiler half: gcc types, endianness and inline expectations. */
#include "acgcc.h"

/* 64-bit kernel. */
#define ACPI_MACHINE_WIDTH          64

/* The kernel is single-threaded for ACPI purposes (everything runs under
 * the big kernel lock), so ACPICA's mutex/semaphore internals compile to
 * stubs and the OSL's are empty. */
#define ACPI_SINGLE_THREADED

/* The kernel provides its own string/memory (kstring) and formatter
 * (drm_vsnprintf), so ACPICA uses the system-library path with the
 * gnoslibc shim headers instead of shipping its utclib. */
#define ACPI_USE_SYSTEM_CLIBRARY

/* ACPICA calls the ctype helpers without including <ctype.h> itself; the
 * platform header is where they are expected to come from. */
#include "ctype.h"

/* The OSL owns the debug output (AcpiOsPrintf routes to the debug console);
 * ACPICA's internal DEBUG prints go through the same path. */
#define ACPI_DEBUG_OUTPUT

/* No 16-bit or 32-bit fallbacks; everything assumes the widths above. */
#define ACPI_64BIT_TIMEOUT

#endif /* __ACGNOS_H__ */
