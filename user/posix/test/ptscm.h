/* The scm section's shared helpers (scm.c) and its second file
 * (scmkid.c: Mojo's pattern between two processes, the fuzz loop). */
#pragma once

#include <stdint.h>
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
