/* Socketpair ends (docs/CR-PLAN.md section 4): AF_UNIX SOCK_STREAM and
 * SOCK_SEQPACKET pairs on a channel (pipe.c makes them; pipe.h has the
 * description's state), carrying descriptors (SCM_RIGHTS) as Mojo's
 * channels need: sendmsg and recvmsg with MSG_DONTWAIT, MSG_PEEK,
 * MSG_WAITALL, MSG_CTRUNC, MSG_CMSG_CLOEXEC, and shutdown(SHUT_RDWR).
 *
 * **Messages.** A message with no handles is plain bytes, as a pipe's:
 * a program that sends no descriptors sends what it always did. A send
 * with descriptors is one or more messages, its **parts**, each starting
 * with a header (struct sp_hdr) and the hand-over's records of some of
 * the descriptors (<posixfds.h>: struct posixfds_fd and its data, padded
 * to 8), carrying their handles; the bytes ride in the last part, after
 * its records. A part holds at most CHANNEL_MAX_HANDLES handles and
 * SP_REC_MAX bytes of records, so 253 descriptors (Linux's most, more is
 * EINVAL) of any kind fit; most sends are one part. A part whose
 * descriptors have no handle (/dev nodes) carries an event instead, so a
 * message with handles is always a part and one without never is.
 * fdpass.c makes the records (what crosses, and as what) and the
 * descriptions from them.
 *
 * **One send at a time** per description (send_lock, held across the
 * send's waits): a send's parts and bytes are never cut into by another
 * thread's. Another process holding the same end could still write
 * between two parts; the receiver then drops the parts it held (below).
 * A write takes the handles it carries whatever its status (the kernel's
 * rule), so the records are made only once the channel has room, and a
 * part the channel refused for being full drops the whole send: blocking,
 * it is made again and sent again; with MSG_DONTWAIT or O_NONBLOCK it is
 * EAGAIN, as if nothing went (the parts already written are dropped by
 * the receiver when the next send's first part or bytes come).
 *
 * **Receiving.** A part's records are checked (the header's fields, each
 * record's place, sizes and handle count) and kept with their handles in
 * the description (struct sp_fds) until the send's last part; then its
 * descriptors go with the first byte of its bytes, as Linux attaches them
 * to a send's first byte: the recvmsg that takes that byte installs them
 * (fdpass_adopt, MSG_CMSG_CLOEXEC honoured), as many as the caller's
 * control room holds; the rest are closed and MSG_CTRUNC set. read, recv
 * and a recvmsg with no room close them all. A stream read never takes
 * bytes from two messages, so a send's descriptors always come with the
 * first byte of a call. A part that breaks the format: its handles
 * closed, the descriptors of its send dropped, and MSG_CTRUNC on the next
 * delivery; a message with handles but no part's header at all is
 * delivered as plain bytes, its handles closed (as a pipe does). Parts of
 * a send that never finished are dropped silently when the next send's
 * first part or a plain message comes (their sender was told).
 *
 * **SOCK_SEQPACKET**: one send is one message, at most PIPE_SIZE bytes
 * (less its parts' records with descriptors; more: EMSGSIZE), empty ones
 * too; a read takes one whole message, the rest of it discarded
 * (MSG_TRUNC in msg_flags; with MSG_TRUNC asked, the message's length is
 * returned). MSG_PEEK keeps the message (kept) for the next read.
 *
 * **MSG_PEEK** copies bytes without taking them; descriptors stay for
 * the read that takes the byte (Linux would install duplicates).
 * **MSG_WAITALL** (a stream, one buffer) reads on until the buffer is
 * full, the end, or a send's descriptors came.
 *
 * **shutdown(SHUT_RDWR)** closes the description's channel end: the peer
 * reads end of file, its writes get EPIPE; here reads see the end and
 * writes EPIPE (every descriptor of the description, as Linux shuts the
 * socket; another process's duplicate of the end, sent it with
 * SCM_RIGHTS, keeps the channel open). The end becomes one whose peer is
 * gone (pipe.h's ch), so poll says hung up. A thread already asleep in a
 * read or write of this description sleeps on until the peer writes or
 * goes (nothing in the kernel wakes a wait on an object whose handle
 * closed). SHUT_RD and SHUT_WR alone are EOPNOTSUPP: a channel has no
 * half-close.
 *
 * **Locks**: the description's (fd.h) for the leftover and sp_fds, never
 * across a wait; send_lock above it (taken first), across waits. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "fdpass.h"
#include "pipe.h"

#define SP_MAGIC     0x4d43534au     /* "JSCM": a part of a send with descriptors */
#define SP_HDR       24u             /* struct sp_hdr's bytes */
#define SP_REC_MAX   (32u * 1024)    /* records' bytes in one part; the bytes have the rest */
#define SP_PARTS_MAX FDPASS_MAX      /* each part has a descriptor at least */
#define REC_BYTES    ((uint32_t)sizeof(struct posixfds_fd))
#define PAD8(n)      (((n) + 7u) & ~7u)

