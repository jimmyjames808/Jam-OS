/* The Wayland socket (docs/G2-PLAN.md section 1): /svc/wayland's framing
 * (svcsock.h), libwayland's byte stream and SCM_RIGHTS descriptors as the
 * compositor's JWL2 batches and handles (<jwl.h>'s transport), so the
 * compositor sees a ported client exactly as it sees one of libjwl's.
 *
 * **Sending** (the stream into batches). Bytes are cut at message
 * boundaries: each message's 8-byte header gives its size (under 8, over
 * JWL_MSG_MAX or not a multiple of 4 is EINVAL, as the compositor would
 * refuse it anyway), whole messages go into batches of up to
 * JWL_BATCH_MAX bytes, and the one message a call ends in the middle of
 * waits in `part` for the rest (libwayland's one byte with descriptors is
 * the first byte of such a message). A whole message counts as taken only
 * once the batch holding it is written, so nothing the program was told
 * went can sit here unsent: a channel that is full takes nothing more,
 * and the call returns what it took, or EAGAIN when that is nothing
 * (libwayland then polls for POLLOUT): a stream socket's partial writes.
 *
 * **Handles ride with the next batch written** after the descriptors they
 * stand for came (<jwl.h>'s rule: a message's handles come no later than
 * the message), at most JWL_BATCH_HANDLES waiting, more is EINVAL. A
 * write takes the handles it carries whatever its status (and a VMO's
 * narrowed handle can't be duplicated to keep), so descriptors are taken
 * only when the channel has room for a batch: we are the end's only
 * writer, so room seen stays room until we write, and the batch that
 * carries them can't be refused for being full. A call with descriptors
 * on a full channel takes nothing (EAGAIN). What crosses is fdpass.h's
 * table under this policy: memfds, as pools the compositor may read and
 * map but not write or duplicate, and pipe and socketpair ends; any other
 * descriptor is EINVAL before a byte is taken.
 *
 * **Receiving** (batches into the stream). The next batch is read only
 * when the last is used up; its header is checked (the magic, the reserved
 * words, the handle count it claims against what it carries) and so is
 * every handle's kind (a VMO or a channel end: what the compositor sends):
 * anything else ends the connection with ECONNRESET, its handles closed,
 * as libjwl's client stops at a compositor that breaks the protocol. A
 * batch's handles are delivered as descriptors, all of them, with its
 * first bytes (a handles-only batch's wait for the next bytes); a caller
 * with less room gets what fits, the rest are closed and MSG_CTRUNC is
 * set, as Linux does; read and recv, with no room, close them all. Bytes
 * go as the caller's buffer allows, one batch's at most per call.
 *
 * **Ending.** A broken batch, or a write the kernel refused after taking
 * its handles, ends the connection: its error is returned once (by the
 * next call either way), then reads see the end and writes EPIPE. The
 * compositor closing is the end of the stream once its last batch (its
 * wl_display.error, if it sent one) is read.
 *
 * **Locks.** Everything here is under the description's lock for one
 * try's bookkeeping; a call that must wait gives it up first and waits on
 * the channel. ready (poll) reads in_at, in_len and dead, which are
 * stored atomically. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jwl.h>
#include <string.h>
#include "fdpass.h"
#include "svcsock.h"

#define HDR     JWL_HEADER_BYTES
#define MSG_MIN 8u   /* a message's header: its object id, its size and opcode */

/* What the Wayland socket passes, and the rights a pool goes with: the
 * compositor reads and maps it, asks its size, and keeps the handle. */
static const struct fdpass_policy wl_policy = {
    .kinds = FDPASS_MEMFD | FDPASS_CHANNEL,
    .memfd_rights = RIGHT_READ | RIGHT_MAP | RIGHT_INSPECT | RIGHT_TRANSFER,
};

