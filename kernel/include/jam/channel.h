/* Channels: bidirectional message pipes made of two endpoints.
 *
 * A message is up to CHANNEL_MAX_BYTES of bytes plus up to
 * CHANNEL_MAX_HANDLES khandles. Writing to one endpoint queues the message
 * on the other (the peer), where channel_read takes it out in FIFO order.
 * Handles in a message are moved, not copied: the queued message owns them
 * until a reader takes them, or releases them if the endpoint closes first.
 *
 * Signals on an endpoint:
 *   SIG_READABLE     its queue is not empty
 *   SIG_WRITABLE     the peer still exists and its queue has room (below
 *                    CHANNEL_MAX_QUEUED): after ERR_SHOULD_WAIT from a
 *                    write, wait for this (or SIG_PEER_CLOSED)
 *   SIG_PEER_CLOSED  the peer's last handle went away, or it was destroyed
 *
 * An endpoint "closes" when its handle count drops to 0 or its last
 * reference goes (whichever is first). Closing tells the peer and releases
 * every message still queued on the closing endpoint.
 *
 * Transaction ids: the first 4 bytes of every message are a txid.
 * channel_call stamps a fresh non-zero txid on its request and takes the
 * reply whose first 4 bytes match it. Plain writes and reads do not care
 * what the txid is; servers echo it back in their reply.
 *
 * Copies. A message's bytes are copied once on the way in (from the
 * writer's buffer straight into the message) and once on the way out
 * (from the message straight into the reader's buffer). The _from / _into
 * forms take a struct chan_bytes, which names either kernel memory or
 * memory of the current process (the system calls' case); the plain forms
 * take kernel pointers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/handle.h>
#include <jam/object.h>
#include <jam/status.h>

#define CHANNEL_MAX_BYTES   65536
#define CHANNEL_MAX_HANDLES 64
/* Messages waiting on one endpoint; a write beyond that fails with
 * ERR_SHOULD_WAIT. */
#define CHANNEL_MAX_QUEUED  1024

struct channel;   /* embeds struct kobject first, type OBJ_CHANNEL */

/* Switch (the benchmark's off/on): a small message for a reader already
 * waiting for it (a reply to a channel_call, a request to a
 * channel_read_wait reader) is built in its writer's per-thread slot and
 * handed over with no allocation and no job charge (channel.c). Off,
 * every message is allocated and charged. Atomic loads and stores. */
extern bool channel_slots;

/* Where a message's bytes come from, or go to. A user address is copied
 * with copy_from_user / copy_to_user, outside every lock; a fault there
 * is ERR_INVALID_ARGS. */
struct chan_bytes {
    uint64_t addr;   /* a kernel pointer, or a user address of the current process */
    uint32_t len;    /* bytes to send, or room to receive */
    bool     user;   /* addr is a user address */
};

static inline struct chan_bytes chan_kbytes(const void *p, uint32_t len)
{
    return (struct chan_bytes){ (uint64_t)(uintptr_t)p, len, false };
}

static inline struct chan_bytes chan_ubytes(uint64_t uaddr, uint32_t len)
{
    return (struct chan_bytes){ uaddr, len, true };
}

/* Two connected endpoints, each with refs = 1 and no handles. */
status_t channel_create(struct channel **a, struct channel **b);

/* Queue a message on the PEER. On success the khandles are consumed (their
 * .obj set to NULL); on failure they are untouched and still the caller's.
 * Sending this channel's own endpoint or its peer, or any endpoint whose own
 * queue already holds an endpoint, is ERR_NOT_SUPPORTED: each could close a
 * reference cycle nobody can reach. Peer gone: ERR_PEER_CLOSED. Peer's
 * queue at CHANNEL_MAX_QUEUED: ERR_SHOULD_WAIT. A message whose txid matches
 * a channel_call waiting on the peer goes to that caller instead of the
 * queue (and does not count against the limit). */
status_t channel_write(struct channel *ch, const void *bytes, uint32_t nbytes,
                       struct khandle *handles, uint32_t nhandles);

/* Take the oldest message. Empty: ERR_SHOULD_WAIT, or ERR_PEER_CLOSED if the
 * peer is gone too. If the bytes or handles don't fit: ERR_BUFFER_TOO_SMALL
 * and the message stays queued. *actual_bytes / *actual_handles (either may
 * be NULL) get the message's sizes in both cases. */
status_t channel_read(struct channel *ch, void *bytes, uint32_t bytes_cap, uint32_t *actual_bytes,
                      struct khandle *handles, uint32_t handles_cap, uint32_t *actual_handles);