/* A part's header. Little-endian, no padding. */
struct sp_hdr {
    uint32_t magic;      /* SP_MAGIC */
    uint32_t cookie;     /* the send's: the same in each of its parts */
    uint16_t part;       /* this part's place, 0 .. parts - 1; the bytes ride in the last */
    uint16_t parts;      /* 1 .. SP_PARTS_MAX */
    uint16_t nfds;       /* records here: at least 1 */
    uint16_t marker;     /* 1: an event after the records' handles, standing for none (the
                          * records have no handle); else 0 */
    uint32_t rec_len;    /* bytes of records after the header */
    uint32_t reserved;   /* 0 */
};
_Static_assert(sizeof(struct sp_hdr) == SP_HDR, "sp_hdr has no padding");

/* What crosses a socketpair: every kind the hand-over passes but the
 * ones fdpass.h names, a memfd with all its description's rights. */
static const struct fdpass_policy sp_policy = {
    .kinds = FDPASS_MEMFD | FDPASS_CHANNEL | FDPASS_FILE | FDPASS_NODE,
    .memfd_rights = RIGHT_SAME,
};

/* One descriptor received and not delivered yet. */
struct sp_fd {
    struct posixfds_fd rec;
    void              *data;                        /* rec.data_len bytes, or NULL */
    handle_t           hs[POSIXFDS_KIND_HANDLES];   /* rec.nhandles of them */
};

/* The descriptors of the send being received (parts > 0), or of the one
 * whose bytes are next (ready). The description's lock. */
struct sp_fds {
    uint32_t     cookie;          /* the send being put together */
    uint16_t     next, parts;     /* its next part, and how many it has; parts 0: none */
    bool         ready;           /* complete: they go with the leftover's first byte */
    bool         lost;            /* a send's descriptors were dropped: MSG_CTRUNC next */
    unsigned     n;               /* fd[0 .. n) */
    struct sp_fd fd[FDPASS_MAX];
};

static uint32_t cookies;   /* the last send's cookie; atomic */

static bool is_shut(const struct pipe_end *p)
{
    return __atomic_load_n(&p->shut, __ATOMIC_ACQUIRE);
}

static bool is_kept(const struct pipe_end *p)
{
    return __atomic_load_n(&p->kept, __ATOMIC_RELAXED);
}

static void set_kept(struct pipe_end *p, bool on)
{
    __atomic_store_n(&p->kept, on, __ATOMIC_RELAXED);
}

static bool nonblocking(struct fd_desc *d, int flags)
{
    return (fd_status(d) & O_NONBLOCK) || (flags & MSG_DONTWAIT);
}

static void close_all(const handle_t *hs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        jam_handle_close(hs[i]);
}

/* ---- descriptors received ---------------------------------------------------------------- */

static void fd_drop(struct sp_fd *e)
{
    close_all(e->hs, e->rec.nhandles);
    free(e->data);
    e->data = NULL;
}

/* Drop every descriptor kept, on the way or ready. */
static void fds_drop(struct sp_fds *f)
{
    for (unsigned i = 0; f && i < f->n; i++)
        fd_drop(&f->fd[i]);
    if (f) {
        f->n = 0;
        f->parts = 0;
        f->ready = false;
    }
}

/* The part's records (len bytes at r, nfds of them) and their handles (hs,
 * nh: all of them, with the marker's last if marker) appended to f.
 * false: malformed; whatever wasn't appended is closed (what was is f's). */
