/* A service's state VMO, and warm spares (user/lib/svcstate.c, the
 * standby half of user/lib/start.c; docs/M11.6-PLAN.md, "The state VMO",
 * "The request in progress" and "The warm spare").
 *
 * **The state VMO** holds what a service knows (open files, positions,
 * what it holds for the disk), so that a successor carries on where a
 * dead instance stopped. The supervisor makes it (svcstate_create), keeps
 * its own handle without ever mapping it, and hands each instance one
 * with read, write and map only (SR_STATE; SVCSTATE_SERVICE_RIGHTS): no
 * resize, transfer or duplicate, so the service can't pass it on. Its
 * pages are charged to the supervisor's job. The service maps it at
 * SVCSTATE_ADDR before anything else (svcstate_open), so pointers inside
 * it stay valid from one instance to the next.
 *
 * Its layout: a header page (struct svcstate_header), then two request
 * slots, each a request area and a reply area, then the service's own
 * area (svcstate_user). Every area starts on a page. The header says what
 * the state is (magic, this header's version, the service's kind and its
 * own layout version, both from the build, and the binding: what the
 * state belongs to, fat's disk and partition), how many times it was
 * adopted, the two slots and the commit word. svcstate_open checks all of
 * it against what the service expects, and every count and index inside;
 * on any mismatch the state is refused, set up empty (a line in the log)
 * and the service starts fresh, as it would without a state. What the
 * service's own area holds it checks itself, and after a fresh start
 * (or a refusal) it sets that area up from scratch: the bytes there are
 * zeros in a new VMO but whatever a refused state left in a used one.
 *
 * **Requests in slots.** A service reads each request itself, straight
 * into a slot (svcstate_take), so the request in progress survives its
 * death. Before the read the slot's length is zeroed and then its number
 * set (in that order, released); the kernel copies the message and writes
 * its length into the slot in one system call, and a killed thread always
 * finishes the call it is in before it dies, so a request is either still
 * queued or wholly in the slot (ktest chanread_kill_loses_nothing). The slot's pages
 * are committed and mapped in advance, so that copy never faults. Then:
 * run it, put the reply in the slot's reply area and commit
 * (svcstate_commit: the reply's length, then the commit word, a single
 * released store, so a death leaves it either old or new), send what was
 * held for the disk, mark it sent (svcstate_sent), reply: svcstate_answer
 * makes the reply wait for the service's next system call (the next take,
 * or its wait on its port: <idl/common.h> struct idl_reply), which sends
 * it and sets the slot's `replied` mark in the kernel, once it went out,
 * before it reads or waits. So whether the reply went out is known
 * exactly: the window in which a successor answered again is gone
 * (svcstate_reply, a plain write, sets the mark itself just after). The
 * two slots alternate; a request found in the other slot proves the
 * previous reply went out. A successor asks svcstate_pending what it
 * found: nothing in progress (nothing taken, or answered and its reply
 * out); taken and not committed (re-run it from the slot's bytes on the
 * state as committed); committed and not sent (send the held writes
 * again, then reply); sent and its reply not out (reply).
 *
 * **Warm spares.** A program started with SR_STANDBY waits in libos's
 * startup, before main, holding nothing but that channel, until its
 * supervisor promotes it (standby_promote: the handles by role, the
 * arguments and the time of the kill, <jam/startup.h> struct
 * standby_msg). From then on it is started like any program: its code
 * doesn't know it was a spare.
 *
 * None of this is thread-safe: one thread owns a state. */
#pragma once

#include <idl/common.h>
#include <os.h>

#define SVCSTATE_ADDR      0x0000600000000000ull   /* where a service maps its state VMO */
#define SVCSTATE_MAX_SIZE  (64ull << 20)           /* the most it reserves there */
#define SVCSTATE_MAGIC     0x4554415453435653ull   /* "SVCSTATE" */
#define SVCSTATE_VERSION   2                       /* struct svcstate_header's layout */
#define SVCSTATE_BINDING   32                      /* bytes naming what a state belongs to */
#define SVCSTATE_REQ_MAX   (128u << 10)            /* largest request or reply area, bytes */
#define SVCSTATE_SLOT_HANDLES 4                    /* handles a request may carry */
/* The rights a service gets on its state VMO (send it with these). */
#define SVCSTATE_SERVICE_RIGHTS (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)

/* A slot's phase after its commit. */
#define SVCSTATE_RUN  0u   /* taken: running, or committed and its held writes not all out */
#define SVCSTATE_SENT 1u   /* committed and every held write out: only the reply is left */

