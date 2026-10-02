/* <sockring.h>: a socket's data path between a program and netstack:
 * two byte rings in one shared VMO, and two events (docs/M9.5-PLAN.md has
 * the plan; the control calls stay IDL, abi/idl/net.idl). Programs (libos's
 * <net.h>) and netstack both use the ring code here (user/lib/sockring.c),
 * so the index logic is written and tested once (utest's sockring_*). The
 * model is <jam/netdev.h>'s, between netstack and a network card's driver.
 *
 * ---- The model --------------------------------------------------------------
 * A socket is its channel (the control calls: connect, state, and for
 * streams the TCP calls; closing it closes the socket), one ring VMO and
 * two events. The VMO is a header page (struct sockring_page), then the
 * tx ring's bytes, then the rx ring's:
 *
 *   tx ring   the program produces, netstack consumes (what to send)
 *   rx ring   netstack produces, the program consumes (what came in)
 *
 * Each ring is `size` bytes, a power of two from SOCKRING_MIN to
 * SOCKRING_MAX, fixed when the rings are made. Its producer owns one line
 * of the header page (struct sockring_line: `count`, `waits`, `flags`) and
 * its consumer another; each side writes only its own lines and reads the
 * other's. The counts are bytes since the open and never wrap (2^64 bytes
 * is millennia at 10 Gb/s), so byte b is at data[b % size], the ring is
 * empty when produced == consumed and full when produced - consumed ==
 * size. A producer writes the bytes, then publishes its count with a
 * release store; a consumer reads the count with an acquire load, copies
 * the bytes out, then publishes its own count (release), after which the
 * producer may write over them.
 *
 * ---- Framing --------------------------------------------------------------------
 * A socket's two rings have one framing, set when they are made:
 *
 *   SOCKRING_DGRAM   (UDP) a ring is a run of records: a 16-byte header
 *                    (struct sockring_dgram: address, port, length, flags)
 *                    then `len` bytes (0..SOCKRING_DGRAM_MAX), padded to
 *                    SOCKRING_ALIGN. Every record starts on SOCKRING_ALIGN,
 *                    so a header never straddles the ring's end; its bytes
 *                    may (the copy helpers take them in two pieces). One
 *                    record is one datagram: a producer publishes whole
 *                    records only. tx: `addr`:`port` is where to send it
 *                    (0:0: the peer sock_connect set); rx: who sent it.
 *   SOCKRING_STREAM  (TCP) a ring is plain bytes in order, no headers: a
 *                    reader takes any number, a writer adds any number.
 *                    The end of a direction is a flag, SOCKRING_END, on the
 *                    producer's line: no byte comes after the count it
 *                    published with it (tx: the program shut down sending,
 *                    netstack sends a FIN after the last byte; rx: the
 *                    peer's FIN came, after the last byte it sent). There is
 *                    no push flag: netstack sends what it finds whenever it
 *                    looks at the ring, so publishing is the push.
 *
 * ---- Waking (the events) ---------------------------------------------------------
 * Two events, one per waiter: `to_stack` (netstack binds it on its port;
 * the program signals it) and `to_prog` (the program waits on it; netstack
 * signals it). A side signals the other only when that side said it is
 * waiting (its line's `waits`), so while data flows there is no system
 * call per datagram or per write:
 *
 *   SOCKRING_SIG_TX       on to_stack: the tx ring has bytes, or SOCKRING_END
 *                         (the program, after publishing, if tx's consumer
 *                         waits)
 *   SOCKRING_SIG_RX_ROOM  on to_stack: the rx ring has room again (the
 *                         program, after consuming, if rx's producer waits:
 *                         only a stream's netstack waits for room)
 *   SOCKRING_SIG_RX       on to_prog: the rx ring has bytes, or SOCKRING_END
 *   SOCKRING_SIG_TX_ROOM  on to_prog: the tx ring has room again
 *   SOCKRING_SIG_STATE    on to_prog: the status line changed (always
 *                         signalled: a connection came up, ended or failed)
 *
 * No wake is lost: a waiter raises its flag, fences and looks at the ring
 * again before it sleeps (sockring_sleep); the other side publishes its
 * count, fences and then reads the flag (sockring_publish): one of the two
 * always sees the other's write. A waiter clears its event bits BEFORE it
 * looks at the rings, so a signal that comes while it works stays for the
 * next wait. An event can't say that the side that signals it has died:
 * a program waits on `to_prog` and on its socket's channel together (a
 * port, with SIG_PEER_CLOSED on the channel), as libos does.
 *
 * ---- Backpressure: netstack never waits for a program ----------------------------
 *   - tx: netstack takes a record (or bytes) only when it can send it now;
 *     otherwise it leaves the rest in the ring and looks again when the card
 *     (or TCP's window) has room. A full tx ring is the program's sign to
 *     wait: it raises tx's producer `waits` and sleeps for SOCKRING_SIG_TX_ROOM.
 *   - rx, datagrams: a datagram that doesn't fit the rx ring is dropped and
 *     counted (the status line's rx_dropped); netstack never holds one back
 *     for a slow reader.
 *   - rx, streams: what doesn't fit stays in lwIP and the TCP window shrinks;
 *     netstack raises rx's producer `waits` and goes on with other work until
 *     SOCKRING_SIG_RX_ROOM.
 *   - netstack takes at most what was ready when it looked (at most a ring)
 *     from one socket a turn, so a busy socket can't starve the others.
 *
 * ---- Trust ------------------------------------------------------------------------
 * The program maps the whole VMO writable, so it can write anything into
 * it at any moment, and netstack treats every byte of it as hostile:
 *   - it keeps its own count of what it consumed or produced (sockring_end's
 *     `count`) and its own copy of the sizes and the status, and never reads
 *     its own lines, the info line or the status line back;
 *   - the program's count is read once a look and clamped to the ring: a tx
 *     `count` that went backwards is no bytes, one more than a ring ahead is
 *     a ring's worth (and on a datagram ring a count not on SOCKRING_ALIGN is
 *     rounded down); an rx `count` ahead of what was produced, or more than a
 *     ring behind, is no room. Each is counted in sockring_end's `errors`;
 *   - a record's header is read once and checked (flags and reserved 0,
 *     `len` at most SOCKRING_DGRAM_MAX, the record inside what was
 *     published); a bad one loses the framing, so everything published so
 *     far is dropped (counted once) and the ring goes on from there;
 *   - a record's bytes are copied out of the ring before they are used, and
 *     the checks (address, port, length) run on the copies; netstack never
 *     keeps a pointer into a ring (lwIP gets copies, TCP_WRITE_FLAG_COPY);
 *   - after SOCKRING_END, bytes the producer adds are ignored and counted;
 *   - a ring that was out of range or held a bad record is looked at again
 *     only when the program signals SOCKRING_SIG_TX, so garbage costs
 *     netstack one look a signal, not one a turn;
 *   - signals: `to_stack` is bound PERSISTENT, so a program that signals in
 *     a loop costs netstack one coalesced packet and a look at its own rings.
 * The program treats netstack's side the same way (the same code): netstack
 * is trusted more, but a bad count must not crash a program.
 *
 * ---- Who holds what (the rights) ---------------------------------------------------
 * netstack makes the VMO (zeroed, sized sockring_bytes) and the events, maps
 * the VMO, and hands the program the VMO with SOCKRING_VMO_RIGHTS (read,
 * write, map: no RIGHT_RESIZE, so the program can neither shrink nor
 * decommit it under netstack's mapping, and no RIGHT_DUPLICATE),
 * `to_stack` with SOCKRING_TO_STACK_RIGHTS (signal only) and `to_prog` with
 * SOCKRING_TO_PROG_RIGHTS (wait, and signal to clear its own bits). Each
 * arrives with RIGHT_TRANSFER (an IDL reply moves handles with
 * channel_write, which keeps their rights): a program may hand a socket on
 * whole, and whoever gets it has the program's power over it, no more.
 *
 * ---- Memory and limits ----------------------------------------------------------------
 * The VMO is netstack's, so its pages are charged to netstack's job as the
 * program touches them. netstack counts each socket's sockring_bytes
 * against its opener (SOCKRING_OPENER_BYTES) and against all sockets
 * together (SOCKRING_TOTAL_BYTES); a socket over either is refused
 * (ERR_NO_RESOURCES). netstack's DHCP socket belongs to no opener and
 * counts against the total only.
 *
 * ---- Closing, errors and the status line ---------------------------------------------
 * Closing the socket's channel ends the socket: netstack stops reading its
 * rings, unmaps the VMO, shrinks it to 0 bytes (so no page it was charged
 * for outlives the socket in a program's hands) and closes its handles. A
 * program unmaps the VMO before it closes the channel: touching the mapping
 * afterwards faults. netstack never shrinks or unmaps while the program's
 * end of the channel is open (a wait set may read the rings until it sees
 * SIG_PEER_CLOSED): a socket whose opener closes is only ended (state
 * CLOSED, error ERR_PEER_CLOSED; no more datagrams either way) and kept,
 * still counted, until the program closes it.
 * What netstack has not taken from the tx ring by then is dropped; a stream
 * that must be sure its bytes went sets SOCKRING_END and waits for the
 * status line to say the direction is done before it closes.
 * netstack's notices live in the status line (struct sockring_status,
 * written by netstack alone, from its own copy): the socket's `state`, the
 * last `error`, and counts of datagrams dropped for a full rx ring, tx
 * records refused (a bad address or port, no route: the reason is the
 * `error`) and ring errors. A change of `state` or `error` signals
 * SOCKRING_SIG_STATE. A datagram socket is SOCKRING_STATE_OPEN for its
 * whole life; a stream's states are below.
 * If netstack ends, every socket's channel sees ERR_PEER_CLOSED: unmap,
 * close everything, open /svc/net and the sockets again. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

/* ---- sizes -------------------------------------------------------------------- */