/* A connection: the description's lock guards it all (svcsock.h). */
struct wlconn {
    handle_t ch;                          /* the channel end to the compositor */
    int      dead;                        /* the errno it ended with; 0 alive; atomic */
    bool     said;                        /* dead was returned once */
    /* sending */
    uint32_t part_len;                    /* bytes of the message begun, in part */
    uint32_t out_nh;                      /* handles waiting in out_h */
    handle_t out_h[JWL_BATCH_HANDLES];    /* for the next batch written */
    uint8_t  part[JWL_MSG_MAX];           /* the message begun and not finished */
    uint8_t  out[JWL_BATCH_MAX];          /* the batch being filled, its header's room first */
    /* receiving */
    uint32_t in_len, in_at;               /* the batch's bytes, and those delivered; atomic
                                           * (ready reads them) */
    uint32_t in_nh;                       /* handles in in_h, to go with the next bytes */
    handle_t in_h[JWL_HANDLES_QUEUED];
    uint8_t  in[JWL_BATCH_MAX];           /* the batch being read */
};

/* The bytes of one call: iov, total bytes, and how far it has been taken. */
struct src {
    const struct iovec *iov;
    int                 iovcnt;
    size_t              total, pos;
};

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void close_all(const handle_t *h, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        jam_handle_close(h[i]);
}

/* The fields poll's ready reads without the lock: every access atomic,
 * the writer's own reads included. */
static int dead_of(const struct wlconn *c)
{
    return __atomic_load_n(&c->dead, __ATOMIC_RELAXED);
}

static uint32_t at_of(const struct wlconn *c)
{
    return __atomic_load_n(&c->in_at, __ATOMIC_RELAXED);
}

static uint32_t len_of(const struct wlconn *c)
{
    return __atomic_load_n(&c->in_len, __ATOMIC_RELAXED);
}

/* The connection ends with err (the first reason is kept): what is
 * waiting either way is dropped. -err. */
static long die(struct wlconn *c, int err)
{
    if (!dead_of(c))
        __atomic_store_n(&c->dead, err, __ATOMIC_RELAXED);
    close_all(c->out_h, c->out_nh);
    c->out_nh = 0;
    c->part_len = 0;
    close_all(c->in_h, c->in_nh);
    c->in_nh = 0;
    __atomic_store_n(&c->in_at, len_of(c), __ATOMIC_RELAXED);
    return -err;
}

/* A call on a dead connection: its error once, then the end (a read's 0)
 * or EPIPE (a write's). */
static long dead_answer(struct wlconn *c, bool write)
{
    if (!c->said) {
        c->said = true;
        return -dead_of(c);
    }
    return write ? -EPIPE : 0;
}

/* The channel has room for a batch now (or the compositor has gone,
 * which the write will say). We are the end's only writer, so room seen
 * stays room until we write. */
static bool room(const struct wlconn *c)
{
    signals_t seen = 0;
    (void)jam_object_wait_one(c->ch, SIG_WRITABLE | SIG_PEER_CLOSED, 0, &seen);
    return seen & (SIG_WRITABLE | SIG_PEER_CLOSED);
}

/* Wait for sig or the compositor's end on the channel, with no lock held:
 * 0, or -EINTR. */
static long wait_for(const struct wlconn *c, signals_t sig)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(c->ch, sig | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
    return st == OK ? 0 : lx_err(st);
}

static bool nonblocking(struct fd_desc *d, int flags)
{
    return (fd_status(d) & O_NONBLOCK) || (flags & MSG_DONTWAIT);
}

/* ---- sending ---------------------------------------------------------------------------- */

static size_t src_take(struct src *s, uint8_t *to, size_t want)
{
    size_t k = s->total - s->pos < want ? s->total - s->pos : want;
    if (fd_iov_gather(s->iov, s->iovcnt, s->pos, to, k))
        memset(to, 0, k);   /* unmapped since the send's check (fd.h): never stale bytes */
    s->pos += k;
    return k;
}

/* The size a message's header gives (its second word's upper half). */
static uint32_t msg_size(const uint8_t *msg)
{
    return rd32(msg + 4) >> 16;
}

static bool size_ok(uint32_t size)
{
    return size >= MSG_MIN && size <= JWL_MSG_MAX && size % 4 == 0;
}

/* Take source bytes into the message begun: its header, then the rest of
 * it. false when the header is no message's: the bytes of it this call
 * brought are given back to the source (so a bad header is never taken,
 * and every later call stops at it too). */
