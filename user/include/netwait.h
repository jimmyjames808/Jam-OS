/* <netwait.h>: one thread waiting on many sockets at once, and on any
 * other handle it can wait on: a wait set (user/lib/netwait.c). It is what
 * poll, select and epoll will be built on (docs/M9.5-PLAN.md, track D, has
 * the mapping).
 *
 * ---- The model --------------------------------------------------------------
 * A set is one port. Each entry is
 *   - a socket: its rings as the program sees them (<sockring.h>), its
 *     `to_prog` event and its channel; or
 *   - a handle: any object with signals (a channel, an event, a process),
 *     and which of its signals mean readable, writable and hung up;
 * with an interest (NETWAIT_READ, NETWAIT_WRITE) and a pointer of the
 * caller's. netwait_wait returns the entries that are ready.
 *
 * **Level-triggered.** An entry is reported by every wait while its
 * condition holds, and readiness is computed from the object's state when
 * the set looks, never from signal bits: for a socket, the rings' counts,
 * the end flags and the status line; for a handle, its signals now. A
 * signal only says which entry to look at, so a signal that was coalesced,
 * came early or came twice can't lose readiness or invent it.
 *
 * **Pending and armed.** Each entry is either pending (looked at by the
 * next wait) or armed (asleep until a packet with its key). A new entry is
 * pending; a packet makes its entry pending; an entry reported ready stays
 * pending (it is looked at again next time: that is the level). A pending
 * entry found not ready is armed and leaves the list:
 *   - a socket: clear `to_prog`'s bits, raise the `waits` flags its
 *     interest needs (rx's consumer flag for READ, tx's producer flag for
 *     WRITE: sockring_sleep raises, fences and looks again), then look again.
 *     `to_prog` is bound PERSISTENT, so the next signal netstack makes (it
 *     signals only when it sees a flag up, after publishing) queues a
 *     packet. Ready after all: reported, flags lowered again;
 *   - a handle: nothing to do: it is bound PERSISTENT for the signals of
 *     its interest, and the kernel queues a packet on their next rise.
 * So a wait costs the packets that came and the entries still pending (the
 * ready ones), never one look at every entry: O(ready), not O(entries).
 *
 * **Why no wake is lost** (the socket rings' argument, <sockring.h>
 * "Waking"): the set raises a flag, fences and looks; netstack publishes,
 * fences and reads the flag; one of the two sees the other's write. The
 * event's bits are cleared before that last look, so a signal made after it
 * is a new edge and queues a packet. A packet from a signal made before it
 * only makes one look too many.
 *
 * **Keys.** A packet's key carries the entry's slot and generation; a
 * removed entry's generation changes, so its late packets are ignored. Each
 * binding owns one packet, so the port never holds more than two packets a
 * socket entry and one a handle entry, plus one netwait_wake.
 *
 * ---- What ready means ----------------------------------------------------------
 * For a socket (`ready` bits of struct netwait_ready):
 *   NETWAIT_READ    the rx ring has a datagram or a byte, or its end: a read
 *                   now doesn't wait (with interest READ only)
 *   NETWAIT_RX_END  the peer ended its direction (a stream's FIN; bytes may
 *                   still come before it): with interest READ only
 *   NETWAIT_WRITE   the socket is OPEN, its tx direction not ended, and the
 *                   tx ring has room for tx_need bytes (by default a
 *                   largest datagram, or one byte of a stream): with
 *                   interest WRITE only
 *   NETWAIT_HUP     the stream is CLOSED, or netstack is gone (the socket's
 *                   channel saw SIG_PEER_CLOSED): always reported
 *   NETWAIT_ERROR   the stream CLOSED with an error, or netstack is gone
 *                   (ERR_PEER_CLOSED), or a call on the entry's handles
 *                   failed: always reported, the reason in `error`
 * Once netstack is gone the set never touches that socket's rings again.
 * A datagram socket's refused sends are not reported (the status line
 * counts them, <sockring.h>).
 * For a handle: READ when any of its `read` signals is up, WRITE for
 * `write` (each with its interest), HUP for `hup` (always); ERROR if the
 * handle can't be waited on any more (`error`: ERR_BAD_HANDLE when it was
 * closed).
 *
 * ---- Rules --------------------------------------------------------------------
 *   - A set belongs to one thread: every call but netwait_wake is made from
 *     it. netwait_wake may come from any thread.
 *   - A socket is in at most one set, once. Its rings struct is the one
 *     the program reads and writes with; the set reads its counts and
 *     raises and lowers its `waits` flags.
 *   - While a socket is in a set, wait for it through the set: calls on it
 *     that don't wait are fine. A call that slept on the socket on its own
 *     (and so lowered the flags the set raised) must be followed by
 *     netwait_touch, or the set may sleep through its next signal.
 *   - Remove a socket (or handle) from the set before closing it: the set
 *     reads the socket's rings, and a binding holds its channel open in
 *     the kernel, so netstack would not see the socket close until the set
 *     is gone. netwait_destroy lowers the flags of the sockets still in it,
 *     so their rings must still be mapped then too.
 *
 * ---- Costs and limits --------------------------------------------------------------
 * At most NETWAIT_MAX entries a set. Each socket entry is two bindings
 * (`to_prog` and its channel), each handle entry one; a binding costs
 * about 1.2 KiB of the job's message bytes (JOB_LIMIT_MSG_BYTES) until it
 * is removed. A look at a socket reads its counts and status line (no
 * system call); arming one is one system call (clearing `to_prog`'s bits).
 * A look at a handle is one system call (its signals now). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <sockring.h>

#define NETWAIT_MAX 256u   /* entries a set may hold */