static bool records_take(struct sp_fds *f, const uint8_t *r, uint32_t len, uint16_t nfds,
                         handle_t *hs, uint32_t nh, bool marker)
{
    uint32_t at = 0, used = 0;
    bool ok = true;
    for (uint16_t k = 0; ok && k < nfds; k++) {
        ok = f->n < FDPASS_MAX && len - at >= REC_BYTES;
        if (!ok)
            break;
        struct sp_fd *e = &f->fd[f->n];
        memcpy(&e->rec, r + at, REC_BYTES);
        ok = e->rec.fd == (int32_t)f->n && e->rec.data_len <= POSIXFDS_DATA_MAX &&
             PAD8(e->rec.data_len) <= len - at - REC_BYTES &&
             e->rec.nhandles <= POSIXFDS_KIND_HANDLES && e->rec.nhandles <= nh - used;
        e->data = ok && e->rec.data_len ? malloc(e->rec.data_len) : NULL;
        ok = ok && (e->data || !e->rec.data_len);
        if (!ok)
            break;
        if (e->rec.data_len)
            memcpy(e->data, r + at + REC_BYTES, e->rec.data_len);
        memcpy(e->hs, hs + used, e->rec.nhandles * sizeof(handle_t));
        used += e->rec.nhandles;
        at += REC_BYTES + PAD8(e->rec.data_len);
        f->n++;
    }
    ok = ok && at == len && used + marker == nh && (!marker || !used);
    close_all(hs + used, nh - used);   /* the marker, or what a bad record left */
    return ok;
}

/* What a message with handles was. */
enum part { PART_RAW, PART_MORE, PART_LAST, PART_DROP };

/* p->buf holds a message of got bytes that came with nh handles hs: a
 * part kept (PART_MORE), a send's last (PART_LAST: its bytes from *skip;
 * its descriptors ready, unless they were lost), one dropped
 * (PART_DROP), or no part at all (PART_RAW: plain bytes). The handles
 * are kept or closed. */
static enum part take_part(struct pipe_end *p, uint32_t got, handle_t *hs, uint32_t nh,
                           uint32_t *skip)
{
    struct sp_hdr h;
    if (got >= SP_HDR)
        memcpy(&h, p->buf, SP_HDR);
    if (got < SP_HDR || h.magic != SP_MAGIC || !h.parts || h.parts > SP_PARTS_MAX ||
        h.part >= h.parts || !h.nfds || h.marker > 1 || h.reserved ||
        h.rec_len > got - SP_HDR) {
        close_all(hs, nh);
        return PART_RAW;
    }
    *skip = SP_HDR + h.rec_len;
    bool last = h.part == h.parts - 1;
    if (!p->fds && !(p->fds = calloc(1, sizeof(*p->fds)))) {
        close_all(hs, nh);   /* no memory: the bytes go on alone */
        return last ? PART_LAST : PART_DROP;
    }
    struct sp_fds *f = p->fds;
    bool next = f->parts && h.cookie == f->cookie && h.part == f->next && h.parts == f->parts;
    if (!next)
        fds_drop(f);   /* a send that never finished (or nothing on the way) */
    if (!next && h.part) {   /* the rest of a send whose start was lost */
        close_all(hs, nh);
        f->lost = true;
        return last ? PART_LAST : PART_DROP;
    }
    if (!h.part) {   /* f is empty (dropped above) */
        f->cookie = h.cookie;
        f->parts = h.parts;
        f->next = 0;
    }
    if (!records_take(f, p->buf + SP_HDR, h.rec_len, h.nfds, hs, nh, h.marker)) {
        fds_drop(f);
        f->lost = true;
        return last ? PART_LAST : PART_DROP;
    }
    f->next++;
    if (!last)
        return PART_MORE;
    f->parts = 0;
    f->ready = true;
    return PART_LAST;
}

/* Read messages into the leftover until one has bytes to deliver (a
 * seqpacket's: any message, empty ones too): 1; 0 at the end; -EAGAIN;
 * -errno. The description's lock held. */
