/*
 * pty.h — UNIX98 pseudo-terminals. (GPLv2)
 *
 * /dev/ptmx is a cloning device: opening it allocates a fresh master/slave
 * pair and registers the slave as /dev/pts/N.  open_resolved() drives the
 * clone through pty_reattach_master(); see pty.c.
 */
#ifndef GNUCOS_PTY_H
#define GNUCOS_PTY_H

#include "vfs.h"

/* Was this node the ptmx clone point? */
int pty_is_ptmx(const vfs_node_t *n);

/* Was this node a /dev/pts/N slave?  Opening one counts towards "the slave
 * is open", which is what stops the master from reading EOF. */
int pty_is_pts(const vfs_node_t *n);
int pty_reattach_slave(vfs_node_t *n);

/* Turn a ptmx node into a live master: allocates the pair, registers the
 * slave, rewrites the node in place.  0 on success. */
int pty_reattach_master(vfs_node_t *n);

/* Register /dev/ptmx.  Call once at boot. */
void pty_init(void);

#endif
