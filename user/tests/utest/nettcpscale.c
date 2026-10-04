/* utest: netstack's TCP with window scaling (RFC 7323) and lwIP's memory
 * shared out, in-process on nettcp.h's fixture (the test is the peer, on
 * the wire, and the program, on the rings):
 *
 *   nettcp_window_scale  a connection netstack opens to a peer that scales:
 *                        both SYNs carry the option, the window is the 2 MiB
 *                        rx ring's room in 64-byte units (never more), the
 *                        peer has more than 64 KiB in flight and every byte
 *                        lands in the ring; netstack has more than 64 KiB in
 *                        flight with a 256 KiB tx ring, and at most 64 KiB
 *                        with a 64 KiB one; a peer that doesn't scale gets
 *                        the unscaled 64240
 *   nettcp_listen_scale  a listener's connection: the SYN-ACK carries the
 *                        option only when the SYN did, its window field
 *                        unscaled; then the scaled window, the peer's bytes
 *                        past 64 KiB in the ring
 *   nettcp_heap_shares   three bulk senders (2 MiB tx rings, a peer that
 *                        never acks) can't take the heap the others need: a
 *                        connection with an ordinary ring still sends its
 *                        bytes, and the heap's last part is left
 *
 * Rings this big don't fit the fixture's slots: they are VMOs here
 * (nettcp.h's fix_big_*, which the out-of-order tests share). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <sockring.h>
#include "netpkt.h"
#include "nettcp.h"
#include "stack.h"
#include "tcp.h"
#include "tcppeer.h"
#include "utest.h"

#define OUR_SHIFT  6u           /* lwipopts.h TCP_RCV_SCALE */
#define PEER_SHIFT 7u           /* the peer's: 0xffff << 7, an 8 MiB window */
#define BIG        (2u << 20)   /* a bulk ring: a whole scaled window */
/* The peer fix_open_big connects to on port n: scaling (PEER_SHIFT), or not. */
#define SCALING(n) (&(struct tp){ .port = (n), .wscale = PEER_SHIFT + 1 })
#define PLAIN(n)   (&(struct tp){ .port = (n) })

bool fix_big_make(uint32_t tx, uint32_t rx, struct sockring *ss, struct sockring *prog,
                  struct fix_big *b)
{
    uint64_t va = 0;
    b->len = sockring_bytes(tx, rx);
    CHECK_ST(jam_vmo_create(b->len, 0, HANDLE_INVALID, &b->vmo), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), b->vmo, 0, b->len,
                          VMAR_READ | VMAR_WRITE, &va), OK);
    b->map = (uint8_t *)(uintptr_t)va;
    CHECK_ST(sockring_make(ss, b->map, SOCKRING_STREAM, tx, rx), OK);
    CHECK_ST(sockring_attach(prog, b->map, b->len, SOCKRING_STREAM, tx, rx), OK);
    return true;
}

void fix_big_free(struct fix_big *b)
{
    if (b->map)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)b->map,
                             b->len);   /* ours: nothing to do if it fails */
    if (b->vmo)
        jam_handle_close(b->vmo);
    *b = (struct fix_big){ 0 };
}

void fix_free_big(struct cx *x, struct fix_big *b)
{
    ntcp_conn_free(x->c);
    fix_big_free(b);
}

/* The window netstack announced last, in bytes. */
static uint32_t wnd_bytes(const struct cx *x, bool scaled)
{
    return scaled ? (uint32_t)x->window << OUR_SHIFT : x->window;
}

/* It is the rx ring's room, rounded down to a scaled window's unit. */
static bool wnd_is_room(struct cx *x)
{
    uint32_t room = x->prog.rx.size - sockring_ready(&x->prog.rx), w = wnd_bytes(x, true);
    if (w > room || room - w >= (1u << OUR_SHIFT))
        FAIL("the window is %u bytes, the ring's room %u", w, room);
    return true;
}