static long fill_locked(struct pipe_end *p)
{
    if (!pipe_buf(p))
        return -ENOMEM;
    for (;;) {
        handle_t hs[CHANNEL_MAX_HANDLES];
        uint32_t got, nh, skip = 0;
        status_t st = pipe_read_msg(pipe_ch(p), p->buf, PIPE_SIZE, &got, hs, &nh);
        if (st == ERR_PEER_CLOSED || (st == ERR_BAD_HANDLE && is_shut(p)))
            return 0;   /* parts of a send that never finished stay until the close */
        if (st != OK)
            return st == ERR_SHOULD_WAIT ? -EAGAIN : lx_err(st);
        enum part k = nh ? take_part(p, got, hs, nh, &skip) : PART_RAW;
        if (k == PART_RAW && p->fds && p->fds->parts)
            fds_drop(p->fds);   /* plain bytes after an unfinished send */
        if (k == PART_MORE || k == PART_DROP)
            continue;
        if (!p->seq && got == skip) {   /* a stream's empty message carries nothing */
            if (k == PART_LAST && p->fds && p->fds->ready) {   /* none: take_part's calloc failed */
                fds_drop(p->fds);
                p->fds->lost = true;
            }
            continue;
        }
        pipe_set_leftover(p, skip, got);
        set_kept(p, p->seq);
        return 1;
    }
}

/* The descriptors that came with the leftover's first byte, now being
 * taken, into s (NULL: closed, as a read with no room for them). */
static void deliver_fds(struct pipe_end *p, struct fdpass_sink *s)
{
    struct sp_fds *f = p->fds;
    if (!f)
        return;
    if (s && f->lost)
        s->cut = true;
    f->lost = false;
    for (unsigned i = 0; f->ready && i < f->n; i++) {
        struct sp_fd *e = &f->fd[i];
        if (!s || s->got == s->room) {
            fd_drop(e);
            if (s)
                s->cut = true;
            continue;
        }
        struct fd_desc *d = NULL;
        long r = fdpass_adopt(&e->rec, e->data, e->hs, sp_policy.kinds, &d);   /* hs gone */
        free(e->data);
        e->data = NULL;
        long fd = r ? r : fd_install(d, 0, s->cloexec);   /* d's reference gone */
        if (fd < 0)
            s->cut = true;
        else
            s->fds[s->got++] = (int)fd;
    }
    f->n = 0;
    f->ready = false;
}

/* ---- receiving ------------------------------------------------------------------------- */

/* One receive: where the bytes go, and what came. */
struct rx {
    const struct iovec *iov;
    int                 iovcnt;
    size_t              total;   /* iov's bytes */
    int                 flags;   /* MSG_* */
    struct fdpass_sink *sink;    /* where descriptors go: NULL closes them */
    size_t              whole;   /* out: the bytes there were (a seqpacket's message) */
};

/* One try, the description's lock held: bytes delivered, 0 at the end,
 * -EAGAIN, or -errno. */
static long recv_try(struct pipe_end *p, struct rx *x)
{
    if (is_shut(p))
        return 0;
    bool have = p->seq ? is_kept(p) : p->at < p->have;
    long r = have ? 1 : fill_locked(p);
    if (r <= 0)
        return r;
    uint32_t left = p->have - p->at;
    size_t n = left < x->total ? left : x->total;
    if (fd_iov_scatter(x->iov, x->iovcnt, 0, p->buf + p->at, n))
        return -EFAULT;   /* the message stays for the next read */
    x->whole = left;
    if (x->flags & MSG_PEEK)
        return (long)n;
    deliver_fds(p, x->sink);
    if (p->seq) {
        pipe_set_leftover(p, 0, 0);
        set_kept(p, false);
    } else {
        pipe_set_leftover(p, p->at + (uint32_t)n, p->have);
    }
    return (long)n;
}

/* recv_try until something comes, unless nb. */
static long recv_wait(struct fd_desc *d, struct rx *x, bool nb)
{
    struct pipe_end *p = pipe_end_of(d);
    for (;;) {
        lock_take(&d->lock);
        long r = recv_try(p, x);
        lock_give(&d->lock);
        if (r != -EAGAIN || nb)
            return r;
        r = pipe_wait(pipe_ch(p), SIG_READABLE);
        if (r && !is_shut(p))
            return r;   /* -EINTR; a shutdown meanwhile is seen by the next try */
    }
}

/* MSG_WAITALL on a stream, one buffer: reads until it is full, the end,
 * an error, or a send's descriptors came. */
static long recv_all(struct fd_desc *d, struct rx *x)
{
    size_t done = 0;
    for (;;) {
        struct iovec v = { (uint8_t *)x->iov[0].iov_base + done, x->total - done };
        struct rx y = *x;
        y.iov = &v;
        y.iovcnt = 1;
        y.total = v.iov_len;
        unsigned had = x->sink ? x->sink->got : 0;
        bool cut = x->sink && x->sink->cut;
        long r = recv_wait(d, &y, false);
        if (r <= 0)
            return done ? (long)done : r;
        done += (size_t)r;
        bool fds = x->sink && (x->sink->got != had || x->sink->cut != cut);
        if (done == x->total || fds)
            return (long)done;
    }
}

