/* Socketpair ends (docs/CR-PLAN.md section 4): AF_UNIX SOCK_STREAM and
 * SOCK_SEQPACKET pairs on a channel (pipe.c makes them; pipe.h has the
 * description's state), carrying descriptors (SCM_RIGHTS) as Mojo's
 * channels need: sendmsg and recvmsg with MSG_DONTWAIT, MSG_PEEK,
 * MSG_WAITALL, MSG_CTRUNC, MSG_CMSG_CLOEXEC, and shutdown.
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
 * gone (pipe.h's ch), so poll says hung up. Nothing in the kernel wakes a
 * wait on an object whose handle closed, so the threads already asleep on
 * the end are woken here, as Linux's shutdown wakes them: one in a read or
 * a write of the description is listed while it sleeps (sp_sleep) and
 * interrupted (thread_interrupt), and then reads the end or gets EPIPE;
 * one in poll, select or epoll_wait is woken by ready_wake_all and looks
 * again. A SHUT_RD and a SHUT_WR, in either order, are a SHUT_RDWR.
 *
 * **Half-close** (pipe.h's half): SHUT_WR ends the description's writes
 * (EPIPE, so SIGPIPE unless MSG_NOSIGNAL) and queues the **end part**, a
 * header of SP_END and nothing else, with an event for its handle as a
 * marker's (a message with handles is always a part): the peer reads what
 * came before it, then the end for good, and poll says readable. The part
 * needs room: on a full channel it goes with the end's next read or send
 * (else the peer reads the end when the end closes). SHUT_RD ends the
 * description's reads once nothing is queued: what came before still
 * reads, as Linux's, then a read returns the end instead of waiting, and
 * poll says readable. Its peer half is not built: Linux's peer gets EPIPE
 * on its writes after a SHUT_RD; here they are queued, and this end reads
 * them (telling the peer would take a signal the kernel's channels don't
 * have). Threads asleep in a read (SHUT_RD) or a write (SHUT_WR) of the
 * description are woken as SHUT_RDWR's are. A child given the end
 * (posix_spawn, SCM_RIGHTS) starts with neither half shut, and reads on
 * past a peer's end part this end had read; a forked child's copy keeps
 * all of it.
 *
 * **SO_PASSCRED**: the kernel stamps every message with its writer's pid
 * (channel_read's CHANNEL_READ_PID), so each recvmsg that takes a
 * message's bytes (a seqpacket's empty one too; not the end) gets
 * SCM_CREDENTIALS before any SCM_RIGHTS, as Linux orders them: the pid of
 * the process that wrote those bytes, and the uid and gid getuid and
 * getgid say (Jam OS has one user). MSG_WAITALL stops where the writer
 * changes, as Linux's does. A sender's own SCM_CREDENTIALS (its pid, uid
 * and gid) is taken and changes nothing, any other EPERM (fdpass.c).
 *
 * **FIONREAD** counts the leftover, else the next message without taking
 * it off the channel (a read with no room leaves it queued): Linux's
 * FIONREAD frees no room in the socket's buffer, so a writer that filled
 * the channel to EAGAIN still gets EAGAIN after the reader's FIONREAD
 * (base's SyncSocket counts on it). A message with descriptors is read
 * into the leftover, as a read would, and counted there.
 *
 * **Locks**: the description's (fd.h) for the leftover, sp_fds and the
 * sleepers, never across a wait; send_lock above it (taken first), across
 * waits. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "calls.h"
#include "fdpass.h"
#include "pipe.h"
#include "ready.h"
#include "threads.h"

#define SP_MAGIC     0x4d43534au     /* "JSCM": a part of a send with descriptors */
#define SP_END       0x4543534au     /* "JSCE": the end part (the peer's SHUT_WR) */
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

/* The end part: this header alone, with one handle (an event). */
static const struct sp_hdr end_hdr = { .magic = SP_END };

/* pipe.h's half: what a half-close did to the end. */
enum {
    HALF_RD   = 1u << 0,   /* shutdown(SHUT_RD) here */
    HALF_WR   = 1u << 1,   /* shutdown(SHUT_WR) here */
    HALF_END  = 1u << 2,   /* the peer's end part was read: reads end */
    HALF_OWED = 1u << 3,   /* SHUT_WR's end part not sent yet (the channel was full) */
};

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

