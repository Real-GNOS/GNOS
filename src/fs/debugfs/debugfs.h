/*
 * debugfs.h — /debug, a read-only filesystem for kernel debugging. (GPLv2)
 *
 * Mirrors the Linux debugfs concept: every file is generated on read from
 * live kernel state (like /proc), but aimed at developers rather than user
 * space.  Drivers register files at boot; the tree is intentionally small
 * and deliberately not exposed through the normal boot paths.
 */
#ifndef GNUCOS_DEBUGFS_H
#define GNUCOS_DEBUGFS_H

#include "vfs.h"

/* Resolve an absolute path under /debug.  Returns 0 and fills `out` when the
 * path names a debugfs file or directory, -E_NOENT when it does not, and
 * -E_INVAL when the path is not under /debug at all. */
int debugfs_resolve(const char *path, vfs_node_t *out);

/* Enumerate a debugfs directory for getdents64.  `index` is the entry number
 * to report, starting at 0 (which is "."), and `name`/`type` are filled in.
 * Returns 0 on success or -E_NOENT once the directory is exhausted. */
int debugfs_readdir(const char *dirpath, uint32_t index, char *name,
                    uint8_t *type);

/* The string builder used by generators -- opaque to callers. */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t pos;
} debugfs_sbuf_t;

/* Register a generated file under /debug/<name>.  `gen` is called on every
 * read() to render fresh content into the scratch buffer.  Returns 0, or
 * -E_NOSPC when the file table is full. */
typedef void (*debugfs_gen_t)(debugfs_sbuf_t *s);
int debugfs_add_file(const char *name, debugfs_gen_t gen);

/* Register a static blob (immutable after boot) under /debug/<name>. */
int debugfs_add_blob(const char *name, const void *data, uint32_t size);

/* Call once at boot, before any user space runs. */
void debugfs_init(void);

#endif