long sockp_recvmsg(struct fd_desc *d, struct msghdr *m, int flags)
{
    struct pipe_end *p = pipe_end_of(d);
    long total = fd_iov_total(m->msg_iov, (int)(m->msg_iovlen > INT_MAX ? -1 : m->msg_iovlen));
    if (total < 0)
        return total;
    if (!p->seq && (flags & MSG_TRUNC))
        return -EOPNOTSUPP;   /* Linux's "discard the bytes" for a stream: not built */
    int fds[FDPASS_MAX];
    struct fdpass_sink sink = { .fds = fds, .room = fdpass_cmsg_room(m),
                                .cloexec = flags & MSG_CMSG_CLOEXEC };
    struct rx x = { .iov = m->msg_iov, .iovcnt = (int)m->msg_iovlen, .total = (size_t)total,
                    .flags = flags, .sink = &sink };
    bool nb = nonblocking(d, flags);
    long r;
    if (!p->seq && !total)
        r = 0;   /* nothing asked of a stream: nothing taken */
    else if (!p->seq && (flags & MSG_WAITALL) && !(flags & MSG_PEEK) && !nb && x.iovcnt == 1)
        r = recv_all(d, &x);
    else
        r = recv_wait(d, &x, nb);
    if (r < 0)
        return r;
    fdpass_cmsg_put(m, fds, sink.got);
    m->msg_flags = (sink.cut ? MSG_CTRUNC : 0) | (p->seq && x.whole > x.total ? MSG_TRUNC : 0);
    m->msg_namelen = 0;   /* a socketpair's ends have no names */
    return p->seq && (flags & MSG_TRUNC) ? (long)x.whole : r;
}

static long sp_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    struct msghdr m = { .msg_iov = (struct iovec *)iov, .msg_iovlen = iovcnt };
    return sockp_recvmsg(d, &m, 0);
}

/* ---- sending --------------------------------------------------------------------------- */

/* One send's bytes. */
struct tx {
    const struct iovec *iov;
    int                 iovcnt;
    size_t              total;
    bool                nb;      /* don't wait for room */
};

/* -errno for a write's status. */
static long write_err(const struct pipe_end *p, status_t st)
{
    if (st == ERR_PEER_CLOSED || (st == ERR_BAD_HANDLE && is_shut(p)))
        return -EPIPE;
    if (st == ERR_NOT_SUPPORTED)
        return -ETOOMANYREFS;   /* the kernel's cycle rule: an end into its own channel, or
                                 * one with channel ends queued (Linux's in-flight limit) */
    return st == ERR_SHOULD_WAIT ? -EAGAIN : lx_err(st);
}

/* Wait for room for a message, unless nb (then: is there room now?): 0,
 * or -errno (-EAGAIN: none). */
static long room(const struct pipe_end *p, bool nb)
{
    if (is_shut(p))
        return -EPIPE;
    if (!nb) {
        long r = pipe_wait(pipe_ch(p), SIG_WRITABLE);
        return r && is_shut(p) ? -EPIPE : r;
    }
    signals_t seen = 0;
    (void)jam_object_wait_one(pipe_ch(p), SIG_WRITABLE | SIG_PEER_CLOSED, 0, &seen);
    return seen & (SIG_WRITABLE | SIG_PEER_CLOSED) ? 0 : -EAGAIN;   /* the write says EPIPE */
}

/* Bytes [from, from + len) of the send: straight from a single buffer,
 * else gathered into buf. */
static const uint8_t *bytes_at(const struct tx *t, size_t from, size_t len, uint8_t *buf)
{
    if (t->iovcnt == 1)
        return (const uint8_t *)t->iov[0].iov_base + from;
    fd_iov_gather(t->iov, t->iovcnt, from, buf, len);
    return buf;
}

/* A stream's bytes from `from` on, in plain messages of PIPE_SIZE at
 * most: how many went, or -errno when none did. */
