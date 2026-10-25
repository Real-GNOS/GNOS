/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STDDEF_H
#define _STDDEF_H

typedef unsigned int size_t;
typedef int          ptrdiff_t;
typedef long         ssize_t; /* signed counterpart of size_t (kobject/sysfs port) */

#define NULL ((void *)0)

/* container_of() and the DRM core's mode objects need offsetof.  GCC's
 * __builtin_offsetof is available even under -ffreestanding. */
#define offsetof(type, member) __builtin_offsetof(type, member)

/* Recover the enclosing structure from a pointer to one of its members.
 * Shared by the driver model (device, bus, class) and the DRM port; a few
 * DRM files still carry their own identical copy. */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#endif