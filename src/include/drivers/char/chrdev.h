/* SPDX-License-Identifier: GPL-2.0 */
/*
 * chrdev.h — character device registry.  (GPL-2.0)
 * Drivers announce devices through cdev_add_vfs() (native GNOS vfs_ops_t)
 * or cdev_add() (Uinxed tmpfs_device_ops_t); chrdev_populate() walks the
 * registry at boot and publishes each /dev node through the matching
 * backend.  The tmpfs backend lives in drm_devtmpfs.h.
 *
 */

#ifndef INCLUDE_DRIVERS_CHAR_CHRDEV_H_
#define INCLUDE_DRIVERS_CHAR_CHRDEV_H_

#include <drivers/gpu/drm/drm_devtmpfs.h> /* tmpfs_device_ops_t, MKDEV, file_stream */

/* A static character device. */
typedef struct cdev {
    char               dir[64];  /* /dev-relative parent dir ("" for the root) */
    char               name[64]; /* leaf node name, e.g. "null", "ttyS0" */
    uint32_t           major;
    uint32_t           minor_base;
    uint32_t           count;
    uint16_t           node_type; /* file_stream, ... */
    uint16_t           mode;      /* node mode; 0 selects the driver default */
    uint16_t           uid;
    uint16_t           gid;
    tmpfs_device_ops_t ops;   /* Uinxed bridge backend */
    const vfs_ops_t   *vops;  /* native GNOS backend; non-NULL wins */
    void              *vpriv; /* node priv for the native backend */
    struct cdev       *next;
} cdev_t;

/* Register a static character device (creates the node in chrdev_populate). */
int cdev_add(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count,
             uint16_t node_type, uint16_t mode, const tmpfs_device_ops_t *ops);

/* Register a static character device with explicit ownership. */
int cdev_add_owned(const char *dir, const char *name, uint32_t major, uint32_t minor,
                   uint32_t count, uint16_t node_type, uint16_t mode, uint16_t uid, uint16_t gid,
                   const tmpfs_device_ops_t *ops);

/* Remove a static character device by its full /dev path. */
int cdev_del(const char *path);

/* Register a character device with GNOS's native vfs_ops_t backend (the
 * TTY path).  The entry joins the same registry the Uinxed-bridge devices
 * use; the /dev node is created by chrdev_publish_path / cdev_publish. */
int cdev_add_vfs(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count,
                 uint16_t mode, uint16_t uid, uint16_t gid, const vfs_ops_t *vops, void *vpriv);

/* Create the /dev node for one registry entry: (dir, name) addressed by
 * path, for devices that appear at runtime (pty slaves).  Returns 0 or a
 * negative errno. */
int cdev_publish_path(const char *dir, const char *name);

/* Copy the character device covering a device number; 0 when none matches. */
int chrdev_lookup(uint32_t major, uint32_t minor, cdev_t *out);

/* Copy the character device whose /dev path (relative to /dev) matches; 0 when
 * none does.  Used by the open path for permission checks. */
int chrdev_lookup_path(const char *rel, cdev_t *out);

/* Create every registered /dev node.  Returns the number created. */
int chrdev_populate(void);

/* Register the default in-kernel character devices (mem, kmsg). */
void chrdev_init(void);

/* drivers/char/mem.c — /dev/null, zero, full, random, urandom */
void memdev_init(void);

/* drivers/char/kmsg.c — /dev/kmsg */
void kmsgdev_init(void);

#endif /* INCLUDE_DRIVERS_CHAR_CHRDEV_H_ */
