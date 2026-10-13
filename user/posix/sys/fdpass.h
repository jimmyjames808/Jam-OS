/* Descriptor passing (fdpass.c): what a descriptor becomes when a socket
 * carries it to another process (SCM_RIGHTS), and what a handle that
 * arrives becomes here. Two tables, one per direction, keyed by kind:
 *
 *   descriptor -> handle   by the description's kind (its fd_ops):
 *       a memfd (memfd_create, shm_open)    a duplicate of its kept VMO,
 *                                           with the description's rights
 *                                           (narrowed by the policy)
 *       a pipe end, a socketpair end        a duplicate of its channel end,
 *                                           with the end's own rights
 *   handle -> descriptor   by the object's type (handle_info):
 *       a VMO                               a memfd on it: read only unless
 *                                           the handle may write; its size
 *                                           the VMO's
 *       a channel end                       a pipe end that reads or writes
 *                                           as the handle's rights say, or a
 *                                           socketpair end when it does both
 *
 * Duplicates, never moves: the sender's descriptor stays as it was, and
 * may be shared (dup) or closed right after the send (libwayland does).
 * Bytes a pipe end's description already took off its channel and keeps
 * (pipe.c's leftover) stay with the sender's description: the receiver
 * reads from the channel on. (Linux shares the description itself.)
 * A VMO that comes without RIGHT_DUPLICATE (the compositor's keymap) is
 * copied into a kept VMO of ours, read only, when it is at most
 * FDPASS_COPY_MAX bytes: a memfd's mappings duplicate its VMO, so the
 * copy is what lets the receiver mmap it (MAP_PRIVATE copies anyway).
 *
 * Each socket kind gives a policy: the kinds it lets cross and the rights
 * a memfd's VMO goes with. The Wayland socket (wlsock.c) passes memfds,
 * as pools the compositor can read and map but not write, and channel
 * ends, and nothing else. Descriptor passing on socketpairs widens the
 * tables to every kind the POSIX hand-over can pass (<posixfds.h>: files,
 * eventfds, /dev nodes) by adding rows and FDPASS_* bits here, so there
 * is one translation and every socket kind uses it.
 *
 * The SCM_RIGHTS helpers at the end read and write the control data of a
 * struct msghdr as Linux lays it out (CMSG_*). Nothing here takes a lock
 * of its own; the descriptor table's calls take theirs. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "fd.h"

#define FDPASS_MEMFD    (1u << 0)        /* memfds and shm_open's files: a VMO */
#define FDPASS_CHANNEL  (1u << 1)        /* pipe ends and socketpair ends: a channel end */
#define FDPASS_COPY_MAX (1u << 20)       /* bytes of a VMO without RIGHT_DUPLICATE copied in */
#define FDPASS_MAX      253              /* descriptors in one sendmsg (Linux's SCM_MAX_FD) */

/* What a socket kind lets cross. */
struct fdpass_policy {
    unsigned kinds;          /* FDPASS_* bits, both ways */
    rights_t memfd_rights;   /* a memfd's VMO goes with at most these (RIGHT_SAME: all the
                              * description's) */
};

/* ---- one at a time -------------------------------------------------------------------- */

/* d as a handle to send: 0 with *out (a new handle, the caller's); -EINVAL
 * when d's kind is not among the policy's; else the duplicate's -errno. */
long     fdpass_to_handle(struct fd_desc *d, const struct fdpass_policy *p, handle_t *out);
/* The FDPASS_* bit of the object h names when it is among kinds, else 0
 * (another type, or no such handle). A system call, no lock. */
unsigned fdpass_kind_of(handle_t h, unsigned kinds);
/* h (consumed, whatever happens) as a new description with one
 * reference: 0 and *out; -EINVAL for a kind not among kinds, or rights
 * that make no descriptor (a VMO that can't be read, or can't be
 * duplicated and is too big to copy; a channel end that can neither read
 * nor write); -ENOMEM. */
long     fdpass_to_desc(handle_t h, unsigned kinds, struct fd_desc **out);

/* ---- a call's worth --------------------------------------------------------------------- */

/* The n descriptors fds as handles into hs, all of them or none: 0; the
 * first failure's -errno (-EBADF: not open; -EINVAL: a kind the policy
 * doesn't pass, said once a process in the log with the kind's name),
 * every handle made before it closed. */
long fdpass_fds_out(const int *fds, unsigned n, const struct fdpass_policy *p, handle_t *hs);

/* Where received descriptors go (fdpass_fds_in). */
struct fdpass_sink {
    int     *fds;       /* room entries */
    unsigned room;      /* how many may be installed */
    bool     cloexec;   /* FD_CLOEXEC on each (MSG_CMSG_CLOEXEC) */
    unsigned got;       /* out: how many were, into fds[0 .. got) */
    bool     cut;       /* out: a handle was closed unused (no room, a kind not passed, no
                         * memory, no free descriptor), as Linux sets MSG_CTRUNC */
};
/* The n handles hs (consumed) as descriptors, in order, into s. */
void fdpass_fds_in(handle_t *hs, unsigned n, unsigned kinds, struct fdpass_sink *s);

/* ---- SCM_RIGHTS --------------------------------------------------------------------------- */

/* The descriptors in m's control data (every SCM_RIGHTS block, in order)
 * into fds, at most max: how many; -EINVAL for a block of another kind
 * (SCM_CREDENTIALS, ...), a malformed one, or more than max. */
long     fdpass_cmsg_fds(const struct msghdr *m, int *fds, unsigned max);
/* How many descriptors one SCM_RIGHTS block in m's control room holds. */
unsigned fdpass_cmsg_room(const struct msghdr *m);
/* n descriptors (n at most fdpass_cmsg_room's) as one SCM_RIGHTS block in
 * m's control, msg_controllen set to the room it took (0 for none). */
void     fdpass_cmsg_put(struct msghdr *m, const int *fds, unsigned n);