/* Synchronous RPC. Overwrites the first 4 bytes of wbytes (wn >= 4) with a
 * fresh txid, writes it (same rules as channel_write: on a failed write the
 * handles stay the caller's), then blocks until the reply with that txid
 * arrives on ch, deadline_ns passes (ERR_TIMED_OUT), the peer closes
 * (ERR_PEER_CLOSED), ch itself closes or the thread is cancelled
 * (ERR_CANCELED). The reply goes only to this caller, never to
 * channel_read. If it doesn't fit rbytes/rh, the call returns
 * ERR_BUFFER_TOO_SMALL with the sizes it needed, and the reply (with its
 * handles) is dropped. A reply that arrives after its caller gave up is
 * queued like any other message. Any number of threads may call at once. */
status_t channel_call(struct channel *ch, void *wbytes, uint32_t wn, struct khandle *wh,
                      uint32_t whn, void *rbytes, uint32_t rcap, uint32_t *ractual,
                      struct khandle *rh, uint32_t rhcap, uint32_t *rhactual,
                      uint64_t deadline_ns);

/* channel_write with the bytes in b (b->len of them). A fault reading
 * user memory: ERR_INVALID_ARGS, nothing sent, the handles still the
 * caller's. */
status_t channel_write_from(struct channel *ch, const struct chan_bytes *b,
                            struct khandle *handles, uint32_t nhandles);

/* channel_read into b (room: b->len). A fault writing user memory comes
 * after the message has left the queue: it is lost, its handles released,
 * and the read fails with ERR_INVALID_ARGS (a bad buffer never leaks a
 * handle). The bytes are written before the read returns, so a thread
 * killed inside it never loses a message it took: it acts on the kill
 * only on its way back to ring 3. */
status_t channel_read_into(struct channel *ch, const struct chan_bytes *b, uint32_t *actual_bytes,
                           struct khandle *handles, uint32_t handles_cap,
                           uint32_t *actual_handles);

/* One channel_call_with: the request, room for the reply, and the results. */
struct chan_call {
    struct chan_bytes req;       /* the request; req.len >= 4 */
    struct khandle   *req_h;     /* its handles: consumed (.obj NULL) once it is sent */
    uint32_t          req_nh;    /* how many */
    uint32_t          rep_hcap;  /* room for the reply's handles */
    struct chan_bytes rep;       /* room for the reply's bytes */
    struct khandle   *rep_h;     /* room for its handles */
    uint32_t          rep_nb;    /* out: the reply's bytes (with ERR_BUFFER_TOO_SMALL too) */
    uint32_t          rep_nh;    /* out: its handles (ditto) */
};

/* channel_call with the buffers of c. The message carries a fresh txid in
 * its first 4 bytes in place of req's: in kernel memory it is also written
 * into req (as channel_call does), user memory never gets it. A fault
 * reading the request: ERR_INVALID_ARGS, nothing sent. A fault writing
 * the reply: it is lost, its handles released, ERR_INVALID_ARGS. */
status_t channel_call_with(struct channel *ch, struct chan_call *c, uint64_t deadline_ns);

/* One channel_read_wait: room for the message, and its sizes. */
struct chan_read {
    struct chan_bytes buf;    /* room for its bytes */
    struct khandle   *h;      /* room for its handles: the reader owns them once read */
    uint32_t          hcap;   /* how many */
    uint32_t          nb;     /* out: its bytes (with ERR_BUFFER_TOO_SMALL too) */
    uint32_t          nh;     /* out: its handles (ditto) */
};

/* channel_read_into, waiting for a message if none is queued: until one
 * comes, deadline_ns passes (ERR_TIMED_OUT), the peer closes with nothing
 * queued (ERR_PEER_CLOSED), ch itself closes (ERR_BAD_STATE) or the thread
 * is cancelled (ERR_CANCELED). The queue is looked at before blocking, so
 * no read ever comes back empty-handed for a message that is there. While
 * it waits the thread is listed on ch with its free message slot on
 * offer, and a writer hands it the next message that fits r directly
 * (bypassing the queue; a small one built in the writer's slot is neither
 * allocated nor charged). A message that doesn't fit is queued, and read as
 * channel_read_into does: ERR_BUFFER_TOO_SMALL with its sizes, and it stays
 * queued. A message the reader was handed or took is wholly in r->buf
 * when this returns, even if the thread was cancelled meanwhile. No lock
 * held; a fault writing user memory is ERR_INVALID_ARGS, the message lost
 * (as channel_read_into). */
status_t channel_read_wait(struct channel *ch, struct chan_read *r, uint64_t deadline_ns);

/* The messages queued on ch now and what they are charged (bytes, plus
 * JOB_OBJECT_BYTES per carried handle): for debug_command "ps". */
void channel_queued(struct channel *ch, uint32_t *msgs, uint64_t *charged);

/* Endpoints alive right now (for leak checks in tests). */
uint64_t channel_live_count(void);