/* Interest (netwait_add_*, netwait_modify) and readiness (netwait_ready). */
#define NETWAIT_READ   (1u << 0)
#define NETWAIT_WRITE  (1u << 1)
#define NETWAIT_HUP    (1u << 2)   /* readiness only: always reported */
#define NETWAIT_ERROR  (1u << 3)   /* readiness only: always reported */
#define NETWAIT_RX_END (1u << 4)   /* readiness only: with NETWAIT_READ */
#define NETWAIT_INTEREST (NETWAIT_READ | NETWAIT_WRITE)

struct netwait;   /* a set: netwait_create */

/* A socket, as the program holds it. */
struct netwait_sock {
    struct sockring *rings;   /* its rings (sockring_attach'd): the program's own struct */
    handle_t to_prog;         /* netstack's event: RIGHT_WAIT and RIGHT_SIGNAL */
    handle_t ch;              /* the socket's channel: RIGHT_WAIT */
    uint32_t tx_need;         /* tx room that makes it writable; 0: a largest datagram
                               * (sockring_dgram_bytes(SOCKRING_DGRAM_MAX)) or 1 byte */
};

/* A handle, and which of its signals mean what. */
struct netwait_handle {
    handle_t  h;              /* RIGHT_WAIT */
    signals_t read;           /* readable when any is up (e.g. SIG_READABLE) */
    signals_t write;          /* writable when any is up (e.g. SIG_WRITABLE) */
    signals_t hup;            /* hung up when any is up (e.g. SIG_PEER_CLOSED) */
};

/* One ready entry. */
struct netwait_ready {
    uint32_t id;              /* the entry (netwait_add_*'s *out_id) */
    uint32_t ready;           /* NETWAIT_* bits */
    status_t error;           /* with NETWAIT_ERROR: why; else OK */
    uint32_t reserved;        /* 0 */
    void    *user;            /* the caller's pointer from netwait_add_* */
};

/* What a set did since it was made (for tests and `ps`-like tools). */
struct netwait_stats {
    uint64_t waits;           /* netwait_wait calls */
    uint64_t blocks;          /* times a wait found nothing ready and went to the port
                               * (it slept there unless its deadline had passed) */
    uint64_t packets;         /* packets taken off the port */
    uint64_t stale;           /* of those, for an entry already removed */
    uint64_t looks;           /* entries looked at */
    uint64_t arms;            /* socket entries armed (one system call each) */
};

/* A set for up to max_entries (1..NETWAIT_MAX) entries. ERR_INVALID_ARGS:
 * max_entries out of range; ERR_NO_MEMORY; the port's errors. */
status_t netwait_create(uint32_t max_entries, struct netwait **out);
/* Lower the flags of the sockets still in it, close its port (every
 * binding goes with it) and free it. NULL: nothing to do. */
void     netwait_destroy(struct netwait *w);

/* Add a socket with this interest (NETWAIT_INTEREST bits, 0 for HUP and
 * ERROR only); user comes back with each report. It is looked at by the
 * next wait. *out_id: its id (never 0). ERR_INVALID_ARGS: an unknown
 * interest bit, no rings, rings not attached, tx_need over the tx ring;
 * ERR_BAD_STATE: those rings are in the set already; ERR_NO_RESOURCES: the
 * set is full; a binding's error (ERR_ACCESS_DENIED: a handle without
 * RIGHT_WAIT; ERR_NO_MEMORY: the job's message bytes). */
status_t netwait_add_sock(struct netwait *w, const struct netwait_sock *s, uint32_t interest,
                          void *user, uint32_t *out_id);
/* Add a handle (its read, write and hup signals as above). Errors as
 * netwait_add_sock's, and ERR_INVALID_ARGS for no signals at all. */
status_t netwait_add_handle(struct netwait *w, const struct netwait_handle *h,
                            uint32_t interest, void *user, uint32_t *out_id);
/* A new interest for entry id; it is looked at again by the next wait.
 * ERR_NOT_FOUND: no such entry (removed); ERR_INVALID_ARGS: an unknown bit;
 * a handle's binding errors. */
status_t netwait_modify(struct netwait *w, uint32_t id, uint32_t interest);
/* Take entry id out: its bindings go, the flags the set raised in a
 * socket's rings are lowered (unless netstack is gone), and its late
 * packets are ignored. ERR_NOT_FOUND: no such entry. */
status_t netwait_remove(struct netwait *w, uint32_t id);
/* Look at entry id again at the next wait (after a call that slept on the
 * socket on its own). ERR_NOT_FOUND: no such entry. */
status_t netwait_touch(struct netwait *w, uint32_t id);
/* Entries in the set. */
uint32_t netwait_count(const struct netwait *w);

/* Wait until an entry is ready or the deadline (absolute ns, DEADLINE_NEVER:
 * none; one already past: look and return at once). OK: *out_n (1..max)
 * ready entries in out, oldest-pending first; entries ready beyond max stay
 * for the next wait. ERR_TIMED_OUT: nothing ready by the deadline;
 * ERR_CANCELED: netwait_wake (or the thread was cancelled) with nothing
 * ready; ERR_INVALID_ARGS: max 0. */
status_t netwait_wait(struct netwait *w, uint64_t deadline, struct netwait_ready *out,
                      uint32_t max, uint32_t *out_n);
/* From any thread: make the set's current wait, or its next one, return
 * (ERR_CANCELED if nothing is ready). Wakes before the wait returns
 * count as one. The port's errors (ERR_NO_MEMORY). */
status_t netwait_wake(struct netwait *w);

/* The set's counts (struct netwait_stats). */
void     netwait_get_stats(const struct netwait *w, struct netwait_stats *out);
