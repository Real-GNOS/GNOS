/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kdev_t.h — device number layout with major/minor accessors. (GPLv2)
 *
 * Ported from the Uinxed driver-model base (kernel/kdev_t.h): the kernel
 * carries dev_t in the Linux x86-64 layout (12-bit major, 20-bit minor),
 * while stat(2)/TIOCGDEV report the older userspace encoding carried in
 * vfs_node_t.rdev.  The two conversions live with the device model.
 */
#ifndef GNUCOS_KDEV_T_H
#define GNUCOS_KDEV_T_H

#include <stdint.h>

/* Linux x86-64 device number: 12-bit major, 20-bit minor. */

#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)

#define MAJOR(dev)    ((uint32_t)((dev) >> MINORBITS))
#define MINOR(dev)    ((uint32_t)((dev) & MINORMASK))
#define MKDEV(ma, mi) ((((uint32_t)(ma) & 0xFFFU) << MINORBITS) | ((uint32_t)(mi) & MINORMASK))

typedef uint32_t dev_t;

_Static_assert(sizeof(dev_t) == 4, "Linux dev_t is 32-bit");

/* User-visible layout (glibc makedev), as stat and TIOCGDEV report it. */
uint32_t dev_encode_uapi(dev_t dev);

/* Convert a userspace (uapi) device number into the kernel layout. */
dev_t dev_decode_uapi(uint64_t dev);

#endif /* GNUCOS_KDEV_T_H */
