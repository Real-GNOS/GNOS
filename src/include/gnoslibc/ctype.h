/* SPDX-License-Identifier: GPL-2.0 */
/* gnoslibc/ctype.h — the host <ctype.h> ACPICA sees under
 * ACPI_USE_SYSTEM_CLIBRARY: inline character classification, locale-free. */
#ifndef GNUCOSLIBC_CTYPE_H
#define GNUCOSLIBC_CTYPE_H

static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isspace(int c)  { return c == ' ' || (c >= 0x09 && c <= 0x0D); }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f')
                                       || (c >= 'A' && c <= 'F'); }
static inline int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }
static inline int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }

#endif
