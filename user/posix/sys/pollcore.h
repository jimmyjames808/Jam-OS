/* What select.c needs of poll.c (poll_run), in a header of its own. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POLL_FDS_MAX 1024u   /* pollfds a call may pass: RLIMIT_NOFILE's answer */

struct pollfd;

/* poll on n pollfds (at most 1024) until the deadline (ns of uptime,
 * DEADLINE_NEVER: none): the pollfds ready, or -errno. */
long poll_run(struct pollfd *fds, unsigned long n, uint64_t deadline);
/* fd is an open descriptor. */
bool poll_fd_open(int fd);
/* The calling thread's room for 1024 pollfds (select's), or NULL: no
 * memory. */
struct pollfd *poll_scratch(void);
/* A deadline for a timeout of sec seconds and nsec nanoseconds: 0, or
 * -EINVAL for a negative one or nsec out of range. Too far: none. */
long ready_deadline(long sec, long nsec, uint64_t *out);
/* Set the signal mask for a wait (ppoll, pselect, epoll_pwait): mask
 * (a sigset_t of size bytes) or NULL for no change; *old gets the mask
 * that ready_mask_restore puts back. 0, or -errno. */
long ready_mask_set(const void *mask, long size, uint64_t *old);
/* After the wait, which returned r: with a mask, a wait ended by a signal
 * (EINTR) runs its handlers under the wait's mask, then the old mask is
 * back (signal.c's sig_wait_unmask). */
void ready_mask_restore(const void *mask, uint64_t old, long r);
