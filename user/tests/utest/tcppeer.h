/* utest: a TCP peer driven by hand, for the netstack TCP tests
 * (nettcp.c): segments built byte by byte (checksums included) and given
 * to stack_input, and every frame netstack sends caught by a fake edge
 * and read back as segments, each checked (untagged, from our MAC and
 * address, IP and TCP checksums) before a test looks at it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define TP_CAP 160u   /* frames the fake edge keeps between two reads */

#define TP_FIN 0x01u
#define TP_SYN 0x02u
#define TP_RST 0x04u
#define TP_PSH 0x08u
#define TP_ACK 0x10u

/* The peer's half of one connection. */
struct tp {
    uint32_t       ip;      /* the peer's address ... */
    const uint8_t *mac;     /* ... and MAC (netstack knows it: tp_arp) */
    uint16_t       port;    /* the peer's port */
    uint16_t       our;     /* netstack's port */
    uint32_t       snd;     /* the next sequence number the peer sends */
    uint32_t       rcv;     /* the next it expects from netstack (what it acks) */
    uint16_t       win;     /* the window it announces (in its scale's units once scaled) */
    uint8_t        wscale;  /* the window scale option it sends on a SYN: shift + 1 (0: none) */
};

/* A segment netstack sent, as read back. `data` points into tp_read's
 * copy: good until the next tp_read. */
struct tp_seg {
    uint16_t       sport, dport;
    uint32_t       seq, ack;
    uint8_t        flags;   /* TP_* */
    uint16_t       win;
    uint16_t       mss;     /* its MSS option (0: none) */
    uint8_t        wscale;  /* its window scale option: shift + 1 (0: none) */
    const uint8_t *data;
    size_t         len;
};

/* The fake edge (stack_edge.tx): keeps every frame, refuses past TP_CAP
 * (ERR_NO_RESOURCES, as a full card's ring does). */
status_t tp_edge_tx(void *ctx, const uint8_t *frame, size_t len);
/* The edge is full once it holds `frames` caught ones (TP_CAP: as many as
 * it keeps; fix_up sets that): a card whose ring has no room. */
void     tp_room(unsigned frames);
/* Frames caught and not read yet; forget them. */
unsigned tp_caught(void);
void     tp_forget(void);
/* Teach netstack the peer's MAC (an ARP request from it for our address). */
void     tp_arp(const uint8_t *mac, uint32_t ip);

/* A segment from p: seq p->snd, ack p->rcv (when TP_ACK), window p->win,
 * len bytes of data; an MSS option of 1460 on a SYN, and a window scale
 * option of p->wscale - 1 when p->wscale is set. Into f (the frame's
 * length returned) or, with tp_send, straight into netstack, after which
 * p->snd moves on by len (and one for a SYN or FIN). */
size_t   tp_frame(uint8_t *f, const struct tp *p, uint8_t flags, const void *data, size_t len);
void     tp_send(struct tp *p, uint8_t flags, const void *data, size_t len);
/* Give netstack a frame as built (a test may spoil it first). */
void     tp_input(const uint8_t *f, size_t n);
/* The TCP checksum of a frame tp_frame built, again (after a change). */
void     tp_refix(uint8_t *f, size_t n);

/* Read the frames caught for p, in order (to its port, from its `our`
 * port, or from any while `our` is 0), each checked, into out (max at most
 * TP_CAP), and forget them; *others counts the frames left caught (ARP,
 * other connections). false: a frame to p's address was malformed. */
bool     tp_read(const struct tp *p, struct tp_seg *out, unsigned max, unsigned *n,
                 unsigned *others);
/* The peer takes, in order, the data and FIN of n segments: each one that
 * starts at p->rcv moves it on; its bytes appended to buf (cap bytes at
 * most, *got counting). Returns how many segments carried new bytes. */
unsigned tp_absorb(struct tp *p, const struct tp_seg *s, unsigned n, uint8_t *buf, size_t cap,
                   size_t *got);

/* The test stream: byte i of stream `id`. */
uint8_t  tp_byte(uint32_t id, uint64_t i);
void     tp_fill(uint32_t id, uint64_t from, uint8_t *buf, size_t n);
bool     tp_same(uint32_t id, uint64_t from, const uint8_t *buf, size_t n);
