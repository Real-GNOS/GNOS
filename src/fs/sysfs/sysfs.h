/*
 * sysfs.h — /sys, the kernel's exported object hierarchy. (GPLv2)
 *
 * A path-prefix pseudo-filesystem in the same family as /proc and /debug:
 * there is no backing store, resolve() matches registered attribute paths,
 * directories materialise as the path components of whatever was
 * registered.  Attributes render on read; some also accept writes.
 */
#ifndef GNUCOS_SYSFS_H
#define GNUCOS_SYSFS_H

#include "vfs.h"

/* Resolve an absolute path under /sys.  0 + filled node, -E_NOENT for an
 * unregistered leaf, -E_INVAL when the path is not under /sys at all. */
int sysfs_resolve(const char *path, vfs_node_t *out);

/* Enumerate /sys directories for getdents64, same contract as debugfs. */
int sysfs_readdir(const char *dirpath, uint32_t index, char *name,
                  uint8_t *type);

/* Register /sys/<relpath>.  `gen` renders the read content; `put`, when
 * non-NULL, receives user writes.  Intermediate directories materialise
 * automatically.  Returns 0, -E_INVAL, or -E_NOSPC when full. */
typedef void (*sysfs_gen_t)(char *buf, uint32_t cap, uint32_t *len);
typedef int  (*sysfs_put_t)(const char *buf, uint32_t len);
int sysfs_add_file(const char *relpath, sysfs_gen_t gen, sysfs_put_t put);

/* Boot-time registration of the built-in attributes. */
void sysfs_init(void);

#endif
