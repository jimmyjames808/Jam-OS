/* The keep channel: kernel objects a service made outlive its process
 * (user/lib/keep.c; docs/M11.6-PLAN.md, "The keep channel").
 *
 * When a process dies the kernel closes every handle it held, and a
 * channel end with no handle left closes: its clients would see
 * ERR_PEER_CLOSED. So a service that must survive its own death hands a
 * duplicate of each handle a client holds the other end of (an open
 * file's channel and buffer VMO, a stream's channel, ring and event) to
 * its supervisor, the keeper, which hands them all back to the next
 * instance. Clients' handles never change, so clients never notice.
 *
 * The protocol is written by hand, like the namespace's NS_SET, because
 * it carries handles in one-way messages (no txid, no reply), which the
 * IDL doesn't do. One channel, both directions: the service's end is its
 * startup handle SR_KEEP, the keeper holds the other.
 *
 *   service -> keeper:   KEEP_PUT (a slot and its handles), KEEP_DROP (a slot)
 *   keeper -> successor: KEEP_RESTORE (whole slots, up to KEEP_BATCH handles
 *                        a message), then one KEEP_DONE
 *
 * A slot is a number the service chooses (an index into its own tables)
 * naming up to KEEP_SLOT_HANDLES handles that come and go together.
 *
 * **Order makes it safe without a reply.** The service sends KEEP_PUT
 * before it records the slot in its state (and before it answers the
 * client), and KEEP_DROP after it has forgotten the slot. A death between
 * the two leaves the keeper holding a slot the state doesn't know: the
 * successor closes those handles (keep_restore asks it slot by slot, and
 * sends KEEP_DROP for the ones it refuses). A slot the state knows but the
 * keeper didn't return (it refused the put) is dropped from the state by
 * the successor.
 *
 * **The keeper's rules** (keeper_take): it takes channels, VMOs and events
 * only (no dma_cap, interrupt, resource, process, thread, job, port,
 * timer or vmar: those carry authority a successor must get again from its
 * supervisor, or nothing a client holds), each able to be duplicated (it
 * hands out duplicates and keeps its own); never widens rights (a restored
 * handle has exactly the rights the service sent); at most KEEP_MAX_SLOTS
 * slots and KEEP_MAX_HANDLES handles per service. A put that breaks a rule
 * is refused whole: its handles are closed and counted. The service isn't
 * told (there is no reply); its slot is simply not restored. The kept
 * handles live in the keeper's handle table, charged to its job.
 *
 * Telling the type apart: there is no system call that names a handle's
 * type, so the keeper asks three questions that change nothing:
 * vmo_get_size works only on a VMO, event_signal with nothing to set or
 * clear only on an event, and only a channel end ever has SIG_WRITABLE or
 * SIG_PEER_CLOSED (it has one of them unless its peer's queue is full;
 * then, or without RIGHT_WAIT to look, it is refused: fail closed).
 * utest's keep_refusals pins this; a system call that names a handle's
 * type would make it exact.
 *
 * Neither side is thread-safe: a keeper and a service's keep channel are
 * each used by one thread at a time. */
#pragma once

#include <os.h>

#define KEEP_MAX_SLOTS    256u   /* slots a keeper holds for one service */
#define KEEP_MAX_HANDLES  256u   /* handles a keeper holds for one service, all slots together */
#define KEEP_SLOT_HANDLES 4u     /* handles in one slot */
#define KEEP_BATCH        64u    /* handles in one KEEP_RESTORE (a channel message's most) */

#define KEEP_PUT     1u   /* service -> keeper: slot, count; carries the count handles */
#define KEEP_DROP    2u   /* service -> keeper: slot; no handles */
#define KEEP_RESTORE 3u   /* keeper -> successor: struct keep_restore */
#define KEEP_DONE    4u   /* keeper -> successor: slot = slots restored, count = handles */

/* KEEP_PUT, KEEP_DROP and KEEP_DONE. */
struct keep_msg {
    uint32_t txid;    /* 0: not a call */
    uint32_t kind;    /* KEEP_PUT, KEEP_DROP or KEEP_DONE */
    uint32_t slot;    /* the slot; KEEP_DONE: how many slots the restore had */
    uint32_t count;   /* KEEP_PUT: handles carried; KEEP_DROP: 0; KEEP_DONE: handles restored */
};

/* One KEEP_RESTORE: whole slots, their handles in order, slot[0]'s first.
 * Sent only as long as the slots it uses: KEEP_RESTORE_SIZE(nslots). */
struct keep_restore {
    uint32_t txid;       /* 0 */
    uint32_t kind;       /* KEEP_RESTORE */
    uint32_t nslots;     /* entries of slot[] used, 1 to KEEP_BATCH */
    uint32_t reserved;   /* 0 */
    struct {
        uint32_t slot;   /* the service's slot number */
        uint32_t count;  /* its handles, 1 to KEEP_SLOT_HANDLES */
    } slot[KEEP_BATCH];
};
#define KEEP_RESTORE_SIZE(nslots) (16u + (nslots) * 8u)

