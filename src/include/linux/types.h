/*
 * linux/types.h — minimal shim so the vendored UAPI headers (sound/asound.h)
 * compile inside the freestanding kernel build. (GPLv2)
 */
#ifndef GNUCOS_LINUX_TYPES_H
#define GNUCOS_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

typedef unsigned char __kernel_u8_t;

typedef long    __kernel_off_t;
typedef long    __kernel_time_t;
typedef long    __kernel_suseconds_t;

typedef long    time_t;
typedef int     __kernel_pid_t;

/* struct timespec for the snd_pcm_status timestamps (x86_64 layout). */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* asound.h's zero-length padding typedefs (x86_64 little endian). */
#define __LITTLE_ENDIAN 1234
typedef char __pad_before_uframe[0];
typedef char __pad_after_uframe[0];

/* uapi ioctl encoding (asm-generic/ioctl.h) */
#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2
#define _IOC_NRMASK   ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK  ((1 << _IOC_DIRBITS) - 1)
#define _IOC_NRSHIFT  0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_TYPEBITS)
#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IOC_DIR(nr)  (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)   (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)
#define _IOC_READDIR  0U
#define _IOR(type, nr, size)  _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)  _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))
#define _IO(type, nr)         _IOC(_IOC_NONE, (type), (nr), 0)
#endif