#define SOCKRING_HDR        4096u            /* the header page; the rings follow */
#define SOCKRING_MIN        4096u            /* a ring's size: a power of two ... */
#define SOCKRING_MAX        (2u << 20)       /* ... from SOCKRING_MIN to here (a whole
                                              * scaled TCP window) */
#define SOCKRING_ALIGN      16u              /* a datagram record starts on this */
#define SOCKRING_DGRAM_MAX  1472u            /* a datagram's bytes: one frame, never fragmented */
#define SOCKRING_DGRAM_HDR  16u              /* struct sockring_dgram */
/* A UDP socket's rings unless it asks for others: 11 full datagrams to
 * send, 22 to receive (hundreds of small ones), 52 KiB with the header. */
#define SOCKRING_UDP_TX     (16u * 1024)
#define SOCKRING_UDP_RX     (32u * 1024)
/* Ring bytes (sockring_bytes, the header included) one opener's sockets
 * may hold together (a TCP connection with both rings at SOCKRING_MAX and
 * room to spare), and all sockets together (<net.h>'s NET_PROG_RING_BYTES
 * of it ordinary programs'). */
#define SOCKRING_OPENER_BYTES (8u << 20)
#define SOCKRING_TOTAL_BYTES  (24u << 20)
#define SOCKRING_MAGIC      0x474e5253u      /* "SRNG" */

