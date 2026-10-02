/* utest: fake sockets for the wait-set tests (netwait.c): n sockets' rings
 * in one VMO, each with a `to_prog` event and a channel, and the test
 * playing both sides. The stack_ calls are netstack's side of <sockring.h>
 * (publish, then signal only a peer whose `waits` is up; SIG_STATE always);
 * the prog_ calls are the program's. Even sockets carry datagrams, odd ones
 * a byte stream. Every datagram and byte is a function of its socket and
 * its place, so each side checks what it takes. One thread may play
 * netstack while another plays the program: they share only the rings,
 * the events and `dead`. */
#pragma once

#include <netwait.h>
#include <os.h>
#include <sockring.h>

#define FAKE_SOCKS 64u              /* sockets an fnet can hold */
#define FAKE_RING  SOCKRING_MIN     /* each ring's bytes */

struct fsock {
    uint32_t idx;                   /* its place in the fnet */
    struct sockring s;              /* netstack's side of the rings */
    struct sockring p;              /* the program's side: the set reads this one */
    struct sockring_status st;      /* netstack's copy of the status line */
    handle_t to_prog;               /* netstack's: all rights */
    handle_t prog_to_prog;          /* the program's: SOCKRING_TO_PROG_RIGHTS */
    handle_t ch_prog, ch_stack;     /* the socket's channel: the program's end, netstack's */
    bool     dead;                  /* netstack's end closed (atomic) */
    /* netstack's counts (datagrams, or bytes of a stream) */
    uint64_t rx_made, tx_took, bad_tx;
    /* the program's */
    uint64_t rx_got, tx_made, bad_rx;
    uint32_t id;                    /* its wait-set entry, 0: none */
    uint32_t interest;              /* that entry's interest */
};

struct fnet {
    handle_t     vmo;               /* every socket's rings, one after another */
    uint8_t     *map;               /* mapped once: both sides use this mapping */
    uint64_t     bytes;
    uint32_t     n;                 /* sockets */
    struct fsock sk[FAKE_SOCKS];
};

/* n sockets (at most FAKE_SOCKS), all OPEN, nothing in their rings. */
bool fnet_open(struct fnet *f, uint32_t n);
void fnet_close(struct fnet *f);
bool fsock_dgram(const struct fsock *k);
/* The socket as the program hands it to a set. */
struct netwait_sock fsock_waitable(struct fsock *k);

/* netstack: up to max datagrams (or bytes) into the rx ring, as many as fit,
 * never more than `limit` in all; returns how many. */
uint32_t stack_rx(struct fsock *k, uint32_t max, uint64_t limit);
/* netstack: the rx direction's end (a stream's FIN). */
void     stack_rx_end(struct fsock *k);
/* netstack: take everything in the tx ring, checked; returns how many. */
uint32_t stack_tx(struct fsock *k);
/* netstack: a new state and error on the status line, and SIG_STATE. */
void     stack_state(struct fsock *k, uint32_t state, status_t error);
/* netstack: close its end of the socket's channel (netstack gone). */
void     stack_kill(struct fsock *k);

/* The program: take up to max datagrams (or bytes), checked; how many. */
uint32_t prog_read(struct fsock *k, uint32_t max);
/* The program: up to max datagrams (or bytes) into the tx ring; how many. */
uint32_t prog_write(struct fsock *k, uint32_t max);

/* What the rings, the status and the channel say the socket's readiness is
 * for this interest, computed from the shared counts on its own (not with
 * the set's code). Netstack's thread must be still while it runs. */
uint32_t fsock_expect(struct fsock *k, uint32_t interest);

/* The set's side: add k with this interest (user: k; k->id, k->interest
 * set), take it out (k->id 0), and one wait: bits[i] and errs[i] what it
 * reported for socket i (every entry of the set must be an fsock), each
 * reported entry checked to be k->id and reported once. */
bool fsock_add(struct netwait *w, struct fsock *k, uint32_t interest);
bool fsock_take_out(struct netwait *w, struct fsock *k);
bool fsock_gather(struct netwait *w, uint64_t deadline, uint32_t *bits, status_t *errs,
                  status_t *out_st);