bool fix_open_big(struct cx *x, struct fix_big *b, const struct tp *peer, uint32_t tx,
                  uint32_t rx)
{
    struct sockring ss;
    struct tp_seg s[4];
    unsigned n;
    *x = (struct cx){ .p = *peer };
    x->p.ip = PEER_IP;
    x->p.mac = pkt_peer_mac;
    x->p.snd = PEER_ISN;
    x->p.win = 0xffff;
    CHECK(fix_big_make(tx, rx, &ss, &x->prog, b));
    CHECK_ST(ntcp_conn_new(x, &x->c), OK);
    ntcp_conn_rings(x->c, &ss);
    CHECK_ST(ntcp_connect(x->c, PEER_IP, x->p.port), OK);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_SYN);
    CHECK_EQ(s[0].wscale, OUR_SHIFT + 1);   /* netstack offers scaling */
    CHECK_EQ(s[0].win, 0xffff);             /* lwIP's, until the handshake settles it */
    x->p.our = s[0].sport;
    x->p.rcv = s[0].seq + 1;
    tp_send(&x->p, TP_SYN | TP_ACK, NULL, 0);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_ACK);
    CHECK_EQ(fix_status(x).state, SOCKRING_STATE_OPEN);
    if (x->p.wscale)
        tp_send(&x->p, TP_ACK, NULL, 0);   /* the scaled window: 0xffff << the peer's shift */
    return true;
}

/* netstack's turn, and its ACKs read; the last segment's ACK may be
 * delayed (lwIP acks every second one at once): then lwIP's timer sends
 * it. All the peer sent is acked, the window as it is now. */
bool fix_acked_all(struct cx *x)
{
    static struct tp_seg s[TP_CAP];
    unsigned n;
    CHECK(fix_turn(x, s, TP_CAP, &n, false));
    if (x->acked != x->p.snd) {
        fix_tick();
        CHECK(fix_turn(x, s, TP_CAP, &n, false));
    }
    CHECK_EQ(x->acked, x->p.snd);
    return true;
}

/* The peer sends `segs` full segments of stream id without waiting for an
 * ACK; netstack must take every byte into the rx ring (nothing dropped:
 * its window covered them) and, if the window is scaled, announce the
 * ring's room. Then the program reads them, each byte checked, and says
 * so. */
static bool peer_burst(struct cx *x, uint32_t id, uint64_t from, unsigned segs, bool scaled)
{
    static uint8_t seg[STACK_TCP_MSS], back[STACK_TCP_MSS];
    uint32_t before = sockring_ready(&x->prog.rx);
    for (unsigned i = 0; i < segs; i++) {
        tp_fill(id, from + (uint64_t)i * STACK_TCP_MSS, seg, STACK_TCP_MSS);
        tp_send(&x->p, TP_ACK | TP_PSH, seg, STACK_TCP_MSS);
    }
    CHECK(fix_acked_all(x));
    CHECK_EQ(sockring_ready(&x->prog.rx), before + segs * STACK_TCP_MSS);
    if (scaled)
        CHECK(wnd_is_room(x));
    for (unsigned i = 0; i < segs; i++) {   /* in order, every byte */
        CHECK_EQ(sockring_stream_read(&x->prog.rx, back, STACK_TCP_MSS), STACK_TCP_MSS);
        CHECK(tp_same(id, from + (uint64_t)i * STACK_TCP_MSS, back, STACK_TCP_MSS));
    }
    fix_publish(x, &x->prog.rx);   /* given back to the window at netstack's next look */
    return true;
}

/* The program keeps its tx ring full; the peer acks every segment on its
 * own (lwIP's congestion window grows a segment an ACK) for at most
 * `rounds` turns, or until more than `stop` bytes were in flight: the most
 * netstack sent past the last ACK the peer had sent (a lower bound of
 * what it had in flight). */