/* Framings (sockring_info.framing). */
#define SOCKRING_DGRAM      1u
#define SOCKRING_STREAM     2u

/* A producer line's flags. */
#define SOCKRING_END        (1u << 0)        /* no byte after this count */

/* The events' bits (SIG_USER_ALL, set and cleared with event_signal). */
#define SOCKRING_SIG_TX       (1u << 24)     /* on to_stack */
#define SOCKRING_SIG_RX_ROOM  (1u << 25)     /* on to_stack */
#define SOCKRING_SIG_RX       (1u << 24)     /* on to_prog */
#define SOCKRING_SIG_TX_ROOM  (1u << 25)     /* on to_prog */
#define SOCKRING_SIG_STATE    (1u << 26)     /* on to_prog */

/* The rights of the handles a program gets (see "Who holds what"). */
#define SOCKRING_VMO_RIGHTS      (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER)
#define SOCKRING_TO_STACK_RIGHTS (RIGHT_SIGNAL | RIGHT_TRANSFER)
#define SOCKRING_TO_PROG_RIGHTS  (RIGHT_WAIT | RIGHT_SIGNAL | RIGHT_TRANSFER)

/* A socket's states (sockring_status.state). A datagram socket is OPEN
 * from start to end. A stream: CONNECTING (tcp_connect sent its SYN),
 * OPEN (data both ways), then each direction ends by SOCKRING_END on its
 * ring; CLOSED when both are done and the peer acked our FIN (`error`
 * OK), or at once when the connection failed or was reset (`error` says
 * why; the rx ring keeps what came before). Which error for which TCP
 * event is net.idl's to say. */