static long send_bytes(struct pipe_end *p, const struct tx *t, size_t from)
{
    size_t left = t->total - from;
    uint8_t *gather = NULL;
    if (t->iovcnt > 1 && left && !(gather = malloc(left < PIPE_SIZE ? left : PIPE_SIZE)))
        return -ENOMEM;
    size_t done = 0;
    long r = 0;
    while (!r && done < left) {
        uint32_t len = left - done > PIPE_SIZE ? PIPE_SIZE : (uint32_t)(left - done);
        status_t st = is_shut(p) ? ERR_PEER_CLOSED
                                 : jam_channel_write(pipe_ch(p), bytes_at(t, from + done, len,
                                                                          gather), len, NULL, 0);
        if (st == OK)
            done += len;
        else if (st != ERR_SHOULD_WAIT || t->nb)
            r = write_err(p, st);
        else
            r = room(p, false);   /* the reader is behind */
    }
    free(gather);
    return done ? (long)done : r;
}

/* A seqpacket's send with no descriptors: one message, empty ones too. */
static long send_record(struct pipe_end *p, const struct tx *t)
{
    if (t->total > PIPE_SIZE)
        return -EMSGSIZE;
    uint8_t *gather = t->iovcnt > 1 && t->total ? malloc(t->total) : NULL;
    if (t->iovcnt > 1 && t->total && !gather)
        return -ENOMEM;
    const uint8_t *b = t->total ? bytes_at(t, 0, t->total, gather) : NULL;
    long r;
    for (;;) {
        status_t st = is_shut(p) ? ERR_PEER_CLOSED
                                 : jam_channel_write(pipe_ch(p), b, (uint32_t)t->total, NULL, 0);
        r = st == OK ? (long)t->total : write_err(p, st);
        if (r != -EAGAIN || t->nb || (r = room(p, false)) != 0)
            break;
    }
    free(gather);
    return r;
}

/* Where each part's descriptors start: part k has gs[cut[k] .. cut[k + 1]).
 * The parts' count. */
static unsigned layout(const struct fd_given *gs, unsigned n, uint16_t *cut)
{
    unsigned parts = 0, h = 0;
    uint32_t b = 0;
    cut[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t rb = REC_BYTES + PAD8(gs[i].data_len);
        if (i > cut[parts] && (h + gs[i].nhandles > CHANNEL_MAX_HANDLES || b + rb > SP_REC_MAX)) {
            cut[++parts] = (uint16_t)i;
            h = 0;
            b = 0;
        }
        h += gs[i].nhandles;
        b += rb;
    }
    cut[++parts] = (uint16_t)n;
    return parts;
}

/* Part k's header and records into msg, its handles moved out of gs into
 * hs (*nh of them; an event for none): the bytes it takes, or -errno. */
static long part_build(uint8_t *msg, struct fd_given *gs, const uint16_t *cut, unsigned k,
                       unsigned parts, uint32_t cookie, handle_t *hs, uint32_t *nh)
{
    uint32_t at = SP_HDR;
    *nh = 0;
    for (unsigned i = cut[k]; i < cut[k + 1]; i++) {
        struct fd_given *g = &gs[i];
        const struct posixfds_fd rec = { .fd = (int32_t)i, .kind = g->kind,
                                         .nhandles = (uint16_t)g->nhandles,
                                         .status = g->status & POSIXFDS_STATUS,
                                         .data_len = g->data_len, .offset = g->offset };
        memcpy(msg + at, &rec, REC_BYTES);
        memcpy(msg + at + REC_BYTES, g->data, g->data_len);
        memset(msg + at + REC_BYTES + g->data_len, 0, PAD8(g->data_len) - g->data_len);
        at += REC_BYTES + PAD8(g->data_len);
        memcpy(hs + *nh, g->handles, g->nhandles * sizeof(handle_t));
        *nh += g->nhandles;
        g->nhandles = 0;   /* moved: the write takes them */
    }
    bool marker = !*nh;
    if (marker) {
        status_t st = jam_event_create(&hs[0]);
        if (st != OK)
            return lx_err(st);
        *nh = 1;
    }
    const struct sp_hdr h = { .magic = SP_MAGIC, .cookie = cookie, .part = (uint16_t)k,
                              .parts = (uint16_t)parts, .nfds = (uint16_t)(cut[k + 1] - cut[k]),
                              .marker = marker, .rec_len = at - SP_HDR };
    memcpy(msg, &h, SP_HDR);
    return at;
}

