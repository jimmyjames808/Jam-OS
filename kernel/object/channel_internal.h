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
    /* Threads waiting on ep[i] to be handed a message (its channel_call
     * callers and channel_read_wait readers). Written under ep[i]'s lock,
     * read without it by writers as a hint: whether a small message should
     * be built in a slot. */
    uint32_t          waiting[2];
};

/* The size of a message slot (struct thread's msg_slot): one allocation
 * holding the header, the handles and the bytes of a small message. */
#define CHAN_SLOT_SIZE 512

/* One queued message. The khandles follow the header, then the bytes. */
struct chan_msg {
    struct list_node node;      /* on the receiver's queue */
    uint32_t         nbytes;    /* message bytes */
    uint32_t         nhandles;  /* khandles carried */
    struct job      *job;       /* the sender's job, charged `charge` bytes (a reference) */
    uint64_t         charge;    /* bytes charged to job */
    bool             slot;      /* in a slot (CHAN_SLOT_SIZE bytes): only ever handed to a
                                 * waiting reader, never queued, never charged */
};

static inline struct khandle *msg_handles(struct chan_msg *m)
{
    return (struct khandle *)(m + 1);
}

static inline uint8_t *msg_bytes(struct chan_msg *m)
{
    return (uint8_t *)(msg_handles(m) + m->nhandles);
}

/* A thread waiting on an endpoint to be handed a message: inside
 * channel_call, for the reply carrying txid; or inside channel_read_wait
 * (`any`), for whatever message comes next, if it fits. The writer that
 * hands one over unlinks it. While a reader is listed, its endpoint's
 * queue is empty: a writer that queues a message there instead (it
 * didn't fit) unlinks and wakes every reader, so they read the queue in
 * order. */
struct chan_waiter {
    struct list_node node;          /* on the endpoint's callers list */
    uint32_t         txid;          /* the reply it waits for; 0 for a reader */
    bool             any;           /* a reader: takes any message that fits */
    uint32_t         bytes_cap;     /* a reader's room for bytes... */
    uint32_t         handles_cap;   /* ...and handles */
    struct thread   *thread;        /* the waiting thread */
    struct chan_msg *reply;         /* set by the writer that hands it a message (or, for
                                     * a reader, the one it took off the queue itself) */
    struct chan_msg *slot;          /* its free slot while it waits, or NULL: a writer
                                     * that hands over a message built in its own slot
                                     * takes this one in exchange (the endpoint's lock) */
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

/* A new message of b's bytes (copied straight in; no lock may be held)
 * and the handles, charged to the current job. txid, unless 0, replaces
 * the first 4 bytes. The handles are copied, not taken: the caller clears
 * its own array once the message is delivered. */
status_t chan_msg_new(const struct chan_bytes *b, uint32_t txid, const struct khandle *handles,
                      uint32_t nhandles, struct chan_msg **out);
/* As chan_msg_new, for a reader that waits for it: a message that fits is
 * built in the current thread's slot (no allocation once the thread has
 * one, no charge). It may only be handed to a waiting reader. */
status_t chan_msg_new_handed(const struct chan_bytes *b, uint32_t txid,
                             const struct khandle *handles, uint32_t nhandles,
                             struct chan_msg **out);
/* A queueable copy of slot message m (allocated and charged as
 * chan_msg_new's); m's slot goes back to the current thread. On failure m
 * is untouched. No locks held. */
status_t chan_msg_unslot(struct chan_msg *m, struct chan_msg **out);
/* Free a message's memory and credit its sender's job; a slot goes to the
 * current thread (or is freed if it has one). Its handles are the
 * caller's business. No locks held (the job reference may be the last). */
void chan_msg_free(struct chan_msg *m);
/* Free a message and every handle it still owns. No locks held. */
void chan_msg_drop(struct chan_msg *m);
/* Hand a message's contents to a reader: its bytes straight into b (no
 * lock may be held), its handles into handles (the reader owns them now);
 * then free it. A fault writing b: the message and its handles are
 * dropped, ERR_INVALID_ARGS. */
status_t chan_msg_deliver(struct chan_msg *m, const struct chan_bytes *b, struct khandle *handles);

/* channel.c: slots and waiters. */

/* The current thread's slot, taken out of the thread (NULL if it has
 * none); a free slot given back to it (freed if it has one already). */
struct chan_msg *chan_slot_take(void);
void chan_slot_keep(struct chan_msg *m);
/* With ch's lock held: list w as waiting on ch to be handed a message, or
 * take it off the list. */
void chan_waiter_list_locked(struct channel *ch, struct chan_waiter *w);
void chan_waiter_unlist_locked(struct channel *ch, struct chan_waiter *w);
/* A writer on ch (no lock needed): is a thread on the peer waiting to be
 * handed a message? A hint: the answer may change at once. */
bool chan_peer_waits(const struct channel *ch);