#define SOCKRING_STATE_OPEN       1u
#define SOCKRING_STATE_CONNECTING 2u
#define SOCKRING_STATE_CLOSED     3u

/* ---- the shared layout ------------------------------------------------------------ */

/* Line 0 of the header page: netstack writes it once, before it hands the
 * VMO out, and never reads it back. */
struct sockring_info {
    uint32_t magic;            /* SOCKRING_MAGIC */
    uint32_t framing;          /* SOCKRING_DGRAM or SOCKRING_STREAM */
    uint32_t tx_size;          /* the tx ring's bytes, at SOCKRING_HDR */
    uint32_t rx_size;          /* the rx ring's bytes, right after the tx ring */
    uint32_t reserved[12];     /* 0 */
};

/* One side's line of one ring. */
struct sockring_line {
    uint64_t count;            /* bytes produced (or consumed) since the open */
    uint32_t waits;            /* 1: this side found the ring full (or empty) and sleeps */
    uint32_t flags;            /* a producer's: SOCKRING_END; a consumer's: 0 */
    uint64_t reserved[6];      /* 0 */
};

/* netstack's notices to the program: written by netstack alone, from its
 * own copy (sockring_status_put), never read back. */
struct sockring_status {
    uint32_t state;            /* SOCKRING_STATE_* */
    int32_t  error;            /* a status_t: why it CLOSED, or the last tx record refused */
    uint32_t changes;          /* times state or error changed */
    uint32_t reserved0;        /* 0 */
    uint64_t rx_dropped;       /* datagrams dropped: the rx ring was full */
    uint64_t tx_refused;       /* tx records refused (their reason: `error`) */
    uint64_t ring_errors;      /* the program's counts or records out of range */
    uint64_t reserved[3];      /* 0 */
};

/* The header page: six cache lines. */
struct sockring_page {
    struct sockring_info   info;      /* netstack's, once */
    struct sockring_line   tx_prod;   /* the program's */
    struct sockring_line   tx_cons;   /* netstack's */
    struct sockring_line   rx_prod;   /* netstack's */
    struct sockring_line   rx_cons;   /* the program's */
    struct sockring_status status;    /* netstack's */
};
_Static_assert(sizeof(struct sockring_info) == 64, "a line");
_Static_assert(sizeof(struct sockring_line) == 64, "a line");
_Static_assert(sizeof(struct sockring_status) == 64, "a line");
_Static_assert(sizeof(struct sockring_page) == 384, "six lines");
_Static_assert(sizeof(struct sockring_page) <= SOCKRING_HDR, "the header fits its page");