static unsigned halves(const struct pipe_end *p)
{
    return __atomic_load_n(&p->half, __ATOMIC_ACQUIRE);
}

/* Reads end once nothing is left over (SHUT_RD: nothing queued either). */
static bool reads_end(const struct pipe_end *p)
{
    return is_shut(p) || (halves(p) & (HALF_RD | HALF_END));
}

static bool writes_end(const struct pipe_end *p)
{
    return is_shut(p) || (halves(p) & HALF_WR);
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

/* ---- sleeping, and shutdown's wake ------------------------------------------------------- */

/* A thread asleep in a read or a write of the end, on its own stack,
 * listed in the end's sleepers while it sleeps. The description's lock. */
struct sp_sleeper {
    struct sp_sleeper *next;
    uint32_t           tid;
    signals_t          sig;      /* SIG_READABLE: a read; SIG_WRITABLE: a write */
    bool               kicked;   /* a shutdown interrupted it */
};

static status_t interrupt_op(handle_t thread, void *arg)
{
    (void)arg;
    return jam_thread_interrupt(thread);
}

/* A shutdown's interrupt that came after the sleeper's wait had ended
 * stays latched for its next wait (the kernel's rule): take it now with a
 * short wait for SIG_SIGNALED on the end's channel, which a channel never
 * asserts, so it ends no wait of the program's. */
static void interrupt_eat(const struct pipe_end *p)
{
    signals_t seen;
    (void)jam_object_wait_one(pipe_ch(p), SIG_SIGNALED, now() + 10 * NS_PER_MS, &seen);
}

/* Wait for sig on d's end, as pipe_wait, listed so that a shutdown by
 * another thread wakes the wait (SHUT_RDWR closes the channel it waits on,
 * and nothing else would; a half-close changes nothing the channel shows):
 * 0, or -errno (-EINTR: a signal, or the shutdown; reads_end or writes_end
 * tells which). An end whose reads (sig SIG_READABLE) or writes end isn't
 * waited on: 0. */
static long sp_sleep(struct fd_desc *d, signals_t sig)
{
    struct pipe_end *p = pipe_end_of(d);
    struct sp_sleeper me = { .tid = (uint32_t)lx_gettid(0, 0, 0, 0, 0, 0), .sig = sig };
    lock_take(&d->lock);
    bool shut = sig == SIG_READABLE ? reads_end(p) : writes_end(p);
    handle_t ch = pipe_ch(p);
    if (!shut && me.tid) {   /* a thread whose id isn't known sleeps as before */
        me.next = p->sleepers;
        p->sleepers = &me;
    }
    lock_give(&d->lock);
    if (shut)
        return 0;   /* the caller's next try sees it */
    long r = pipe_wait(ch, sig);
    if (!me.tid)
        return r;
    lock_take(&d->lock);
    for (struct sp_sleeper **s = &p->sleepers; *s; s = &(*s)->next) {
        if (*s == &me) {
            *s = me.next;
            break;
        }
    }
    bool kicked = me.kicked;
    lock_give(&d->lock);
    if (kicked && r != -EINTR)
        interrupt_eat(p);
    return r;
}

/* The sleepers in sig's waits (0: all), which a shutdown just ended:
 * interrupt each (sp_sleep), once. The description's lock (thread_apply's
 * lock is a leaf under it). */
static void sleepers_wake_locked(struct pipe_end *p, signals_t sig)
{
    for (struct sp_sleeper *s = p->sleepers; s; s = s->next)
        if ((!sig || s->sig == sig) && !s->kicked)
            s->kicked = thread_apply(s->tid, interrupt_op, NULL) == 0;
}

/* SHUT_WR's end part, if still owed: a full channel keeps it owed for the
 * end's next read or send. No lock: the owed bit is taken atomically. */
static void end_send(struct pipe_end *p)
{
    if (!(halves(p) & HALF_OWED) || is_shut(p) ||
        !(__atomic_fetch_and(&p->half, (uint8_t)~HALF_OWED, __ATOMIC_ACQ_REL) & HALF_OWED))
        return;
    handle_t ev;
    status_t st = jam_event_create(&ev);
    if (st == OK)   /* ev goes whatever the status */
        st = jam_channel_write(pipe_ch(p), &end_hdr, SP_HDR, &ev, 1);
    if (st != OK && st != ERR_PEER_CLOSED && !is_shut(p))
        __atomic_fetch_or(&p->half, (uint8_t)HALF_OWED, __ATOMIC_ACQ_REL);
}

static bool is_passcred(const struct pipe_end *p)
{
    return __atomic_load_n(&p->passcred, __ATOMIC_RELAXED);
}

void sockp_set_passcred(struct fd_desc *d, bool on)
{
    __atomic_store_n(&pipe_end_of(d)->passcred, on, __ATOMIC_RELAXED);
}

bool sockp_passcred(struct fd_desc *d)
{
    return is_passcred(pipe_end_of(d));
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
enum part { PART_RAW, PART_MORE, PART_LAST, PART_DROP, PART_END };

/* p->buf holds a message of got bytes that came with nh handles hs: a
 * part kept (PART_MORE), a send's last (PART_LAST: its bytes from *skip;
 * its descriptors ready, unless they were lost), one dropped
 * (PART_DROP), the peer's end part (PART_END), or no part at all
 * (PART_RAW: plain bytes). The handles are kept or closed. */
static enum part take_part(struct pipe_end *p, uint32_t got, handle_t *hs, uint32_t nh,
                           uint32_t *skip)
{
    if (got == SP_HDR && nh == 1 && !memcmp(p->buf, &end_hdr, SP_HDR)) {
        close_all(hs, nh);
        return PART_END;
    }
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
 * seqpacket's: any message, empty ones too): 1; 0 at the end (the peer's
 * end part too, for good); -EAGAIN; -errno. The description's lock held. */
static long fill_locked(struct pipe_end *p)
{
    if (halves(p) & HALF_END)
        return 0;
    if (!pipe_buf(p))
        return -ENOMEM;
    for (;;) {
        handle_t hs[CHANNEL_MAX_HANDLES];
        uint32_t got, nh, pid, skip = 0;
        status_t st = pipe_read_msg(pipe_ch(p), p->buf, PIPE_SIZE, &got, hs, &nh, &pid);
        if (st == ERR_PEER_CLOSED || (st == ERR_BAD_HANDLE && is_shut(p)))
            return 0;   /* parts of a send that never finished stay until the close */
        if (st != OK)
            return st == ERR_SHOULD_WAIT ? -EAGAIN : lx_err(st);
        enum part k = nh ? take_part(p, got, hs, nh, &skip) : PART_RAW;
        if (k == PART_END) {
            if (p->fds && p->fds->parts)
                fds_drop(p->fds);   /* a send the end cut */
            __atomic_fetch_or(&p->half, (uint8_t)HALF_END, __ATOMIC_ACQ_REL);
            return 0;
        }
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
        p->pid = pid;   /* a send's last part's writer: its bytes' */
        set_kept(p, p->seq);
        return 1;
    }
}

/* FIONREAD with nothing kept: the next message's bytes, read without
 * taking it (no room: ERR_BUFFER_TOO_SMALL leaves it queued), so the
 * channel's room stays the writer's. A message with handles (a send's
 * part) is read into the leftover as a read would (fill_locked) and
 * counted there; an empty one is taken as fill_locked takes it (a
 * stream's carries nothing, a seqpacket's is kept). 0 with nothing
 * queued or at the end, or -errno. The description's lock. */
static long next_bytes_locked(struct pipe_end *p)
{
    if (halves(p) & HALF_END)
        return 0;   /* nothing after the peer's end part is read */
    if (p->seq && !pipe_buf(p))
        return -ENOMEM;   /* an empty message would be kept there */
    for (;;) {
        uint32_t sizes[3] = { 0 };   /* the bytes, the handles, the writer */
        struct channel_read_args a = {
            .h = pipe_ch(p), .rd_flags = CHANNEL_READ_PID,
            .rd_actual = (uint64_t)(uintptr_t)&sizes[0],
            .rd_hactual = (uint64_t)(uintptr_t)&sizes[1],
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL && !sizes[1])
            return sizes[0];   /* plain bytes, still queued */
        if (st == ERR_BUFFER_TOO_SMALL) {
            long r = fill_locked(p);
            return r > 0 ? (long)(p->have - p->at) : r == -EAGAIN ? 0 : r;
        }
        if (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED || (st == ERR_BAD_HANDLE && is_shut(p)))
            return 0;
        if (st != OK)
            return lx_err(st);
        if (p->fds && p->fds->parts)
            fds_drop(p->fds);   /* plain bytes after an unfinished send */
        if (!p->seq)
            continue;
        pipe_set_leftover(p, 0, 0);
        p->pid = sizes[2];
        set_kept(p, true);
        return 0;
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
    bool                took;    /* out: a message's bytes came (SCM_CREDENTIALS) */
    bool                same;    /* only bytes `pid` wrote (MSG_WAITALL with SO_PASSCRED) */
    uint32_t            pid;     /* out: their writer; in, with same */
};

#define OTHER_WRITER (-EXDEV)   /* recv_try's "the next bytes are another writer's" */

/* One try, the description's lock held: bytes delivered, 0 at the end,
 * -EAGAIN, OTHER_WRITER, or -errno. */
static long recv_try(struct pipe_end *p, struct rx *x)
{
    if (is_shut(p))
        return 0;
    bool have = p->seq ? is_kept(p) : p->at < p->have;
    long r = have ? 1 : fill_locked(p);
    if (r == -EAGAIN && (halves(p) & HALF_RD))
        return 0;   /* SHUT_RD: the end once nothing is queued */
    if (r <= 0)
        return r;
    if (x->same && p->pid != x->pid)
        return OTHER_WRITER;   /* they stay for the next read */
    uint32_t left = p->have - p->at;
    size_t n = left < x->total ? left : x->total;
    if (fd_iov_scatter(x->iov, x->iovcnt, 0, p->buf + p->at, n))
        return -EFAULT;   /* the message stays for the next read */
    x->whole = left;
    x->took = true;
    x->pid = p->pid;
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
    end_send(p);   /* SHUT_WR's end part, if the channel had no room for it */
    for (;;) {
        lock_take(&d->lock);
        long r = recv_try(p, x);
        lock_give(&d->lock);
        if (r != -EAGAIN || nb)
            return r;
        r = sp_sleep(d, SIG_READABLE);
        if (r && !reads_end(p))
            return r;   /* -EINTR; a shutdown (or one meanwhile) is seen by the next try */
    }
}

/* MSG_WAITALL on a stream, one buffer: reads until it is full, the end,
 * an error, a send's descriptors came, or (SO_PASSCRED) the next bytes
 * are another writer's. */
static long recv_all(struct fd_desc *d, struct rx *x)
{
    size_t done = 0;
    bool cred = is_passcred(pipe_end_of(d));
    for (;;) {
        struct iovec v = { (uint8_t *)x->iov[0].iov_base + done, x->total - done };
        struct rx y = *x;
        y.iov = &v;
        y.iovcnt = 1;
        y.total = v.iov_len;
        y.same = cred && done;
        unsigned had = x->sink ? x->sink->got : 0;
        bool cut = x->sink && x->sink->cut;
        long r = recv_wait(d, &y, false);
        if (r == OTHER_WRITER)
            return (long)done;
        if (r <= 0)
            return done ? (long)done : r;
        x->took = true;
        x->pid = y.pid;
        done += (size_t)r;
        bool fds = x->sink && (x->sink->got != had || x->sink->cut != cut);
        if (done == x->total || fds)
            return (long)done;
    }
}

/* SCM_CREDENTIALS of the writer pid into ctl (room bytes: CMSG_SPACE of
 * a ucred at most): false when it doesn't fit, and nothing is written
 * (Linux's put_cmsg writes what fits of it; MSG_CTRUNC both). uid and gid
 * are what getuid and getgid say: Jam OS has one user. */
static bool creds_put(void *ctl, size_t room, uint32_t pid)
{
    if (!ctl || room < CMSG_LEN(sizeof(struct ucred)))
        return false;
    struct cmsghdr *c = ctl;
    c->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_CREDENTIALS;
    const struct ucred u = { .pid = (pid_t)pid, .uid = (uid_t)lx_getuid(0, 0, 0, 0, 0, 0),
                             .gid = (gid_t)lx_getgid(0, 0, 0, 0, 0, 0) };
    memcpy(CMSG_DATA(c), &u, sizeof(u));
    return true;
}

long sockp_recvmsg(struct fd_desc *d, struct msghdr *m, int flags)
{
    struct pipe_end *p = pipe_end_of(d);
    long total = fd_iov_total(m->msg_iov, (int)(m->msg_iovlen > INT_MAX ? -1 : m->msg_iovlen));
    if (total < 0)
        return total;
    if (!p->seq && (flags & MSG_TRUNC))
        return -EOPNOTSUPP;   /* Linux's "discard the bytes" for a stream: not built */
    /* SO_PASSCRED: the credentials' block first, the descriptors' after it. */
    bool cred = is_passcred(p);
    size_t room = m->msg_control ? m->msg_controllen : 0;
    size_t cs = !cred ? 0 : room < CMSG_SPACE(sizeof(struct ucred)) ? room
                                                                       : CMSG_SPACE(sizeof(struct ucred));
    struct msghdr rights = *m;   /* the room after the credentials */
    rights.msg_control = cs < room ? (uint8_t *)m->msg_control + cs : NULL;
    rights.msg_controllen = room - cs;
    int fds[FDPASS_MAX];
    struct fdpass_sink sink = { .fds = fds, .room = fdpass_cmsg_room(&rights),
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
    bool cut = sink.cut;
    if (cred && x.took) {
        bool fit = creds_put(m->msg_control, cs, x.pid);   /* else no room was left either */
        cut |= !fit;
        fdpass_cmsg_put(&rights, fds, sink.got);
        m->msg_controllen = (fit ? cs : 0) + rights.msg_controllen;
    } else {
        fdpass_cmsg_put(m, fds, sink.got);   /* nothing taken: none came */
    }
    m->msg_flags = (cut ? MSG_CTRUNC : 0) | (p->seq && x.whole > x.total ? MSG_TRUNC : 0);
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
    struct fd_desc     *d;       /* the end's description */
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

/* Wait for room for a message on t's end, unless nb (then: is there room
 * now?): 0, or -errno (-EAGAIN: none; -EPIPE: shut, before or meanwhile). */
static long room(const struct tx *t, bool nb)
{
    const struct pipe_end *p = pipe_end_of(t->d);
    if (writes_end(p))
        return -EPIPE;
    if (!nb) {
        long r = sp_sleep(t->d, SIG_WRITABLE);
        return writes_end(p) ? -EPIPE : r;
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
    if (fd_iov_gather(t->iov, t->iovcnt, from, buf, len))
        memset(buf, 0, len);   /* unmapped since the send's check (fd.h): never stale bytes */
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
        status_t st = writes_end(p) ? ERR_PEER_CLOSED
                                    : jam_channel_write(pipe_ch(p), bytes_at(t, from + done, len,
                                                                             gather), len, NULL, 0);
        if (st == OK)
            done += len;
        else if (st != ERR_SHOULD_WAIT || t->nb)
            r = write_err(p, st);
        else
            r = room(t, false);   /* the reader is behind */
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
        status_t st = writes_end(p) ? ERR_PEER_CLOSED
                                    : jam_channel_write(pipe_ch(p), b, (uint32_t)t->total, NULL, 0);
        r = st == OK ? (long)t->total : write_err(p, st);
        if (r != -EAGAIN || t->nb || (r = room(t, false)) != 0)
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
            if (fd_iov_gather(t->iov, t->iovcnt, 0, msg + len, *first))
                memset(msg + len, 0, *first);   /* as bytes_at: only after the check */
            len += (long)*first;
        }
        bool ended = writes_end(p);
        status_t st = ended ? ERR_PEER_CLOSED
                            : jam_channel_write(pipe_ch(p), msg, (uint32_t)len, hs, nh);
        if (st != OK) {
            if (ended)
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
        if ((r = room(t, t->nb)) != 0 || (r = fdpass_gives_out(fds, n, &sp_policy, gs)) != 0)
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
    if (!p->seq && !total && !writes_end(p))
        return 0;   /* a stream sends nothing, descriptors with it (Linux drops them too) */
    struct tx t = { .d = d, .iov = m->msg_iov, .iovcnt = (int)m->msg_iovlen,
                    .total = (size_t)total, .nb = nonblocking(d, flags) };
    end_send(p);   /* SHUT_WR's end part, if the channel had no room for it */
    lock_take(&p->send_lock);
    long r = writes_end(p) ? -EPIPE
             : n           ? send_fds(p, &t, fds, (unsigned)n)
             : p->seq      ? send_record(p, &t)
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
    struct pipe_end *p = pipe_end_of(d);
    unsigned want = how == SHUT_RD ? HALF_RD : how == SHUT_WR ? HALF_WR : HALF_RD | HALF_WR;
    long r = 0;
    bool done = false;
    lock_take(&d->lock);
    unsigned had = halves(p);
    if (is_shut(p) || !(want & ~had)) {
        /* done before */
    } else if (((had | want) & (HALF_RD | HALF_WR)) != (HALF_RD | HALF_WR)) {   /* one half */
        __atomic_fetch_or(&p->half, (uint8_t)(want == HALF_WR ? HALF_WR | HALF_OWED : want),
                          __ATOMIC_ACQ_REL);
        sleepers_wake_locked(p, want == HALF_WR ? SIG_WRITABLE : SIG_READABLE);
        done = true;
    } else {   /* both halves: the channel end goes */
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
            sleepers_wake_locked(p, 0);
            done = true;
        }
    }
    lock_give(&d->lock);
    end_send(p);   /* SHUT_WR's end part, after any send the sleepers' wake ended */
    if (done)
        ready_wake_all();   /* a poll or epoll_wait asleep on the end looks again */
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
 * message's): the leftover's, else the next message's, left on the
 * channel (next_bytes_locked). */
static long sp_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    struct pipe_end *p = pipe_end_of(d);
    lock_take(&d->lock);
    bool have = p->seq ? is_kept(p) : p->at < p->have;
    long r = is_shut(p) ? 0 : have ? (long)(p->have - p->at) : next_bytes_locked(p);
    int n = 0;
    if (r >= 0) {
        n = (int)r;
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

/* fork (docs/FORK-PLAN.md section 1.6): the end as a pipe's (both sides
 * on the one channel); in the child the kept message and the descriptors
 * that came with the leftover are dropped with it, their handles
 * forgotten (the parent's), and the send lock fresh. */
static bool sp_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct pipe_end *p = pipe_end_of(d);
    if (fd_fork_child(f)) {
        p->send_lock = (lock_t){ 0 };
        set_kept(p, false);
        if (p->fds) {   /* as fds_drop leaves it, with nothing closed */
            p->fds->n = 0;
            p->fds->next = p->fds->parts = 0;
            p->fds->ready = p->fds->lost = false;
        }
    }
    return pipe_end_fork(p, f);
}

/* poll: the leftover (a kept message), or a shut end (every call answers
 * at once; a half-close's half too: reads that end, writes that fail);
 * the channel's signals for the rest (pipe_arm). */
static uint32_t sp_ready(struct fd_desc *d, uint32_t interest)
{
    const struct pipe_end *p = pipe_end_of(d);
    if (is_shut(p))
        return NETWAIT_READ | NETWAIT_WRITE;
    unsigned h = halves(p);
    bool have = p->seq ? is_kept(p)
                       : __atomic_load_n(&p->at, __ATOMIC_RELAXED) <
                             __atomic_load_n(&p->have, __ATOMIC_RELAXED);
    return (have || (h & (HALF_RD | HALF_END)) ? NETWAIT_READ : 0) |
           (h & HALF_WR ? NETWAIT_WRITE : 0);
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
    .fork = sp_fork,
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