struct svcstate_slot {
    uint64_t seq;         /* the request's number (1, 2, ...); 0: never used; slot = seq & 1 */
    uint32_t len;         /* request bytes, written by the kernel's read; 0: none taken */
    uint32_t nhandles;    /* handles the request carried (gone with a dead instance) */
    uint32_t channel;     /* which channel it was read from: the service's own key */
    uint32_t phase;       /* SVCSTATE_RUN or SVCSTATE_SENT */
    uint32_t reply_len;   /* reply bytes in the reply area, set at the commit */
    uint32_t runs;        /* the service's count of its runs of it (0 at the take) */
    uint64_t replied;     /* 1 once its reply went out (the kernel's mark: struct
                           * idl_reply), 0 before; a reply of 0 bytes never goes */
};

struct svcstate_header {
    uint64_t magic;                       /* SVCSTATE_MAGIC */
    uint32_t version;                     /* SVCSTATE_VERSION */
    uint32_t header_size;                 /* sizeof(struct svcstate_header) */
    uint32_t kind;                        /* which service (a tag of its own) */
    uint32_t layout;                      /* the service's own layout version, from its build */
    uint64_t size;                        /* bytes the layout uses (the VMO may be bigger) */
    uint8_t  binding[SVCSTATE_BINDING];   /* what it belongs to */
    uint32_t req_cap;                     /* each request area, bytes */
    uint32_t rep_cap;                     /* each reply area, bytes */
    uint64_t user_size;                   /* the service's own area, bytes */
    uint64_t adopted;                     /* times a successor took it over */
    uint64_t commit;                      /* the commit word: seq of the last committed request */
    struct svcstate_slot slot[2];         /* the request slots */
};

/* What a service expects of its state. */
struct svcstate_layout {
    uint32_t kind;                        /* which service */
    uint32_t layout;                      /* its own layout version */
    uint8_t  binding[SVCSTATE_BINDING];   /* what this instance serves (zeros: nothing particular) */
    uint32_t req_cap;                     /* bytes of a request slot: its biggest request, 4 or more */
    uint32_t rep_cap;                     /* bytes of a reply, 4 or more */
    uint64_t user_size;                   /* bytes of its own area */
};

/* What svcstate_open found. */
enum svcstate_start {
    SVCSTATE_FRESH,     /* an empty state (a new VMO): set up now */
    SVCSTATE_ADOPTED,   /* a dead instance's state, checked: carry on */
    SVCSTATE_REFUSED,   /* a state that failed a check: set up empty, start fresh */
};

/* What a successor finds in progress (svcstate_pending). */
enum svcstate_case {
    SVCSTATE_IDLE,      /* nothing: the last request was answered */
    SVCSTATE_RERUN,     /* taken, not committed: run it again from the slot */
    SVCSTATE_RESEND,    /* committed: send the held writes again, then reply */
    SVCSTATE_REPLY,     /* sent: reply (again) */
};

/* A service's mapped state. */
struct svcstate {
    struct svcstate_header *h;          /* at SVCSTATE_ADDR */
    uint64_t  mapped;                   /* bytes mapped there */
    uint64_t  next_seq;                 /* the number the next take gives */
    handle_t  handles[SVCSTATE_SLOT_HANDLES];   /* the last take's request handles */
    const char *why;                    /* SVCSTATE_REFUSED: the check that failed */
};

/* ---- the supervisor's side --------------------------------------------------------- */

/* Bytes of state VMO a layout needs (0: the layout is impossible). */
uint64_t svcstate_size(const struct svcstate_layout *l);
/* A new, empty state VMO of size bytes (svcstate_size), its handle with
 * every right in *out: the supervisor's, never mapped. */
status_t svcstate_create(uint64_t size, handle_t *out);
/* A handle on the state for an instance: SVCSTATE_SERVICE_RIGHTS plus
 * RIGHT_TRANSFER, to be sent with SVCSTATE_SERVICE_RIGHTS
 * (spawn_args.extra_rights, standby_promote's rights), which takes the
 * transfer right off in transit. */
status_t svcstate_give(handle_t state, handle_t *out);

/* Promote the spare whose SR_STANDBY's other end is ch: handles hs[0..n)
 * (n at most STANDBY_MAX_HANDLES; consumed whatever happens), each sent
 * with rights[i] (NULL, or RIGHT_SAME: as it is), argv (argc strings) and
 * envp (NULL-terminated, may be NULL), and the time the instance it
 * replaces was killed or found dead (0: none). ERR_OUT_OF_RANGE: too many
 * handles or strings for one message. */
struct standby_args {
    const struct spawn_handle *hs;       /* the handles by role */
    const rights_t            *rights;   /* NULL, or one per handle */
    unsigned                   n;        /* entries in hs */
    int                        argc;     /* entries in argv */
    const char *const         *argv;     /* the program's argv */
    const char *const         *envp;     /* NULL-terminated, or NULL */
    uint64_t                   kill_ns;  /* standby_kill_ns() in the spare */
};
status_t standby_promote(handle_t ch, const struct standby_args *a);

