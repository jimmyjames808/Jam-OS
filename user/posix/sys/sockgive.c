/* A socket handed to a child (posix_spawn, B3's spawnfd.c) and a
 * child's socket from its hand-over (fd.c's fd_adopters): sock.h's
 * model.
 *
 * **It moves.** A socket's ring VMO and events can't be duplicated
 * (<sockring.h>), so the child gets the handles themselves and this
 * side's private state (the rings' counts, the ends seen, the options) as
 * the record's data; the description here is SK_GONE (every call
 * ENOTCONN), its rings unmapped. A spawn that fails after that loses the
 * socket, as a pipe's leftover is lost (pipe.c). A socket still SK_NEW
 * has nothing to move: the child doesn't get it. A listener moves with
 * its accept in flight (its txid), so the child takes that answer.
 *
 * netstack ends every socket of an opener that goes (sock.h): a child
 * keeps a socket only while the process that made it lives. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "kinds.h"
#include "sock.h"

#define GIVEN_MAGIC  0x4b434f53u       /* "SOCK": a hand-over's data, this layer's own */

/* What a socket handed to a child carries besides its handles. */
struct sock_given {
    uint32_t         magic;        /* GIVEN_MAGIC */
    uint32_t         type;         /* SOCK_STREAM or SOCK_DGRAM */
    uint32_t         tx_size;      /* the rings' sizes (the ring page says them too) */
    uint32_t         rx_size;
    uint64_t         tx_count;     /* this side's counts, published already */
    uint64_t         rx_count;
    uint64_t         rx_end_at;    /* the rx ring's end, if seen */
    uint32_t         bind_addr;    /* sock's own */
    uint16_t         bind_port;
    uint16_t         port;         /* ns.port, or a listener's l.port */
    uint32_t         peer_addr;    /* a datagram socket's peer */
    uint16_t         peer_port;
    uint8_t          flags;        /* GIVEN_* */
    uint8_t          reserved;
    uint32_t         txid;         /* a listener's accept in flight (net_listener's) */
    uint32_t         last_txid;
    struct sock_opts opts;
};

#define GIVEN_RD_SHUT   (1u << 0)
#define GIVEN_WR_SHUT   (1u << 1)
#define GIVEN_TX_ENDED  (1u << 2)
#define GIVEN_RX_ENDED  (1u << 3)
#define GIVEN_CONNECTED (1u << 4)
#define GIVEN_ASKED     (1u << 5)
#define GIVEN_ERR_SAID  (1u << 6)

/* Undo net_sock_attach's map and forget the rings (the handles are
 * someone else's now, or closed by the caller). */
static void rings_drop(struct net_sock *ns)
{
    if (ns->map)
        (void)jam_vmar_unmap(lx_self_vmar(), (uint64_t)(uintptr_t)ns->map, ns->map_len);
    if (ns->waiter)
        jam_handle_close(ns->waiter);
    *ns = (struct net_sock){ 0 };
}

static void given_fill(const struct sock *s, struct sock_given *g)
{
    *g = (struct sock_given){ .magic = GIVEN_MAGIC, .type = (uint32_t)s->type,
                              .bind_addr = s->bind_addr, .bind_port = s->bind_port,
                              .peer_addr = s->peer_addr, .peer_port = s->peer_port,
                              .opts = s->opts };
    g->flags = (s->rd_shut ? GIVEN_RD_SHUT : 0) | (s->wr_shut ? GIVEN_WR_SHUT : 0) |
               (s->connected ? GIVEN_CONNECTED : 0) | (s->err_said ? GIVEN_ERR_SAID : 0);
    if (sock_state(s) == SK_LISTENING) {
        g->port = s->l.port;
        g->flags |= s->l.asked ? GIVEN_ASKED : 0;
        g->txid = s->l.txid;
        g->last_txid = s->l.last_txid;
        return;
    }
    g->port = s->ns.port;
    g->tx_size = s->ns.r.tx.size;
    g->rx_size = s->ns.r.rx.size;
    g->tx_count = s->ns.r.tx.count;
    g->rx_count = s->ns.r.rx.count;
    g->rx_end_at = s->ns.r.rx.end_at;
    g->flags |= (s->ns.r.tx.ended ? GIVEN_TX_ENDED : 0) | (s->ns.r.rx.ended ? GIVEN_RX_ENDED : 0);
}

/* Move the socket into out: its handles and its state as data. The
 * description is SK_GONE after. Every lock held: no other call is inside. */
static long give_locked(struct sock *s, struct fd_given *out)
{
    uint32_t st = sock_state(s);
    if (st != SK_STREAM && st != SK_DGRAM && st != SK_LISTENING)
        return -ENOTSUP;   /* nothing at netstack yet: the child doesn't get it */
    long r = 0;
    struct sock_given g;
    given_fill(s, &g);
    void *data = kind_data(&g, sizeof(g), &r);
    if (!data)
        return r;
    *out = (struct fd_given){ .status = 0, .data = data, .data_len = sizeof(g) };
    if (st == SK_LISTENING) {
        out->kind = POSIXFDS_LISTENER;
        out->handles[0] = s->l.ch;
        out->nhandles = 1;
        s->l = (struct net_listener){ 0 };
    } else {
        out->kind = POSIXFDS_SOCKET;
        out->handles[0] = s->ns.ch;
        out->handles[1] = s->ns.ring;
        out->handles[2] = s->ns.to_stack;
        out->handles[3] = s->ns.to_prog;
        out->nhandles = 4;
        rings_drop(&s->ns);
    }
    s->sleepers[SIDE_RX] = s->sleepers[SIDE_TX] = 0;
    sock_state_set(s, SK_GONE);
    return 0;
}