static bool in_flight(struct cx *x, uint32_t id, unsigned rounds, uint32_t stop, uint32_t *most)
{
    static uint8_t tmp[16 * 1024];
    static struct tp_seg s[TP_CAP];
    uint64_t put = 0;
    *most = 0;
    for (unsigned r = 0; r < rounds && *most <= stop; r++) {
        for (uint32_t room; (room = sockring_room(&x->prog.tx)) != 0; put += room) {
            room = room < sizeof(tmp) ? room : (uint32_t)sizeof(tmp);
            tp_fill(id, put, tmp, room);
            CHECK_EQ(sockring_stream_write(&x->prog.tx, tmp, room), room);
        }
        fix_publish(x, &x->prog.tx);
        unsigned n;
        CHECK(fix_turn(x, s, TP_CAP, &n, false));
        uint32_t acked = x->p.rcv;
        for (unsigned i = 0; i < n; i++)   /* in order: the peer's rcv moves on */
            if (s[i].len && s[i].seq == x->p.rcv)
                x->p.rcv += (uint32_t)s[i].len;
        uint32_t end = x->p.rcv;
        *most = end - acked > *most ? end - acked : *most;
        x->p.rcv = acked;
        for (unsigned i = 0; i < n; i++) {   /* an ACK for each segment */
            if (!s[i].len || s[i].seq != x->p.rcv)
                continue;
            x->p.rcv += (uint32_t)s[i].len;
            tp_send(&x->p, TP_ACK, NULL, 0);
        }
        CHECK_EQ(x->p.rcv, end);
    }
    return true;
}

/* Close a connection the test is done with: the peer resets it. */
void fix_reset_by_peer(struct cx *x)
{
    tp_send(&x->p, TP_RST | TP_ACK, NULL, 0);
    tp_forget();
}

/* ---- connections netstack opens ---------------------------------------------------- */

static bool scaled_rx(void)
{
    struct cx x;
    struct fix_big b = { 0 };
    CHECK(fix_open_big(&x, &b, SCALING(8080), 4096, BIG));
    CHECK_EQ(wnd_bytes(&x, true), BIG);    /* the handshake's ACK: the ring's 2 MiB */
    CHECK(peer_burst(&x, 5, 0, 100, true));        /* 146000 bytes: more than 64 KiB in flight */
    CHECK(peer_burst(&x, 5, 146000, 1, true));     /* what was read is given back: */
    CHECK(wnd_bytes(&x, true) > BIG - 2 * STACK_TCP_MSS);   /* a whole window but a segment */
    uint8_t seg[STACK_TCP_MSS];            /* bytes unread: the window shrinks with them */
    tp_fill(6, 0, seg, sizeof(seg));
    for (unsigned i = 0; i < 2; i++)
        tp_send(&x.p, TP_ACK | TP_PSH, seg, sizeof(seg));
    CHECK(fix_acked_all(&x));
    CHECK(wnd_is_room(&x));
    CHECK_EQ(sockring_ready(&x.prog.rx), 2 * STACK_TCP_MSS);
    fix_reset_by_peer(&x);
    fix_free_big(&x, &b);
    return true;
}

static bool scaled_tx(void)
{
    struct cx x;
    struct fix_big b = { 0 };
    uint32_t most;
    CHECK(fix_open_big(&x, &b, SCALING(8081), 256 * 1024, 4096));
    CHECK(in_flight(&x, 7, 40, 0xffff, &most));
    CHECK(most > 0xffff);   /* a 256 KiB send buffer: more than an unscaled window */
    fix_reset_by_peer(&x);
    fix_free_big(&x, &b);
    CHECK(fix_open_big(&x, &b, SCALING(8082), 64 * 1024, 4096));
    CHECK(in_flight(&x, 8, 40, 64 * 1024, &most));
    CHECK(most <= 64 * 1024);   /* its send buffer is its 64 KiB ring */
    CHECK(most > 32 * 1024);
    fix_reset_by_peer(&x);
    fix_free_big(&x, &b);
    return true;
}