/* A datagram record's header, then `len` bytes, padded to SOCKRING_ALIGN. */
struct sockring_dgram {
    uint32_t addr;             /* tx: where to (0 with port 0: the connected peer); rx: from */
    uint16_t port;             /* tx: the port to; rx: the sender's */
    uint16_t len;              /* the datagram's bytes: 0..SOCKRING_DGRAM_MAX */
    uint32_t flags;            /* 0: a record with anything else is bad */
    uint32_t reserved;         /* 0: a record with anything else is bad */
};
_Static_assert(sizeof(struct sockring_dgram) == SOCKRING_DGRAM_HDR, "a record's header");
_Static_assert(SOCKRING_DGRAM_HDR % SOCKRING_ALIGN == 0, "the bytes start aligned");
_Static_assert(SOCKRING_MIN % SOCKRING_ALIGN == 0, "a header never straddles the end");

/* ---- one side's private view (never in shared memory) --------------------------------- */

struct sockring_end {
    struct sockring_line *prod;   /* the ring's producer line (shared: the peer may write it) */
    struct sockring_line *cons;   /* ... and its consumer line */
    uint8_t  *data;               /* the ring's first byte */
    uint32_t  size;               /* its bytes, a power of two: this side's own copy */
    uint32_t  framing;            /* SOCKRING_DGRAM or SOCKRING_STREAM */
    bool      producer;           /* this side produces (else it consumes) */
    bool      ended;              /* producer: it set SOCKRING_END; consumer: it saw it */
    uint64_t  end_at;             /* consumer: the producer's count when it saw END */
    uint64_t  count;              /* bytes this side produced or consumed: the copy it trusts */
    uint64_t  errors;             /* times the peer's count, flags or a record were bad */
};

/* A socket's rings as one side sees them. */
struct sockring {
    struct sockring_page *page;   /* the mapped header page */
    struct sockring_end   tx;     /* the program produces, netstack consumes */
    struct sockring_end   rx;     /* netstack produces, the program consumes */
};

/* ---- setting up (user/lib/sockring.c) ------------------------------------------------- */

/* Is size a ring size: a power of two, SOCKRING_MIN..SOCKRING_MAX? */
bool     sockring_size_ok(uint32_t size);
/* The VMO's bytes for rings of these sizes (the header page included). */
uint64_t sockring_bytes(uint32_t tx_size, uint32_t rx_size);
/* netstack: set up the rings in a VMO it just made and mapped at map
 * (sockring_bytes, zeroed as a new VMO is): the info line, the status
 * line's state SOCKRING_STATE_OPEN (netstack's own copy starts the same),
 * and its own ends (tx consumer, rx producer). ERR_INVALID_ARGS: a size or framing
 * not allowed (nothing written). */
status_t sockring_make(struct sockring *r, void *map, uint32_t framing, uint32_t tx_size,
                       uint32_t rx_size);
/* The program: take on rings netstack made, mapped at map (map_len bytes),
 * which netstack said have this framing and sizes. ERR_BAD_STATE: the
 * header isn't that (magic, framing, sizes), or the sizes don't fit
 * map_len; nothing set up then. */
status_t sockring_attach(struct sockring *r, void *map, uint64_t map_len, uint32_t framing,
                         uint32_t tx_size, uint32_t rx_size);

/* ---- the counts, both sides ---------------------------------------------------------------- */

/* Pure: bytes a consumer may take, from the producer's count as read
 * (untrusted) and its own (trusted), clamped to a ring of size bytes; *bad
 * set when `produced` went backwards or is more than a ring ahead. */
uint32_t sockring_ready_n(uint64_t produced, uint64_t consumed, uint32_t size, bool *bad);
/* Pure: bytes a producer may write, from its own count (trusted) and the
 * consumer's as read (untrusted). None, and *bad set, when `consumed` is
 * ahead of `produced` or more than a ring behind. */
uint32_t sockring_room_n(uint64_t produced, uint64_t consumed, uint32_t size, bool *bad);
/* Consumer: bytes waiting now (the producer's line read once, clamped; on
 * a datagram ring rounded down to SOCKRING_ALIGN; never past SOCKRING_END's
 * count). Notes SOCKRING_END the first time it is seen. */