long sock_give(struct fd_desc *d, struct fd_given *out)
{
    struct sock *s = d->priv;
    if (sock_state(s) == SK_STREAM)
        sock_streams_remove(s);   /* first: never under the three locks */
    lock_take(&d->lock);
    lock_take(&s->rx_lock);
    lock_take(&s->tx_lock);
    long r = give_locked(s, out);
    if (!r)
        out->status = fd_status(d);
    lock_give(&s->tx_lock);
    lock_give(&s->rx_lock);
    lock_give(&d->lock);
    if (r && sock_state(s) == SK_STREAM)
        sock_streams_add(s);
    return r;
}

/* ---- a child's sockets ------------------------------------------------------------------- */

static long given_read(const struct posixfds_fd *rec, const void *data, struct sock_given *g)
{
    if (rec->data_len != sizeof(*g))
        return -EINVAL;
    memcpy(g, data, sizeof(*g));
    if (g->magic != GIVEN_MAGIC || (g->type != SOCK_STREAM && g->type != SOCK_DGRAM))
        return -EINVAL;
    return 0;
}

static struct sock *sock_from_given(const struct sock_given *g)
{
    struct sock *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->type = (int)g->type;
    s->bind_addr = g->bind_addr;
    s->bind_port = g->bind_port;
    s->bound = g->bind_port || g->bind_addr;
    s->peer_addr = g->peer_addr;
    s->peer_port = g->peer_port;
    s->connected = g->flags & GIVEN_CONNECTED;
    s->rd_shut = g->flags & GIVEN_RD_SHUT;
    s->wr_shut = g->flags & GIVEN_WR_SHUT;
    s->err_said = g->flags & GIVEN_ERR_SAID;
    s->opts = g->opts;
    return s;
}

/* Map the rings the parent had and take its counts on (its trusted copies:
 * the counts it published last). */
static status_t rings_take(struct sock *s, const struct sock_given *g, const handle_t *hs)
{
    uint32_t framing = g->type == SOCK_STREAM ? SOCKRING_STREAM : SOCKRING_DGRAM;
    uint64_t va = 0, len = sockring_bytes(g->tx_size, g->rx_size);
    if (!sockring_size_ok(g->tx_size) || !sockring_size_ok(g->rx_size))
        return ERR_INVALID_ARGS;
    status_t st = jam_vmar_map(lx_self_vmar(), hs[1], 0, len, VMAR_READ | VMAR_WRITE, &va);
    if (st != OK)
        return st;
    s->ns = (struct net_sock){ .ch = hs[0], .port = g->port, .ring = hs[1], .to_stack = hs[2],
                               .to_prog = hs[3], .map = (uint8_t *)(uintptr_t)va,
                               .map_len = len };
    st = sockring_attach(&s->ns.r, s->ns.map, len, framing, g->tx_size, g->rx_size);
    if (st != OK) {
        (void)jam_vmar_unmap(lx_self_vmar(), va, len);
        s->ns = (struct net_sock){ 0 };
        return st;
    }
    s->ns.r.tx.count = g->tx_count;
    s->ns.r.tx.ended = g->flags & GIVEN_TX_ENDED;
    s->ns.r.rx.count = g->rx_count;
    s->ns.r.rx.ended = g->flags & GIVEN_RX_ENDED;
    s->ns.r.rx.end_at = g->rx_end_at;
    struct sockring_status ss;
    sockring_status_get(&s->ns.r, &ss);
    s->refused = ss.tx_refused;
    return OK;
}

long sock_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                struct fd_desc **out)
{
    struct sock_given g;
    long r = given_read(rec, data, &g);
    struct sock *s = r ? NULL : sock_from_given(&g);
    status_t st = s ? rings_take(s, &g, hs) : ERR_NO_MEMORY;
    if (st != OK) {
        for (unsigned i = 0; i < 4; i++)
            jam_handle_close(hs[i]);
        free(s);
        return r ? r : lx_err(st);
    }
    sock_state_set(s, g.type == SOCK_STREAM ? SK_STREAM : SK_DGRAM);
    *out = fd_new(&sock_ops, rec->status, s);
    if (!*out) {
        net_close(&s->ns);
        free(s);
        return -ENOMEM;
    }
    if (g.type == SOCK_STREAM)
        sock_streams_add(s);
    return 0;
}

long sock_listener_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                         struct fd_desc **out)
{
    struct sock_given g;
    long r = given_read(rec, data, &g);
    struct sock *s = r || g.type != SOCK_STREAM ? NULL : sock_from_given(&g);
    if (!s) {
        jam_handle_close(hs[0]);
        return r ? r : -EINVAL;
    }
    s->l = (struct net_listener){ .ch = hs[0], .port = g.port, .asked = g.flags & GIVEN_ASKED,
                                  .txid = g.txid, .last_txid = g.last_txid };
    sock_state_set(s, SK_LISTENING);
    *out = fd_new(&sock_ops, rec->status, s);
    if (!*out) {
        net_listener_close(&s->l);
        free(s);
        return -ENOMEM;
    }
    return 0;
}