static bool plain_peer(void)
{
    struct cx x;
    struct fix_big b = { 0 };
    CHECK(fix_open_big(&x, &b, PLAIN(8083), 4096, BIG));
    CHECK_EQ(x.window, STACK_TCP_WND_PLAIN);   /* unscaled: the 44 segments, not the ring */
    CHECK(peer_burst(&x, 9, 0, 44, false));
    CHECK_EQ(x.window, 0);   /* all of it used */
    fix_reset_by_peer(&x);
    fix_free_big(&x, &b);
    return true;
}

bool t_nettcp_window_scale(void)
{
    CHECK(fix_up());
    bool ok = scaled_rx() && scaled_tx() && plain_peer();
    CHECK(ok);
    CHECK(fix_down());
    return true;
}

/* ---- a listener's connections ------------------------------------------------------ */

static struct fix_big lst_big[2];
static struct sockring lst_prog[2];

static status_t big_lst_rings(struct ntcp_listener *l, struct ntcp_conn *c)
{
    unsigned i = lst_big[0].map ? 1 : 0;
    struct sockring ss;
    if (lst_big[i].map || !fix_big_make(4096, l->req.rx_size, &ss, &lst_prog[i], &lst_big[i]))
        return ERR_NO_RESOURCES;
    ntcp_conn_rings(c, &ss);
    c->owner = &lst_big[i];
    return OK;
}

static void big_lst_drop(struct ntcp_listener *l, struct ntcp_conn *c)
{
    (void)l;
    fix_big_free(c->owner);
}

/* The peer (scaling or not) opens a connection to l: the SYN-ACK checked,
 * the handshake's ACK sent, the program accepts it. */
static bool lst_open(struct ntcp_listener *l, struct cx *x, uint16_t from, uint8_t wscale,
                     uint32_t win)
{
    struct tp_seg s[4];
    unsigned n;
    *x = (struct cx){ .p = { .ip = PEER_IP, .mac = pkt_peer_mac, .port = from, .our = l->port,
                             .snd = PEER_ISN, .win = 0xffff, .wscale = wscale } };
    tp_send(&x->p, TP_SYN, NULL, 0);
    CHECK(fix_turn(x, s, 4, &n, false));
    CHECK_EQ(n, 1);
    CHECK_EQ(s[0].flags, TP_SYN | TP_ACK);
    CHECK_EQ(s[0].wscale, wscale ? OUR_SHIFT + 1 : 0);   /* only if the SYN offered it */
    CHECK_EQ(s[0].win, win);                             /* a SYN's: never scaled */
    x->p.rcv = s[0].seq + 1;
    tp_send(&x->p, TP_ACK, NULL, 0);
    CHECK_ST(ntcp_accept(l, &x->c), OK);
    x->prog = lst_prog[(struct fix_big *)x->c->owner - lst_big];
    x->acked = x->p.rcv;
    x->window = win;
    return true;
}

bool t_nettcp_listen_scale(void)
{
    struct ntcp_listener *l;
    struct cx x, y;
    CHECK(fix_up());
    struct ntcp_listen_req req = { .port = 6000, .backlog = 4, .rx_size = BIG,
                                   .rings = big_lst_rings, .drop = big_lst_drop };
    CHECK_ST(ntcp_listen(&req, &l), OK);
    CHECK(lst_open(l, &x, 31000, PEER_SHIFT + 1, 0xffff));
    CHECK(peer_burst(&x, 11, 0, 44, true));   /* what the SYN-ACK's 65535 allowed; then the */
    CHECK(peer_burst(&x, 11, 44 * STACK_TCP_MSS, 100, true));   /* scaled window: past 64 KiB */
    CHECK(lst_open(l, &y, 31001, 0, STACK_TCP_WND_PLAIN));   /* a peer that doesn't scale */
    CHECK(peer_burst(&y, 12, 0, 44, false));
    CHECK_EQ(y.window, 0);               /* its 64240, all of it used: none left */
    fix_reset_by_peer(&x);
    fix_reset_by_peer(&y);
    ntcp_conn_free(x.c);
    ntcp_conn_free(y.c);
    fix_big_free(&lst_big[0]);
    fix_big_free(&lst_big[1]);
    ntcp_unlisten(l);
    CHECK(fix_down());
    return true;
}