uint32_t sockring_ready(struct sockring_end *e);
/* Producer: room now (the consumer's count read once; none if out of range). */
uint32_t sockring_room(struct sockring_end *e);
/* Either side: publish this side's count (release), then read the peer's
 * `waits` after a full fence. true: signal the peer (SOCKRING_SIG_RX /
 * _TX after producing, _TX_ROOM / _RX_ROOM after consuming). */
bool     sockring_publish(struct sockring_end *e);
/* Either side, about to wait (a consumer for bytes, a producer for room):
 * raise this side's flag, fence, look again. `need` is what it waits for:
 * a producer, room for its next record (sockring_dgram_bytes) or 1 byte; a
 * consumer, 1 (0 counts as 1). true: not there yet, sleep (the flag stays
 * up; the peer signals at its next publish, however much it moved); false:
 * there is work after all (the flag is down again). A consumer that saw
 * SOCKRING_END doesn't sleep. */
bool     sockring_sleep(struct sockring_end *e, uint32_t need);
/* Either side, woken or working anyway: lower this side's flag. */
void     sockring_awake(struct sockring_end *e);
/* Producer: no more bytes in this direction. Publishes the count, then
 * SOCKRING_END (release). true: signal the peer, as sockring_publish. */
bool     sockring_finish(struct sockring_end *e);
/* Consumer: did the producer end, and has this side taken every byte
 * before the end? (Looks at the ring, as sockring_ready.) */
bool     sockring_at_end(struct sockring_end *e);

/* ---- datagram framing ------------------------------------------------------------------- */

/* A record's bytes in the ring: its header and len bytes, padded. */
uint32_t sockring_dgram_bytes(uint32_t len);
/* Producer: write one datagram (h->len bytes at data) as the next record;
 * not visible until sockring_publish. ERR_INVALID_ARGS: len over
 * SOCKRING_DGRAM_MAX, or h's flags or reserved not 0; ERR_SHOULD_WAIT: no
 * room for it now; ERR_BAD_STATE: this side finished. */
status_t sockring_dgram_put(struct sockring_end *e, const struct sockring_dgram *h,
                            const void *data);
/* Consumer: take the next datagram: its header into *h and its bytes into
 * data. OK; ERR_SHOULD_WAIT: none published; ERR_OUT_OF_RANGE: a bad record
 * (its header, or more than was published): everything published so far is
 * dropped, counted once in `errors`, and the next call starts after it. The
 * header is read once and checked on the copy. */
status_t sockring_dgram_take(struct sockring_end *e, struct sockring_dgram *h,
                             uint8_t data[SOCKRING_DGRAM_MAX]);

/* ---- byte-stream framing --------------------------------------------------------------------- */

/* Producer: copy up to n bytes into the ring, as many as there is room
 * for (0 once this side finished); not visible until sockring_publish.
 * Returns how many. */
uint32_t sockring_stream_write(struct sockring_end *e, const void *src, uint32_t n);
/* Consumer: copy up to n bytes out of the ring, as many as are ready;
 * returns how many (0: none yet, or the end: sockring_at_end says which). */
uint32_t sockring_stream_read(struct sockring_end *e, void *dst, uint32_t n);

/* ---- the status line ---------------------------------------------------------------------- */

/* netstack: write its copy *s to the status line (every field; `state`
 * after `error` and the counts, and `changes` last, both with release
 * stores). The caller bumps s->changes when state or error changed, and
 * signals SOCKRING_SIG_STATE then. */
void     sockring_status_put(struct sockring *r, const struct sockring_status *s);
/* The program: read the status line (`changes` first with an acquire load,
 * then `state` with one, then each field once). Fields may be from two
 * moments if netstack writes meanwhile (`changes` tells), but a `state`
 * read never comes with an `error` older than it. */
void     sockring_status_get(const struct sockring *r, struct sockring_status *out);