/* ---- the service's side --------------------------------------------------------- */

/* Send the keeper duplicates of hs[0..n) (same rights; ours stay ours) as
 * slot `slot`, replacing what it held there. Call it before recording the
 * slot in the state. ERR_INVALID_ARGS: n is 0 or over KEEP_SLOT_HANDLES;
 * a handle that can't be duplicated or sent: that error; the keep
 * channel's queue full: ERR_SHOULD_WAIT (the slot isn't kept: a service
 * refuses what it would have kept, fail closed). Nothing is sent on an
 * error. */
status_t keep_put(handle_t keep, uint32_t slot, const handle_t *hs, unsigned n);
/* The keeper forgets slot (closes its duplicates). Call it after the
 * state has forgotten it. Errors as channel_write's. */
status_t keep_drop(handle_t keep, uint32_t slot);

/* What keep_restore hands over: take(ctx, slot, hs, n) for each slot the
 * keeper returned. Return true if the state knows the slot: the n
 * handles are then the caller's. false: keep_restore closes them and tells
 * the keeper to drop the slot. */
typedef bool (*keep_take_fn)(void *ctx, uint32_t slot, const handle_t *hs, unsigned n);

struct keep_restored {
    uint32_t slots;     /* slots take() was given */
    uint32_t taken;     /* ... and kept */
    uint32_t handles;   /* handles in the slots it kept */
};

/* The successor's side: read the keeper's KEEP_RESTORE messages on keep
 * up to its KEEP_DONE (waiting until deadline_ns), handing each slot to
 * take(). *out (may be NULL) says what came. Every handle comes with a
 * slot that is either taken or closed, also on an error. ERR_INVALID_ARGS:
 * a malformed message (its handles closed; slots taken before it are the
 * caller's: a service that sees an error starts fresh and closes them);
 * ERR_TIMED_OUT, ERR_PEER_CLOSED: no KEEP_DONE came. */
status_t keep_restore(handle_t keep, uint64_t deadline_ns, keep_take_fn take, void *ctx,
                      struct keep_restored *out);

/* ---- the keeper's side ----------------------------------------------------------- */

struct keep_slot {
    uint32_t id;                       /* the service's slot number */
    uint32_t n;                        /* handles in h[]; 0: this entry is free */
    handle_t h[KEEP_SLOT_HANDLES];     /* our duplicates */
};

struct keeper {
    handle_t         ch;          /* our end of the keep channel; HANDLE_INVALID: none */
    unsigned         nslots;      /* entries of slot[] in use (n != 0) */
    unsigned         nhandles;    /* handles held, all slots */
    struct keep_slot slot[KEEP_MAX_SLOTS];
    /* Counted since keeper_init. */
    uint32_t         puts;        /* KEEP_PUTs kept */
    uint32_t         drops;       /* KEEP_DROPs of a slot we held */
    uint32_t         replaced;    /* KEEP_PUTs of a slot we already held (the old one closed) */
    uint32_t         refused;     /* messages refused (handles closed) */
    uint32_t         lost;        /* slots dropped because they couldn't be restored */
};

/* An empty keeper with no channel. */
void     keeper_init(struct keeper *k);
/* A new keep channel: ours in k->ch, the service's end in *service_end
 * (its SR_KEEP, for a new instance). An old channel is drained first
 * (what the dead instance sent before it died still counts) and closed.
 * The slots stay. */
status_t keeper_attach(struct keeper *k, handle_t *service_end);
/* Take one message off k->ch and apply it (a refused one is counted and
 * its handles closed). OK: one was taken; else the read's status
 * (ERR_SHOULD_WAIT: none queued; ERR_PEER_CLOSED: the service is gone and
 * its queue empty). For a loop: bind k->ch for SIG_READABLE |
 * SIG_PEER_CLOSED. */
status_t keeper_take(struct keeper *k);
/* keeper_take until nothing is left: the service has died, its queue is
 * the rest of what it said. */
void     keeper_drain(struct keeper *k);
/* Hand the successor duplicates of every slot on k->ch (KEEP_RESTORE
 * batches, then KEEP_DONE), after keeper_attach and after the service end
 * was sent to it (the kernel won't send a channel end whose queue holds
 * channel ends). A slot whose handles can't be sent is closed and counted
 * as lost, not retried. ERR_BAD_STATE: no channel; other errors (out of
 * message bytes): the successor must start fresh (keeper_release). */
status_t keeper_restore(struct keeper *k);
/* Close every slot and the channel: the service was given up on, retired
 * or stopped in order; its clients see ERR_PEER_CLOSED. */
void     keeper_release(struct keeper *k);