/* ---- lwIP's memory shared out ------------------------------------------------------ */

/* Fill x's tx ring (stream id) and let netstack move what it may into
 * lwIP; the peer acks nothing. Bytes netstack took. */
static uint32_t stuff(struct cx *x, uint32_t id)
{
    static uint8_t tmp[16 * 1024];
    static struct tp_seg s[TP_CAP];
    uint64_t put = 0;
    for (uint32_t room; (room = sockring_room(&x->prog.tx)) != 0; put += room) {
        room = room < sizeof(tmp) ? room : (uint32_t)sizeof(tmp);
        tp_fill(id, put, tmp, room);
        (void)sockring_stream_write(&x->prog.tx, tmp, room);   /* room: all of it */
    }
    fix_publish(x, &x->prog.tx);
    unsigned n;
    for (unsigned i = 0; i < 3; i++)
        (void)fix_turn(x, s, TP_CAP, &n, false);   /* the frames are dropped below */
    tp_forget();
    return (uint32_t)(put - (x->prog.tx.size - sockring_room(&x->prog.tx)));
}

bool t_nettcp_heap_shares(void)
{
    static struct cx bulk[3];
    static struct fix_big bb[3];
    struct cx small;
    struct fix_big sb = { 0 };
    uint32_t took = 0;
    CHECK(fix_up());
    tp_room(0);   /* the card takes nothing: every byte stays in lwIP */
    for (unsigned i = 0; i < 3; i++) {
        tp_room(TP_CAP);
        CHECK(fix_open_big(&bulk[i], &bb[i], SCALING((uint16_t)(8100 + i)), BIG, 4096));
        tp_room(0);
        took += stuff(&bulk[i], 20 + i);
    }
    struct stack_counts c;
    stack_get_counts(&c);
    printf("utest: nettcp: three bulk senders took %u bytes; lwIP's heap %u of %u bytes, "
           "%u segments\n", took, c.heap_used, STACK_HEAP, fix_counts().segs_used);
    CHECK(took >= 2 * STACK_TCP_SND_MAX);   /* two whole bulk senders fit, the third ... */
    CHECK(took < 3 * STACK_TCP_SND_MAX);    /* ... gets what is left of their part: */
    /* the ordinary connections' part is left, but for the bulk senders' own
     * first 64240 bytes (and their segments' cost) */
    CHECK(STACK_HEAP - c.heap_used >=
          STACK_HEAP_KEEP + STACK_HEAP_KEEP_BULK - 3 * (STACK_TCP_SND_MIN + STACK_TCP_SND_MIN / 8));
    tp_room(TP_CAP);
    CHECK(fix_open_big(&small, &sb, PLAIN(8110), 16 * 1024, 4096));
    tp_room(0);
    CHECK_EQ(stuff(&small, 30), 16 * 1024);   /* an ordinary connection still sends */
    stack_get_counts(&c);
    CHECK(c.heap_used + STACK_HEAP_KEEP <= STACK_HEAP);   /* and the stack's own part is left */
    tp_room(TP_CAP);
    fix_reset_by_peer(&small);
    fix_free_big(&small, &sb);
    for (unsigned i = 0; i < 3; i++) {
        fix_reset_by_peer(&bulk[i]);
        fix_free_big(&bulk[i], &bb[i]);
    }
    CHECK(fix_down());
    return true;
}