/* The parts of one try, the bytes' first piece in the last: OK, or the
 * write's status (or ERR_NO_MEMORY for a marker). *first: the bytes that
 * went. The records' handles are moved into the writes. */
static status_t parts_write(struct pipe_end *p, const struct tx *t, struct fd_given *gs,
                            const uint16_t *cut, unsigned parts, uint8_t *msg, size_t *first)
{
    uint32_t cookie = __atomic_add_fetch(&cookies, 1, __ATOMIC_RELAXED);
    for (unsigned k = 0; k < parts; k++) {
        handle_t hs[CHANNEL_MAX_HANDLES];
        uint32_t nh;
        long len = part_build(msg, gs, cut, k, parts, cookie, hs, &nh);
        if (len < 0)
            return ERR_NO_MEMORY;
        if (k == parts - 1) {
            size_t room_left = PIPE_SIZE - (size_t)len;
            *first = t->total < room_left ? t->total : room_left;
            fd_iov_gather(t->iov, t->iovcnt, 0, msg + len, *first);
            len += (long)*first;
        }
        status_t st = is_shut(p) ? ERR_PEER_CLOSED
                                 : jam_channel_write(pipe_ch(p), msg, (uint32_t)len, hs, nh);
        if (st != OK) {
            if (is_shut(p))
                close_all(hs, nh);   /* never written: ours to close */
            return st;
        }
    }
    return OK;
}

/* The bytes in the last part, its records' room taken first: may a
 * seqpacket's whole message go? */
static bool record_fits(const struct fd_given *gs, const uint16_t *cut, unsigned parts,
                        size_t total)
{
    uint32_t b = SP_HDR;
    for (unsigned i = cut[parts - 1]; i < cut[parts]; i++)
        b += REC_BYTES + PAD8(gs[i].data_len);
    return total <= PIPE_SIZE - b;
}

/* A send with the n descriptors fds: the bytes that went, or -errno. */
static long send_fds(struct pipe_end *p, const struct tx *t, const int *fds, unsigned n)
{
    struct fd_given *gs = calloc(n, sizeof(*gs));
    uint8_t *msg = malloc(PIPE_SIZE);
    uint16_t *cut = malloc((n + 1) * sizeof(*cut));
    long r = gs && msg && cut ? 0 : -ENOMEM;
    size_t first = 0;
    while (!r) {   /* each round a try: the records made anew, every part written */
        if ((r = room(p, t->nb)) != 0 || (r = fdpass_gives_out(fds, n, &sp_policy, gs)) != 0)
            break;
        unsigned parts = layout(gs, n, cut);
        status_t st = p->seq && !record_fits(gs, cut, parts, t->total) ? ERR_OUT_OF_RANGE
                      : parts_write(p, t, gs, cut, parts, msg, &first);
        fdpass_gives_drop(gs, n);   /* what no write took, and the data */
        if (st == OK)
            break;
        r = st == ERR_OUT_OF_RANGE ? -EMSGSIZE : write_err(p, st);
        if (r == -EAGAIN && !t->nb)
            r = 0;   /* refused for room after the look (another writer): again */
    }
    free(cut);
    free(msg);
    free(gs);
    if (r)
        return r;
    if (p->seq || first == t->total)
        return (long)first;
    long rest = send_bytes(p, t, first);
    return rest > 0 ? (long)first + rest : (long)first;
}

long sockp_sendmsg(struct fd_desc *d, const struct msghdr *m, int flags)
{
    struct pipe_end *p = pipe_end_of(d);
    int fds[FDPASS_MAX];
    long n = fdpass_cmsg_fds(m, fds, FDPASS_MAX);   /* more than 253: EINVAL */
    long total = n < 0 ? n : fd_iov_total(m->msg_iov,
                                            (int)(m->msg_iovlen > INT_MAX ? -1 : m->msg_iovlen));
    if (total < 0)
        return total;
    if (!p->seq && !total)
        return 0;   /* a stream sends nothing, descriptors with it (Linux drops them too) */
    struct tx t = { .iov = m->msg_iov, .iovcnt = (int)m->msg_iovlen, .total = (size_t)total,
                    .nb = nonblocking(d, flags) };
    lock_take(&p->send_lock);
    long r = n ? send_fds(p, &t, fds, (unsigned)n) : p->seq ? send_record(p, &t)
                                                          : send_bytes(p, &t, 0);
    lock_give(&p->send_lock);
    return r;
}