static bool part_fill(struct wlconn *c, struct src *s)
{
    if (c->part_len < MSG_MIN) {
        size_t k = src_take(s, c->part + c->part_len, MSG_MIN - c->part_len);
        c->part_len += (uint32_t)k;
        if (c->part_len < MSG_MIN)
            return true;
        if (!size_ok(msg_size(c->part))) {
            c->part_len -= (uint32_t)k;
            s->pos -= k;
            return false;
        }
    }
    c->part_len += (uint32_t)src_take(s, c->part + c->part_len, msg_size(c->part) - c->part_len);
    return true;
}

/* Fill the batch (*len bytes so far, its header's room included) with
 * whole messages from the source. 0: the source is used up (its last
 * message, if unfinished, waits in part); 1: the batch is full, another
 * round; -EAGAIN: the channel has no room for a batch; -EINVAL: a bad
 * header. A message that doesn't go into this batch is given back whole. */
static long fill_batch(struct wlconn *c, struct src *s, uint32_t *len)
{
    while (s->pos < s->total) {
        size_t pos0 = s->pos;
        uint32_t part0 = c->part_len;
        if (!part_fill(c, s))
            return -EINVAL;
        if (c->part_len < MSG_MIN || c->part_len < msg_size(c->part))
            return 0;   /* the source ended inside the message */
        bool first = *len == HDR;
        if (*len + c->part_len > JWL_BATCH_MAX || (first && !room(c))) {
            s->pos = pos0;
            c->part_len = part0;
            return first ? -EAGAIN : 1;
        }
        memcpy(c->out + *len, c->part, c->part_len);
        *len += c->part_len;
        c->part_len = 0;
    }
    return 0;
}

/* Write the batch (len bytes) with every handle waiting: 0, or the
 * connection's end (the handles went with the write, whatever its
 * status). */
static long batch_write(struct wlconn *c, uint32_t len)
{
    uint32_t nh = c->out_nh;
    wr32(c->out, JWL_MAGIC);
    wr32(c->out + 4, nh);
    wr32(c->out + 8, 0);
    wr32(c->out + 12, 0);
    c->out_nh = 0;
    status_t st = jam_channel_write(c->ch, c->out, len, c->out_h, nh);
    if (st == OK)
        return 0;
    return die(c, st == ERR_PEER_CLOSED ? EPIPE : st == ERR_NO_MEMORY ? ENOBUFS : -lx_err(st));
}

/* Batches from the source until it is used up (0), the channel is full
 * (-EAGAIN), a bad header (-EINVAL) or the connection ended (its -errno).
 * s->pos is past every byte taken: a batch the kernel refused took none. */
static long send_batches(struct wlconn *c, struct src *s)
{
    for (;;) {   /* each round writes one batch, or ends */
        uint32_t len = HDR;
        size_t pos0 = s->pos;
        long r = fill_batch(c, s, &len);
        if (len > HDR) {
            long w = batch_write(c, len);
            if (w) {
                s->pos = pos0;   /* never delivered: the call says so if it took nothing else */
                return w;
            }
        }
        if (r != 1)
            return r;
    }
}

/* One try, the description's lock held: the bytes taken (the nh handles
 * hs taken with them, *nh set to 0, if any byte was), or -errno: -EAGAIN
 * (nothing taken: the channel is full), -EINVAL (too many handles
 * waiting, or a bad header first). */
static long send_try(struct wlconn *c, struct src *s, handle_t *hs, unsigned *nh)
{
    if (dead_of(c))
        return dead_answer(c, true);
    unsigned queued = *nh;
    if (queued && queued > JWL_BATCH_HANDLES - c->out_nh)
        return -EINVAL;   /* more waiting than one batch carries */
    if (queued && !room(c))
        return -EAGAIN;   /* handles go only where a batch can follow them */
    memcpy(c->out_h + c->out_nh, hs, queued * sizeof(handle_t));
    c->out_nh += queued;
    *nh = 0;
    size_t from = s->pos;
    long r = send_batches(c, s);
    if (s->pos > from)
        return (long)(s->pos - from);   /* an end met after them is said by the next call */
    if (dead_of(c))
        return dead_answer(c, true);    /* the handles went with the connection */
    if (queued) {   /* nothing taken: the handles are the caller's again */
        c->out_nh -= queued;
        memcpy(hs, c->out_h + c->out_nh, queued * sizeof(handle_t));
        *nh = queued;
    }
    return r;
}