/* ---- the service's side ------------------------------------------------------------ */

/* Map the state VMO (SR_STATE) at SVCSTATE_ADDR and check it against
 * want (above: fresh, adopted or refused, in *how), commit and touch its
 * header and slot pages, and count an adoption. OK with *how: the state
 * is usable (set up empty unless SVCSTATE_ADOPTED). ERR_OUT_OF_RANGE: the
 * VMO is smaller than the layout or bigger than SVCSTATE_MAX_SIZE, or the
 * layout is impossible; ERR_ALREADY_BOUND: something is mapped there
 * already; else vmar_map's and vmo_commit's errors. */
status_t svcstate_open(handle_t vmo, const struct svcstate_layout *want, struct svcstate *out,
                       enum svcstate_start *how);
/* Unmap it (a test's "death"; a service never needs to). */
void     svcstate_close(struct svcstate *s);
/* The service's own area (user_size bytes, page-aligned). */
void    *svcstate_user(const struct svcstate *s);

/* Read one request from ch into the next slot, recording `key` (which
 * channel) in it; *slot gets its index. OK, or the read's status: then no
 * request is in the slot (ERR_BUFFER_TOO_SMALL: the request is bigger
 * than req_cap or carries more than SVCSTATE_SLOT_HANDLES handles, and is
 * still queued: the caller throws it away). ERR_INVALID_ARGS: a message
 * under 4 bytes (no txid), thrown away. The request's handles, if any,
 * are in s->handles. */
status_t svcstate_take(struct svcstate *s, uint32_t key, handle_t ch, unsigned *slot);
/* svcstate_take in two halves, around a read of the caller's own (a
 * protocol's generated <proto>_take_slot, <idl/common.h>): set up the next
 * slot for a request from channel `key` (its length zeroed, then its
 * number set), *slot its index and *out where the read goes (its request
 * area, length word, handles: s->handles); then, after the read, whether
 * a request is in it (its length 4 or more, which the read wrote). A slot
 * that got none stays set up for the next prepare. */
void     svcstate_prepare(struct svcstate *s, uint32_t key, unsigned *slot, struct idl_slot *out);
bool     svcstate_taken(struct svcstate *s, unsigned slot);
/* Slot `slot` as <idl/common.h> describes one (for a protocol's
 * <proto>_run_slot): its request, length, handles and reply area. */
void     svcstate_slot(struct svcstate *s, unsigned slot, struct idl_slot *out);
/* The request in a slot (*len bytes), and its reply area (rep_cap bytes). */
void    *svcstate_request(const struct svcstate *s, unsigned slot, uint32_t *len);
void    *svcstate_reply_area(const struct svcstate *s, unsigned slot);
/* Commit the slot's request: its reply (reply_len bytes, already in the
 * reply area), then the commit word. ERR_OUT_OF_RANGE: reply_len over
 * rep_cap (nothing committed). */
status_t svcstate_commit(struct svcstate *s, unsigned slot, uint32_t reply_len);
/* Every held write of the slot's request is out: only its reply is left. */
void     svcstate_sent(struct svcstate *s, unsigned slot);
/* Write the slot's reply on ch, with hs[0..nh) (moved), marking it
 * committed and sent first if it isn't yet (a request that changed
 * nothing), and replied once it went. Errors as channel_write's. */
status_t svcstate_reply(struct svcstate *s, unsigned slot, handle_t ch, const handle_t *hs,
                        uint32_t nh);
/* The slot's reply made to wait for the service's next system call, into
 * *out (<idl/common.h> struct idl_reply, empty before: nothing else may
 * wait), with hs[0..nh) (moved; at most IDL_REP_HANDLES), on ch, and the
 * slot's `replied` word as the mark the kernel sets once it went out; it
 * is marked committed and sent first if it isn't yet, as svcstate_reply.
 * A reply of 0 bytes (answered later, or no txid) waits for nothing:
 * *out stays empty and the handles are closed. */
void     svcstate_answer(struct svcstate *s, unsigned slot, handle_t ch, const handle_t *hs,
                         uint32_t nh, struct idl_reply *out);
/* An adopted state's request in progress (*slot: its slot). */
enum svcstate_case svcstate_pending(const struct svcstate *s, unsigned *slot);

/* ---- a promoted spare ---------------------------------------------------------------- */

/* The promotion's kill time (struct standby_msg kill_ns); 0 if this
 * program wasn't a spare, or nothing died before it. */
uint64_t standby_kill_ns(void);
/* When the promotion arrived (uptime ns); 0 if this program wasn't a spare. */
uint64_t standby_promoted_ns(void);
