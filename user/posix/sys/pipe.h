/* What pipe.c and sockpair.c share: a channel end's description (a pipe
 * end, or a socketpair end) and the helpers on it. pipe.c has pipes,
 * their reads and writes, and makes both kinds; sockpair.c has the
 * socketpair end's own calls (descriptors with the bytes, SOCK_SEQPACKET,
 * shutdown). Everything here is under the description's lock (fd.h)
 * unless it says otherwise; never across a wait. */
#pragma once

#include "kinds.h"

#define PIPE_SIZE    CHANNEL_MAX_BYTES   /* the most one message (and one atomic write) holds */
#define PIPE_ATOMIC  4096                /* PIPE_BUF, as musl's <limits.h> */

struct sp_fds;       /* sockpair.c's: descriptors on their way, or come with the leftover */
struct sp_sleeper;   /* sockpair.c's: a thread asleep in a read or write of the end */

struct pipe_end {
    handle_t ch;        /* the channel end; atomic (a socketpair end's shutdown swaps it
                         * for one whose peer is gone: pipe_ch) */
    uint8_t *buf;       /* PIPE_SIZE bytes once a message didn't fit a read, else NULL */
    uint32_t at, have;  /* buf[at .. have): read from the channel, not by the program;
                         * changed under the description's lock, stored atomically
                         * (the ready hooks read them without it) */
    /* socketpair ends only (sockpair.c) */
    lock_t         send_lock;   /* one send at a time (its parts stay together) */
    struct sp_fds *fds;         /* NULL until descriptors first come */
    bool           seq;         /* SOCK_SEQPACKET: a read takes one whole message */
    bool           kept;        /* seq: buf holds the next message (MSG_PEEK left it);
                                 * atomic */
    bool           shut;        /* shutdown(SHUT_RDWR) done; atomic */
    uint8_t        half;        /* a half-close's state (sockpair.c's HALF_*); atomic */
    bool           passcred;    /* SO_PASSCRED: SCM_CREDENTIALS with what is read; atomic */
    uint32_t       pid;         /* the leftover's writer (the kernel's stamp) */
    struct sp_sleeper *sleepers;   /* the threads asleep in its reads and writes, for
                                    * shutdown to wake */
};

static inline struct pipe_end *pipe_end_of(struct fd_desc *d)
{
    return d->priv;
}

/* The end's channel now (see ch above). No lock. */
static inline handle_t pipe_ch(const struct pipe_end *p)
{
    return __atomic_load_n(&p->ch, __ATOMIC_ACQUIRE);
}

/* A new end on ch (consumed, whatever happens) with len leftover bytes;
 * NULL: no memory. */
struct pipe_end *pipe_end_new(handle_t ch, const void *leftover, size_t len);
/* Wait for sig (or the peer's end) on the channel, no lock held: 0, or
 * -errno (-EINTR when a signal cut the wait short). */
long pipe_wait(handle_t ch, signals_t sig);
/* channel_read of the next message into buf (cap bytes): its size in
 * *got, its handles into hs (CHANNEL_MAX_HANDLES of room), how many in
 * *nh, and its writer's pid in *pid (NULL: not asked; channel_read's
 * CHANNEL_READ_PID). */
status_t pipe_read_msg(handle_t ch, void *buf, uint32_t cap, uint32_t *got, handle_t *hs,
                       uint32_t *nh, uint32_t *pid);
/* The leftover's buffer, made if it wasn't: false when out of memory. */
bool pipe_buf(struct pipe_end *p);
/* Up to n leftover bytes into iov: how many. */
long pipe_take_leftover(struct pipe_end *p, const struct iovec *iov, int iovcnt, size_t n);
/* The leftover is now buf[at .. have). */
void pipe_set_leftover(struct pipe_end *p, uint32_t at, uint32_t have);
/* What a child gets for the end (fd.h's give): a duplicate of the channel
 * end, the leftover moved to it as the record's data, as kind. */
long pipe_give_end(struct fd_desc *d, uint16_t kind, struct fd_given *out);
/* fork's hook for the end (fd.h's ops->fork): the parent puts a duplicate
 * of its channel end; the child takes it, its leftover emptied (those
 * bytes are the parent's description's to read). Whether the child keeps
 * it. */
bool pipe_end_fork(struct pipe_end *p, struct fd_fork *f);
/* The shared hooks: fstat as mode, poll's arm, and the end's last close
 * (its channel end and its buffer; a socketpair end's descriptors are
 * sockpair.c's to close first). */
void pipe_stat_as(struct fd_desc *d, struct stat *st, mode_t mode);
long pipe_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out);
void pipe_end_free(struct pipe_end *p);

/* sockpair.c: a socketpair end's description on p (consumed), seq for
 * SOCK_SEQPACKET; NULL: no memory (p freed). */
struct fd_desc *sockp_desc_new(struct pipe_end *p, uint32_t status, bool seq);