static long wl_sendmsg(struct fd_desc *d, void *conn, const struct msghdr *m, int flags)
{
    struct wlconn *c = conn;
    int fds[JWL_BATCH_HANDLES];
    handle_t hs[JWL_BATCH_HANDLES];
    long n = fdpass_cmsg_fds(m, fds, JWL_BATCH_HANDLES);   /* 29 or more: EINVAL */
    long total = n < 0 ? n : fd_iov_total(m->msg_iov, m->msg_iovlen);
    if (total <= 0)
        return total;   /* an error, or nothing to send: no descriptor goes without a byte */
    long r = n ? fdpass_fds_out(fds, (unsigned)n, &wl_policy, hs) : 0;
    if (r)
        return r;
    unsigned nh = (unsigned)n;
    struct src s = { .iov = m->msg_iov, .iovcnt = m->msg_iovlen, .total = (size_t)total };
    bool nb = nonblocking(d, flags);
    for (;;) {   /* each turn takes bytes, or waits for room, or ends */
        lock_take(&d->lock);
        r = send_try(c, &s, hs, &nh);
        lock_give(&d->lock);
        if (r == -EAGAIN && !nb) {
            if ((r = wait_for(c, SIG_WRITABLE)) != 0)
                break;
            continue;
        }
        if (r <= 0 || nb || s.pos == s.total)
            break;
    }
    close_all(hs, nh);   /* never taken */
    return s.pos ? (long)s.pos : r;
}

/* ---- receiving -------------------------------------------------------------------------- */

/* Every handle is one the compositor sends: a VMO or a channel end. */
static bool kinds_ok(const handle_t *h, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (!fdpass_kind_of(h[i], wl_policy.kinds))
            return false;
    return true;
}

/* Read the next batch into in (the last one is used up): 0; 1 at the
 * end (the compositor closed, nothing left); -EAGAIN; or the connection's
 * end (ECONNRESET for a batch that breaks JWL2). */
static long batch_read(struct wlconn *c)
{
    uint32_t nb = 0, nh = 0;
    handle_t h[JWL_BATCH_HANDLES];
    struct channel_read_args a = {
        .h = c->ch,
        .rd_cap = JWL_BATCH_MAX,
        .rd_bytes = (uint64_t)(uintptr_t)c->in,
        .rd_actual = (uint64_t)(uintptr_t)&nb,
        .rd_handles = (uint64_t)(uintptr_t)h,
        .rd_hcap = JWL_BATCH_HANDLES,
        .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_SHOULD_WAIT)
        return -EAGAIN;
    if (st == ERR_PEER_CLOSED)
        return 1;
    if (st == ERR_BUFFER_TOO_SMALL)
        return die(c, ECONNRESET);   /* over 16 KiB or 28 handles */
    if (st != OK)
        return die(c, -lx_err(st));
    bool ok = nb >= HDR && rd32(c->in) == JWL_MAGIC && rd32(c->in + 4) == nh &&
              !rd32(c->in + 8) && !rd32(c->in + 12) && nh <= JWL_HANDLES_QUEUED - c->in_nh &&
              kinds_ok(h, nh);
    if (!ok) {
        close_all(h, nh);
        return die(c, ECONNRESET);
    }
    memcpy(c->in_h + c->in_nh, h, nh * sizeof(handle_t));
    c->in_nh += nh;
    __atomic_store_n(&c->in_at, HDR, __ATOMIC_RELAXED);
    __atomic_store_n(&c->in_len, nb, __ATOMIC_RELAXED);
    return 0;
}

/* Bytes to deliver in in (0), or why not: 1 at the end, -EAGAIN, or the
 * connection's end. A run of batches with no bytes is bounded as libjwl
 * bounds it (JWL_EMPTY_RUN): the compositor sends none. */
