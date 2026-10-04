/* What the channel's two files share, and nothing else includes:
 * channel.c (the endpoints, messages, closing and reading) and
 * channel_send.c (writing and channel_call). The model, the lock order
 * and what is charged to whom are in channel.c's header. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/channel.h>
#include <jam/list.h>
#include <jam/process.h>
#include <jam/spinlock.h>

struct chan_pair {
    spinlock_t        lock;    /* "channel pair": guards both endpoints' queues and state */
    struct channel   *ep[2];   /* NULL once that endpoint has closed */
    uint32_t          refs;    /* one per endpoint not yet destroyed */
};

/* One queued message. The khandles follow the header, then the bytes. */
struct chan_msg {
    struct list_node node;      /* on the receiver's queue */
    uint32_t         nbytes;    /* message bytes */
    uint32_t         nhandles;  /* khandles carried */
    struct job      *job;       /* the sender's job, charged `charge` bytes (a reference) */
    uint64_t         charge;    /* bytes charged to job */
};

static inline struct khandle *msg_handles(struct chan_msg *m)
{
    return (struct khandle *)(m + 1);
}

static inline uint8_t *msg_bytes(struct chan_msg *m)
{
    return (uint8_t *)(msg_handles(m) + m->nhandles);
}

/* A thread inside channel_call, waiting on its own endpoint for the reply
 * carrying txid. The writer that delivers the reply unlinks it. */
struct chan_waiter {
    struct list_node node;     /* on the endpoint's callers list */
    uint32_t         txid;     /* the reply it waits for */
    struct thread   *thread;   /* the caller */
    struct chan_msg *reply;    /* set by the writer that delivers it */
};

struct channel {
    struct kobject    base;          /* OBJ_CHANNEL */
    struct chan_pair *pair;          /* shared with the peer */
    uint32_t          side;          /* our index in pair->ep */
    struct list_node  queue;         /* chan_msg, oldest first */
    uint32_t          nqueued;       /* messages on queue (at most CHANNEL_MAX_QUEUED) */
    struct list_node  callers;       /* chan_waiter */
    bool              closed;        /* we left the pair */
    bool              peer_closed;   /* the peer left the pair */
};

/* channel.c: messages. */

/* Copy bytes and handles into a new message, charged to the current job.
 * The handles are copied, not taken: the caller clears its own array once
 * the message is delivered. */
status_t chan_msg_new(const void *bytes, uint32_t nbytes, const struct khandle *handles,
                      uint32_t nhandles, struct chan_msg **out);
/* Free a message's memory and credit its sender's job. Its handles are
 * the caller's business. No locks held (the job reference may be the last). */
void chan_msg_free(struct chan_msg *m);
/* Free a message and every handle it still owns. No locks held. */
void chan_msg_drop(struct chan_msg *m);
/* Hand a message's contents to a reader, which now owns the handles, and
 * free it. */
void chan_msg_deliver_to(struct chan_msg *m, void *bytes, struct khandle *handles);
