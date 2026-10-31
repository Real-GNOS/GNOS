/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mqueue.h — POSIX message queues (mq_open/mq_send/mq_receive/...). (GPLv2)
 *
 * Linux ABI: mq_open(240) returns a file descriptor into a kernel-private
 * queue table; musl's mq_send/mq_receive call mq_timedsend/mq_timedreceive
 * (242/243) with that descriptor, and the descriptor is also poll(2)able.
 * Struct layouts below are musl's x86-64 ABI (bits/mqueue.h), byte for
 * byte, because mq_getsetattr and mq_notify memcpy whole structs across
 * the user boundary.
 */
#ifndef GNUCOS_MQUEUE_H
#define GNUCOS_MQUEUE_H

#include <stdint.h>

/* musl's struct mq_attr (x86-64): four longs, exactly this order. */
typedef struct {
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
} mq_attr_t;

/* oflag bits beyond the O_* the caller already passes. */
#define MQ_O_CREAT  0100
#define MQ_O_EXCL   0200

/* sigev_notify values (musl bits/signal.h). */
#define SIGEV_NONE      0
#define SIGEV_SIGNAL    1
#define SIGEV_THREAD    2
#define SIGEV_THREAD_ID 4

/* Notify registration as copied from the user's struct sigevent: only the
 * fields the kernel acts on.  SIGEV_THREAD is delivered by musl's user-space
 * helper (it blocks in sigwait), so the kernel only ever sees NONE/SIGNAL/
 * THREAD_ID. */
typedef struct {
    int      notify;
    int      signo;
    uint64_t value;
    int      tid;
} mq_notify_t;

/* All syscalls are serialised by the big kernel lock, like sysvipc. */
int64_t sys_mq_open(uint64_t name, uint64_t oflag, uint64_t mode, uint64_t attr);
int64_t sys_mq_unlink(uint64_t name);
int64_t sys_mq_timedsend(uint64_t mqd, uint64_t msg, uint64_t len, uint64_t prio, uint64_t timeout);
int64_t sys_mq_timedreceive(uint64_t mqd, uint64_t msg, uint64_t len, uint64_t prio, uint64_t timeout);
int64_t sys_mq_notify(uint64_t mqd, uint64_t sevp);
int64_t sys_mq_getsetattr(uint64_t mqd, uint64_t newattr, uint64_t oldattr);

#endif
