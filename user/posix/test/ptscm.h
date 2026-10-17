/* The scm section's shared helpers (scm.c), its second file (scmkid.c:
 * Mojo's pattern between two processes, the fuzz loop) and its third
 * (scmcred.c: SO_PASSCRED, shutdown's wakes, FIONREAD). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jamos/libjam.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SCM_ROOM_MAX 256   /* descriptors one helper call sends or takes at most */

/* What a recvmsg gave: the descriptors, and msg_flags. */
struct scm_got {
    unsigned n;
    int      fds[SCM_ROOM_MAX];
    int      flags;
};

/* Handle units charged to our job now (a handle each, one more for a VMO),
 * net of libc's own mappings (maptable.h: each a VMO and its handle, 2
 * units): malloc's come and go with the heap (mallocng keeps a freed group
 * or gives it back as it sees fit), whatever else leaks or not. Read
 * without mem.c's lock: call it with no other thread mapping. */
uint64_t scm_job_handles(void);
/* The lowest free descriptor now. */
int      scm_lowest_fd(void);
/* sendmsg of n bytes at b with the nf descriptors fds (SCM_RIGHTS). */
ssize_t  scm_send(int s, const void *b, size_t n, const int *fds, unsigned nf, int flags);
/* recvmsg into b (n bytes) with control room for `room` descriptors (0: no
 * control at all) into *g. */
ssize_t  scm_recv(int s, void *b, size_t n, unsigned room, int flags, struct scm_got *g);
/* Close what g got. */
void     scm_close_got(struct scm_got *g);

/* scmkid.c */
void scm_mojo(void);
void scm_fuzz(void);

/* scmcred.c: what base's socket tests ask beyond the descriptors */
void scm_cred(void);
void scm_wake(void);
void scm_fionread(void);
/* base's control room (UnixDomainSocket::RecvMsgWithFlags): 16
 * descriptors and a ucred. */
#define SCM_CRED_ROOM (CMSG_SPACE(16 * sizeof(int)) + CMSG_SPACE(sizeof(struct ucred)))
/* What a recvmsg gave with `room` bytes of control (SCM_CRED_ROOM at
 * most): the bytes (or -1, errno set), SCM_CREDENTIALS's fields (pid -1:
 * none came), the descriptors that came (closed), msg_flags and
 * msg_controllen. */
struct scm_cred {
    ssize_t  n;
    pid_t    pid;
    uid_t    uid;
    gid_t    gid;
    unsigned nfds;
    int      flags;
    size_t   controllen;
};
struct scm_cred scm_recv_cred(int s, void *b, size_t n, int flags, size_t room);

/* The fuzz's makings (scmkid.c), which the peer section (peer.c) shares.
 * The generator: a 64-bit LCG, its state scm_seed (set it to repeat a
 * run); scm_rnd(n) is a number below n. */
extern uint64_t scm_seed;
uint32_t scm_rnd(uint32_t n);
/* A handle of a random kind: a kept VMO, a channel end, or an event. */
handle_t scm_any_handle(void);
/* The channel end h as a socketpair end's descriptor (seq: SOCK_SEQPACKET). */
int      scm_wrap(handle_t h, bool seq);
/* One raw message from `from`, mutated now and then (bytes changed, a
 * handle dropped, added or swapped for any kind, cut short, junk first),
 * into `to` (b: CHANNEL_MAX_BYTES of room). false: nothing more queued. */
bool     scm_forward(handle_t from, handle_t to, uint8_t *b);
/* A round's sends on x: random bytes and descriptors of the pool. */
void     scm_fuzz_send(int x, const int *pool, unsigned npool, bool seq, uint8_t *b);