static long sp_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    const struct msghdr m = { .msg_iov = (struct iovec *)iov, .msg_iovlen = iovcnt };
    return sockp_sendmsg(d, &m, 0);
}

/* ---- the rest ------------------------------------------------------------------------- */

long sockp_shutdown(struct fd_desc *d, int how)
{
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (how != SHUT_RDWR)
        return -EOPNOTSUPP;   /* no half-close on a channel */
    struct pipe_end *p = pipe_end_of(d);
    long r = 0;
    lock_take(&d->lock);
    if (!is_shut(p)) {
        handle_t dead, peer;
        status_t st = jam_channel_create(&dead, &peer);
        r = lx_err(st);
        if (st == OK) {
            jam_handle_close(peer);   /* dead's peer is gone: poll says hung up */
            handle_t old = pipe_ch(p);
            __atomic_store_n(&p->ch, dead, __ATOMIC_RELEASE);
            __atomic_store_n(&p->shut, true, __ATOMIC_RELEASE);
            fds_drop(p->fds);
            pipe_set_leftover(p, 0, 0);
            set_kept(p, false);
            jam_handle_close(old);   /* the peer reads the end (if this was the last handle) */
        }
    }
    lock_give(&d->lock);
    return r;
}

int sockp_type(struct fd_desc *d)
{
    return pipe_end_of(d)->seq ? SOCK_SEQPACKET : SOCK_STREAM;
}

static long sp_stat(struct fd_desc *d, struct stat *st)
{
    pipe_stat_as(d, st, S_IFSOCK | 0777);
    return 0;
}

/* FIONREAD: the bytes the next read would get (a seqpacket's next
 * message's), the next message read into the leftover to be counted. */
static long sp_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    struct pipe_end *p = pipe_end_of(d);
    lock_take(&d->lock);
    bool have = p->seq ? is_kept(p) : p->at < p->have;
    long r = have || is_shut(p) ? 0 : fill_locked(p);
    int n = 0;
    if (r >= 0 || r == -EAGAIN) {
        n = (int)(r < 0 || is_shut(p) ? 0 : p->have - p->at);
        r = 0;
    }
    lock_give(&d->lock);
    if (!r)
        r = uc_put_int(arg, n);
    return r;
}

/* A child gets the end as a pipe end's give does (the leftover, or a
 * seqpacket's kept message, moved to it); descriptors waiting here are
 * closed (a hand-over has no place for them). */
static long sp_give(struct fd_desc *d, struct fd_given *out)
{
    struct pipe_end *p = pipe_end_of(d);
    lock_take(&d->lock);
    fds_drop(p->fds);
    long r = pipe_give_end(d, p->seq ? POSIXFDS_SEQPACKET : POSIXFDS_PIPE, out);
    if (!r)
        set_kept(p, false);
    lock_give(&d->lock);
    return r;
}

/* poll: the leftover (a kept message), or a shut end (every call answers
 * at once); the channel's signals for the rest (pipe_arm). */
static uint32_t sp_ready(struct fd_desc *d, uint32_t interest)
{
    const struct pipe_end *p = pipe_end_of(d);
    if (is_shut(p))
        return NETWAIT_READ | NETWAIT_WRITE;
    bool have = p->seq ? is_kept(p)
                       : __atomic_load_n(&p->at, __ATOMIC_RELAXED) <
                             __atomic_load_n(&p->have, __ATOMIC_RELAXED);
    return have ? NETWAIT_READ : 0;
}

static void sp_close(struct fd_desc *d)
{
    struct pipe_end *p = pipe_end_of(d);
    fds_drop(p->fds);
    free(p->fds);
    pipe_end_free(p);
}

const struct fd_ops sockp_ops = {
    .name = "socketpair",
    .read = sp_read,
    .write = sp_write,
    .stat = sp_stat,
    .ioctl = sp_ioctl,
    .ready = sp_ready,
    .arm = pipe_arm,
    .give = sp_give,
    .close = sp_close,
};

struct fd_desc *sockp_desc_new(struct pipe_end *p, uint32_t status, bool seq)
{
    p->seq = seq;
    struct fd_desc *d = fd_new(&sockp_ops, status, p);
    if (!d)
        pipe_end_free(p);
    return d;
}
