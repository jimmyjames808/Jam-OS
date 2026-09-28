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
 * what the txid is; servers echo it back in their reply. */
#pragma once

#include <stdint.h>
#include <jam/handle.h>
#include <jam/object.h>
#include <jam/status.h>

#define CHANNEL_MAX_BYTES   65536
#define CHANNEL_MAX_HANDLES 64
#define CHANNEL_MAX_QUEUED  1024   /* messages waiting on one endpoint; write fails ERR_SHOULD_WAIT beyond */

struct channel;   /* embeds struct kobject first, type OBJ_CHANNEL */

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

/* Endpoints alive right now (for leak checks in tests). */
uint64_t channel_live_count(void);
