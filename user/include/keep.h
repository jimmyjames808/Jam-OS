/* The keep channel: kernel objects a service made outlive its process
 * (user/lib/keep.c; docs/HISTORY.md, M11.6).
 *
 * When a process dies the kernel closes every handle it held, and a
 * channel end with no handle left closes: its clients would see
 * ERR_PEER_CLOSED. So a service that must survive its own death hands a
 * duplicate of each handle a client holds the other end of (an open
 * file's channel and buffer VMO, a stream's channel, ring and event) to
 * its supervisor, the keeper, which hands them all back to the next
 * instance. Clients' handles never change, so clients never notice.
 *
 * The messages are the IDL protocol keepch (abi/idl/keepch.idl, id 80):
 * events both ways, with handles, no txid and no reply. One channel: the
 * service's end is its startup handle SR_KEEP, the keeper holds the other.
 *
 *   service -> keeper:   put (a slot and its handles), drop (a slot)
 *   keeper -> successor: restore (whole slots, up to KEEP_BATCH handles
 *                        a message), then one done
 *
 * A slot is a number the service chooses (an index into its own tables)
 * naming up to KEEP_SLOT_HANDLES handles that come and go together.
 *
 * **Order makes it safe without a reply.** The service sends a put
 * before it records the slot in its state (and before it answers the
 * client), and a drop after it has forgotten the slot. A death between
 * the two leaves the keeper holding a slot the state doesn't know: the
 * successor closes those handles (keep_restore asks it slot by slot, and
 * sends a drop for the ones it refuses). A slot the state knows but the
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
 * handles live in the keeper's handle table, charged to its job. A message
 * that breaks the protocol's format (a drop with a handle, a put of more
 * than KEEP_SLOT_HANDLES, a txid) is refused by the generated server, its
 * handles closed, and counted as refused too.
 *
 * **A ring** (keep_put_ring): a slot whose second handle is an event a
 * client waits on (a watcher channel's bell) may ask the keeper to set
 * bits of SIG_USER_ALL on it when the keeper closes the slot itself (it
 * gives the service up, or can't hand the slot over), after the slot's
 * other handles are closed: the client wakes to find its channel closed.
 * A drop, or a put replacing the slot, rings nothing: the service ends
 * its channels itself.
 *
 * Telling the type apart: handle_info names it (and the handle's rights),
 * whatever the handle's other rights and state. utest's keep_refusals
 * pins this.
 *
 * Neither side is thread-safe: a keeper and a service's keep channel are
 * each used by one thread at a time. */
#pragma once

#include <idl/keepch.h>
#include <os.h>

#define KEEP_MAX_SLOTS    KEEPCH_MAX_SLOTS      /* slots a keeper holds for one service (1024) */
#define KEEP_MAX_HANDLES  KEEPCH_MAX_HANDLES    /* handles a keeper holds for one service, all slots (2048) */
#define KEEP_SLOT_HANDLES KEEPCH_SLOT_HANDLES   /* handles in one slot (4) */
#define KEEP_BATCH        KEEPCH_BATCH          /* handles in one restore event (64) */

/* ---- the service's side --------------------------------------------------------- */

/* Send the keeper duplicates of hs[0..n) (same rights; ours stay ours) as
 * slot `slot`, replacing what it held there. Call it before recording the
 * slot in the state. ERR_INVALID_ARGS: n is 0 or over KEEP_SLOT_HANDLES;
 * a handle that can't be duplicated or sent: that error; the keep
 * channel's queue full: ERR_SHOULD_WAIT (the slot isn't kept: a service
 * refuses what it would have kept, fail closed). Nothing is sent on an
 * error. */
status_t keep_put(handle_t keep, uint32_t slot, const handle_t *hs, unsigned n);
/* keep_put with a ring: bits of SIG_USER_ALL the keeper sets on hs[1] (an
 * event with RIGHT_SIGNAL, n at least 2) when it closes the slot itself.
 * ERR_INVALID_ARGS as well for a ring that can't be (the keeper would
 * refuse the put). */
status_t keep_put_ring(handle_t keep, uint32_t slot, signals_t ring, const handle_t *hs,
                       unsigned n);
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

/* The successor's side: read the keeper's restore events on keep
 * up to its done (waiting until deadline_ns), handing each slot to
 * take(). *out (may be NULL) says what came. Every handle comes with a
 * slot that is either taken or closed, also on an error. ERR_INVALID_ARGS:
 * a malformed message (its handles closed; slots taken before it are the
 * caller's: a service that sees an error starts fresh and closes them);
 * ERR_TIMED_OUT, ERR_PEER_CLOSED: no done came. */
status_t keep_restore(handle_t keep, uint64_t deadline_ns, keep_take_fn take, void *ctx,
                      struct keep_restored *out);

/* ---- the keeper's side ----------------------------------------------------------- */

struct keep_slot {
    uint32_t id;                       /* the service's slot number */
    uint32_t n;                        /* handles in h[]; 0: this entry is free */
    uint32_t ring;                     /* set on h[1] when we close it ourselves; 0: none */
    handle_t h[KEEP_SLOT_HANDLES];     /* our duplicates */
};

struct keeper {
    handle_t         ch;          /* our end of the keep channel; HANDLE_INVALID: none */
    unsigned         nslots;      /* entries of slot[] in use (n != 0) */
    unsigned         nhandles;    /* handles held, all slots */
    struct keep_slot slot[KEEP_MAX_SLOTS];
    /* Counted since keeper_init. */
    uint32_t         puts;        /* puts kept */
    uint32_t         drops;       /* drops of a slot we held */
    uint32_t         replaced;    /* puts of a slot we already held (the old one closed) */
    uint32_t         refused;     /* messages refused (handles closed) */
    uint32_t         handled;     /* messages the protocol's handlers ran for (put, drop) */
    uint32_t         lost;        /* slots dropped because they couldn't be restored */
    uint32_t         rung;        /* rings sounded (a slot closed by us with one) */
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
 * the rest of what it said. A loop that listens to a service still running
 * stops after CHANNEL_MAX_QUEUED so that a flood can't hold it: then true,
 * messages left. That is the loop's to come back to, for a binding fires
 * only on an edge (empty -> not empty), and a queue that is never found
 * empty makes none: the service would find it full for ever
 * (sup_kept_event re-queues its packet). keeper_drain_upto takes at most
 * `max`. */
bool     keeper_drain(struct keeper *k);
bool     keeper_drain_upto(struct keeper *k, unsigned max);
/* Hand the successor duplicates of every slot on k->ch (restore
 * batches, then done), after keeper_attach and after the service end
 * was sent to it (the kernel won't send a channel end whose queue holds
 * channel ends). A slot whose handles can't be sent is closed (its ring
 * sounded) and counted as lost, not retried. ERR_BAD_STATE: no channel;
 * other errors (out of message bytes): the successor must start fresh
 * (keeper_release). */
status_t keeper_restore(struct keeper *k);
/* Close every slot (each ring sounded) and the channel: the service was
 * given up on, retired or stopped in order; its clients see
 * ERR_PEER_CLOSED. */
void     keeper_release(struct keeper *k);
