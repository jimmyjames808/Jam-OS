/* Readiness: what poll.c and epoll.c share (docs/M13-PLAN.md section 7),
 * over the descriptor kinds' ready and arm (fd.h's struct fd_ops).
 *
 * ---- The model --------------------------------------------------------------
 * poll and epoll wait on wait sets (<netwait.h>: level-triggered,
 * O(ready)). A kind's arm says what a set waits on for one of its
 * descriptions: a handle (which of its signals mean readable, writable
 * and hung up) or a socket's rings: one set entry. A kind with no arm is
 * always ready, for reading and writing as its access mode allows (a
 * file, /dev/null, an /etc file, a memfd: Linux answers the same for
 * them); epoll refuses it (EPERM, as Linux's epoll_ctl refuses a regular
 * file). A kind's ready says what libc itself holds: the bytes of a
 * pipe's message that a read took off the channel and kept.
 *
 * **Why no wake is lost** to libc's own state. A waiter looks at its set
 * (a past deadline), then at every description's ready, then sleeps in
 * netwait_block, which returns once a packet is queued and takes none.
 * Bytes come into libc's keeping only off a channel; if they came after
 * the set's look, the channel's rise queued a packet, so the sleep
 * returns at once and the next round sees them. The one change with no
 * such rise, a socketpair end's shutdown (its arm's channel closed),
 * wakes every waiter itself (ready_wake_all).
 *
 * **What a kind must keep to.** ready makes no system call and takes no
 * lock (it runs under epoll's lock, from any thread). arm gives the
 * handles the description holds (with RIGHT_WAIT), the same ones for as
 * long as it lives: a set keeps a binding from call to call while the
 * description's arm gives the same answer. An arm that returns an error
 * makes the call fail with it (an epoll description's: EINVAL, since a
 * port can't be bound to a port).
 *
 * **Descriptions that go.** Each description has a serial (fd.c's
 * fd_new: never 0, never given twice), so a set never mistakes a new
 * description for one freed at the same address whose handle had the
 * same value. When a description's last reference goes, fd_put calls
 * epoll_forget before the kind's close, which takes it out of every epoll
 * set (Linux's rule: an epoll item lives until every descriptor of its
 * description is closed). poll's per-thread sets need no such call: a
 * closed handle's bindings end in the kernel, and the place, keyed by the
 * serial, just ages out.
 *
 * ---- Bits ---------------------------------------------------------------------
 * Linux's POLL* values (musl's <poll.h>), which EPOLL* share. A set's
 * READ is POLLIN | POLLRDNORM, WRITE POLLOUT | POLLWRNORM, a socket's
 * RX_END POLLRDHUP, ERROR POLLERR, and HUP POLLHUP, but POLLERR for a
 * handle with no read signals (a pipe's write end whose reader is gone,
 * as Linux says it). POLLPRI is never ready: Jam OS has no out-of-band
 * data. */
#pragma once

#include "fd.h"
#include "pollcore.h"   /* ready_deadline, ready_mask_set and _restore */

/* The interest (NETWAIT_READ, NETWAIT_WRITE) for POLL* events. */
uint32_t ready_interest(uint32_t events);
/* POLL* bits always ready for d, whose kind has no arm. */
uint32_t ready_always(const struct fd_desc *d);
/* A set's report (NETWAIT_* bits) about what w waits on as POLL* bits. */
uint32_t ready_bits(uint32_t netwait_bits, const struct fd_wait *w);
/* The kind's own readiness of d (ops->ready) as POLL* bits. */
uint32_t ready_libc(struct fd_desc *d, uint32_t interest);
/* Add what w waits on to set s with this interest (and user): its id. */
status_t ready_add(struct netwait *s, const struct fd_wait *w, uint32_t interest, void *user,
                   uint32_t *out_id);
/* d's arm for interest into *out (zeroed first: callers compare it whole):
 * 0, or the kind's -errno. Only for a kind with an arm. */
long     ready_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out);
/* A deadline (ns of uptime) for a timeout of ms milliseconds, negative:
 * none (DEADLINE_NEVER). */
uint64_t ready_deadline_ms(long ms);
/* Wake every poll, select and epoll_wait of this process asleep now, so
 * each looks again ("Why no wake is lost" above): for a kind whose own
 * readiness rose with no signal on what its arm gave (a socketpair end's
 * shutdown closes the channel the sets wait on). A waiter lists itself
 * before the look that decides it sleeps, so one woken late still sees
 * the change. Its halves: poll.c's sets asleep, epoll.c's with waiters. */
void     ready_wake_all(void);
void     poll_wake_all(void);
void     epoll_wake_all(void);

/* The epoll kind (epoll.c): its description holds a struct epoll. */
bool     epoll_is(const struct fd_desc *d);