static long next_bytes(struct wlconn *c)
{
    for (unsigned i = 0; i <= JWL_EMPTY_RUN; i++) {
        if (at_of(c) < len_of(c))
            return 0;
        long r = batch_read(c);
        if (r)
            return r;
    }
    return die(c, ECONNRESET);
}

/* One try, the description's lock held: the bytes delivered (and the
 * waiting handles as descriptors in m's control), 0 at the end, or
 * -errno (-EAGAIN: nothing yet). */
static long recv_try(struct wlconn *c, struct msghdr *m, size_t total, int flags)
{
    if (dead_of(c))
        return dead_answer(c, false);
    long r = next_bytes(c);
    if (r)
        return r == 1 ? 0 : dead_of(c) ? dead_answer(c, false) : r;
    int fds[JWL_HANDLES_QUEUED];
    struct fdpass_sink sink = { .fds = fds, .room = fdpass_cmsg_room(m),
                                .cloexec = flags & MSG_CMSG_CLOEXEC };
    fdpass_fds_in(c->in_h, c->in_nh, wl_policy.kinds, &sink);   /* consumes them all */
    c->in_nh = 0;
    uint32_t at = at_of(c), left = len_of(c) - at;
    size_t n = left < total ? left : total;
    if (fd_iov_scatter(m->msg_iov, m->msg_iovlen, 0, c->in + at, n))
        return -EFAULT;   /* the bytes stay; the descriptors are in the table */
    __atomic_store_n(&c->in_at, at + (uint32_t)n, __ATOMIC_RELAXED);
    fdpass_cmsg_put(m, fds, sink.got);
    m->msg_flags = sink.cut ? MSG_CTRUNC : 0;
    m->msg_namelen = 0;   /* the compositor has no name */
    return (long)n;
}

static long wl_recvmsg(struct fd_desc *d, void *conn, struct msghdr *m, int flags)
{
    struct wlconn *c = conn;
    long total = fd_iov_total(m->msg_iov, m->msg_iovlen);
    if (total <= 0) {
        m->msg_controllen = 0;
        return total;   /* an error, or no room for a byte (and so for no descriptor) */
    }
    bool nb = nonblocking(d, flags);
    for (;;) {   /* each turn delivers, ends, or waits for a batch */
        lock_take(&d->lock);
        long r = recv_try(c, m, (size_t)total, flags);
        lock_give(&d->lock);
        if (r != -EAGAIN || nb)
            return r;
        if ((r = wait_for(c, SIG_READABLE)) != 0)
            return r;
    }
}

/* ---- the rest of the framing ------------------------------------------------------------ */

static void *wl_open(handle_t ch)
{
    struct wlconn *c = jam_calloc(1, sizeof(*c));
    if (!c) {
        jam_handle_close(ch);
        return NULL;
    }
    c->ch = ch;
    return c;
}

/* poll: bytes of a batch not yet delivered, or an end to say. */
static uint32_t wl_ready(void *conn, uint32_t interest)
{
    const struct wlconn *c = conn;
    if (dead_of(c))
        return NETWAIT_READ | NETWAIT_WRITE;   /* every call answers at once */
    return at_of(c) < len_of(c) ? NETWAIT_READ : 0;
}

static long wl_arm(void *conn, uint32_t interest, struct fd_wait *out)
{
    const struct wlconn *c = conn;
    out->kind = FD_WAIT_HANDLE;
    out->h = (struct netwait_handle){ .h = c->ch, .read = SIG_READABLE, .write = SIG_WRITABLE,
                                      .hup = SIG_PEER_CLOSED };
    return 0;
}

static void wl_close(void *conn)
{
    struct wlconn *c = conn;
    close_all(c->out_h, c->out_nh);
    close_all(c->in_h, c->in_nh);
    jam_handle_close(c->ch);
    jam_free(c);
}

const struct svcsock_framing wlsock_framing = {
    .name = "wayland",
    .open = wl_open,
    .sendmsg = wl_sendmsg,
    .recvmsg = wl_recvmsg,
    .ready = wl_ready,
    .arm = wl_arm,
    .close = wl_close,
};
