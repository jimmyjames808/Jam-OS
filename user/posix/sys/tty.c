/* The terminal descriptor: a kind of the descriptor table (fd.h) over the
 * console's POSIX side (tty.h; docs/M13-PLAN.md, "The terminal layer").
 *
 * A terminal is three handles: the `tty` channel (abi/idl/console.idl:
 * the settings, the size, drain and flush; the size and signal events),
 * the input pipe (what is typed, after the console's line discipline: in
 * canonical mode one line per message, an empty message the end of the
 * file) and the output pipe (what the program writes, in messages of at
 * most TTY_OUT_MAX bytes).
 *
 * read takes the input pipe's messages as a pipe's read does (what doesn't
 * fit waits in the description's buffer), but never past one message in
 * canonical mode, and by MIN and TIME otherwise (<ldisc.h>'s
 * ld_read_deadline: the settings are asked of the console at each read,
 * since another process on the terminal may have changed them). write
 * cuts the bytes into messages. tcgetattr, tcsetattr, tcdrain, tcflush
 * and TIOCGWINSZ are `tty` calls: musl's struct termios is the protocol's
 * fields as they are (Linux's). isatty is TIOCGWINSZ answering; ttyname
 * is TTY_NAME (musl's ttyname_r is overridden: no /proc).
 *
 * Locks: d->lock guards the buffer's bookkeeping, never a wait; two
 * threads reading one terminal at once is the program's race, as with a
 * pipe. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/console.h>
#include <ldisc.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include "tty.h"

#define TTY_BUF LD_REC_MAX   /* the longest message the console sends */

_Static_assert(sizeof(((struct termios *)0)->c_cc) == LD_NCCS, "c_cc is the protocol's cc");
_Static_assert(ICANON == LD_ICANON && ECHO == LD_ECHO && ISIG == LD_ISIG && ICRNL == LD_ICRNL &&
               OPOST == LD_OPOST && ONLCR == LD_ONLCR && VMIN == LD_VMIN && VTIME == LD_VTIME,
               "musl's termios bits are the line discipline's (Linux's)");
_Static_assert(TCSADRAIN - TCSANOW == TTY_WHEN_DRAIN && TCSAFLUSH - TCSANOW == TTY_WHEN_FLUSH &&
               TCIFLUSH == TTY_QUEUE_IN && TCOFLUSH == TTY_QUEUE_OUT &&
               TCIOFLUSH == TTY_QUEUE_BOTH, "the protocol's constants are Linux's");

struct tty {
    handle_t ch, in, out;          /* the tty channel, the input pipe, the output pipe */
    uint32_t have, at;             /* buf[at .. have) read from the input and not taken */
    uint8_t  buf[TTY_BUF];
};

static struct fd_desc *ctty;      /* the process's terminal (a reference); ctty_lock */
static lock_t ctty_lock;

/* ---- reading ------------------------------------------------------------------------ */

/* The next input message into the buffer, waiting until deadline: 1, 0
 * for an empty message (the end of the file), -EAGAIN when none came, or
 * -errno. */
static long fill(struct tty *t, uint64_t deadline, bool nonblock)
{
    for (;;) {
        uint32_t got = 0, nh = 0;
        handle_t hs[CHANNEL_MAX_HANDLES];
        status_t st = drv_channel_read(t->in, t->buf, TTY_BUF, &got, hs, CHANNEL_MAX_HANDLES,
                                       &nh);
        for (uint32_t i = 0; st == OK && i < nh; i++)
            jam_handle_close(hs[i]);   /* bytes only */
        if (st == OK) {
            t->at = 0;
            t->have = got;
            return got ? 1 : 0;
        }
        if (st == ERR_PEER_CLOSED)
            return 0;   /* the terminal is gone: the end of the file */
        if (st != ERR_SHOULD_WAIT)
            return lx_err(st);
        if (nonblock || deadline <= now())
            return -EAGAIN;
        signals_t seen;
        st = jam_object_wait_one(t->in, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return -EAGAIN;
        if (st != OK)
            return lx_err(st);   /* ERR_INTERRUPTED: EINTR */
    }
}

/* The settings, or the defaults if the console doesn't answer (the
 * terminal is gone: the read then finds the end of the file). */
static void settings(const struct tty *t, struct ld_termios *s)
{
    if (tty_get_attr(t->ch, &s->iflag, &s->oflag, &s->cflag, &s->lflag, s->cc, &s->ispeed,
                     &s->ospeed) != OK)
        ld_default(s);
}

static long tty_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    struct tty *t = d->priv;
    long want = fd_iov_total(iov, n);
    if (want <= 0)
        return want;
    struct ld_termios s;
    settings(t, &s);
    bool nonblock = fd_status(d) & O_NONBLOCK;
    uint64_t start = now(), last = start;
    size_t got = 0;
    bool polled = false;   /* MIN 0 and TIME 0: one look at the pipe */
    for (;;) {
        lock_take(&d->lock);
        size_t k = t->have - t->at < (size_t)want - got ? t->have - t->at : (size_t)want - got;
        fd_iov_scatter(iov, n, got, t->buf + t->at, k);
        t->at += (uint32_t)k;
        lock_give(&d->lock);
        got += k;
        uint64_t deadline = ld_read_deadline(&s, got, (size_t)want, start, last);
        if ((!deadline && (got || polled)) || got == (size_t)want)
            return (long)got;
        polled = !deadline;
        long r = fill(t, deadline, nonblock || !deadline);
        if (r == -EAGAIN)   /* none: TIME ran out, or the poll found nothing */
            return got ? (long)got : nonblock ? -EAGAIN : 0;
        if (r <= 0)
            return got ? (long)got : r;   /* the end of the file, or an error */
        last = now();
    }
}

/* ---- writing ------------------------------------------------------------------------ */

static long write_one(struct fd_desc *d, const uint8_t *p, size_t n)
{
    const struct tty *t = d->priv;
    size_t done = 0;
    while (done < n) {
        uint32_t k = n - done > TTY_OUT_MAX ? TTY_OUT_MAX : (uint32_t)(n - done);
        status_t st = jam_channel_write(t->out, p + done, k, NULL, 0);
        if (st == ERR_SHOULD_WAIT) {
            if (fd_status(d) & O_NONBLOCK)
                return done ? (long)done : -EAGAIN;
            signals_t seen;
            st = jam_object_wait_one(t->out, SIG_WRITABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                                     &seen);
            if (st != OK)
                return done ? (long)done : lx_err(st);
            continue;
        }
        if (st != OK)   /* hung up: Linux's EIO for a terminal that is gone */
            return done ? (long)done : st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
        done += k;
    }
    return (long)done;
}

static long tty_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long total = 0;
    for (int i = 0; i < n; i++) {
        long r = write_one(d, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

/* ---- ioctl ----------------------------------------------------------------------------- */

static long get_termios(const struct tty *t, struct termios *tio)
{
    struct termios r;
    memset(&r, 0, sizeof(r));
    status_t st = tty_get_attr(t->ch, &r.c_iflag, &r.c_oflag, &r.c_cflag, &r.c_lflag, r.c_cc,
                               &r.__c_ispeed, &r.__c_ospeed);
    if (st != OK)
        return st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
    *tio = r;
    return 0;
}

static long set_termios(struct fd_desc *d, unsigned when, const struct termios *tio)
{
    struct tty *t = d->priv;
    struct termios s = *tio;   /* read once */
    status_t st = tty_set_attr(t->ch, (uint8_t)when, s.c_iflag, s.c_oflag, s.c_cflag, s.c_lflag,
                               s.c_cc, s.__c_ispeed, s.__c_ospeed);
    if (st == OK && when == TTY_WHEN_FLUSH) {
        lock_take(&d->lock);
        t->at = t->have;   /* what we read and nobody took goes too */
        lock_give(&d->lock);
    }
    return st == OK ? 0 : st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
}

static long flush(struct fd_desc *d, long queue)
{
    struct tty *t = d->priv;
    if (queue < TCIFLUSH || queue > TCIOFLUSH)
        return -EINVAL;
    status_t st = tty_flush(t->ch, (uint8_t)queue);
    if (st == OK && queue != TCOFLUSH) {
        lock_take(&d->lock);
        t->at = t->have;
        lock_give(&d->lock);
    }
    return st == OK ? 0 : st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
}

static long winsize(const struct tty *t, struct winsize *w)
{
    uint16_t c, r;
    status_t st = tty_get_size(t->ch, &c, &r);
    if (st != OK)
        return st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
    *w = (struct winsize){ .ws_row = r, .ws_col = c };
    return 0;
}

/* FIONREAD: what a read would get now without waiting (this message's
 * rest; with none, the next message's, read into the buffer). */
static long readable(struct fd_desc *d, int *out)
{
    struct tty *t = d->priv;
    lock_take(&d->lock);
    if (t->at == t->have)
        (void)fill(t, 0, true);   /* none: 0 */
    *out = (int)(t->have - t->at);
    lock_give(&d->lock);
    return 0;
}

static long tty_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    struct tty *t = d->priv;
    void *p = (void *)(uintptr_t)arg;
    switch (req) {
    case TCGETS:     return get_termios(t, p);
    case TCSETS:
    case TCSETSW:
    case TCSETSF:    return set_termios(d, (unsigned)(req - TCSETS), p);
    case TCSBRK:     /* tcdrain (1), and tcsendbreak (0): a break means nothing here */
        return tty_drain(t->ch) == OK ? 0 : -EIO;
    case TCXONC:     return arg >= TCOOFF && arg <= TCION ? 0 : -EINVAL;   /* no flow control */
    case TCFLSH:     return flush(d, arg);
    case TIOCGWINSZ: return winsize(t, p);
    case FIONREAD:   return readable(d, p);
    }
    /* TIOCSWINSZ: the size is the terminal's (its window, or the remote
     * one's); process groups and the controlling terminal are not built. */
    return req == TIOCSWINSZ ? -EINVAL : -ENOTTY;
}

/* ---- the rest of the kind ------------------------------------------------------------------ */

static long tty_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFCHR | 0620;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_rdev = 5 << 8;   /* Linux's /dev/tty: major 5, minor 0 */
    return 0;
}

static uint32_t tty_ready(struct fd_desc *d, uint32_t interest)
{
    const struct tty *t = d->priv;
    uint32_t r = interest & NETWAIT_WRITE;   /* the console takes output as it comes */
    if (t->at < t->have)
        r |= interest & NETWAIT_READ;
    return r;
}

static long tty_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    const struct tty *t = d->priv;
    *out = (struct fd_wait){ .kind = FD_WAIT_HANDLE };
    out->h = (struct netwait_handle){ .h = t->in, .read = SIG_READABLE, .hup = SIG_PEER_CLOSED };
    return 0;
}

static long tty_give(struct fd_desc *d, struct fd_given *out)
{
    const struct tty *t = d->priv;
    handle_t hs[3] = { t->ch, t->in, t->out };
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < 3; i++) {
        status_t st = jam_handle_duplicate(hs[i], RIGHT_SAME, &out->handles[i]);
        if (st != OK) {
            for (unsigned k = 0; k < i; k++)
                jam_handle_close(out->handles[k]);
            return lx_err(st);
        }
    }
    out->kind = POSIXFDS_TTY;
    out->status = fd_status(d);
    out->nhandles = 3;
    return 0;   /* no data: what we read and didn't take stays ours */
}

static void tty_close(struct fd_desc *d)
{
    struct tty *t = d->priv;
    jam_handle_close(t->ch);
    jam_handle_close(t->in);
    jam_handle_close(t->out);
    free(t);
}

static const struct fd_ops tty_ops = {
    .name = "tty", .read = tty_read, .write = tty_write, .stat = tty_stat, .ioctl = tty_ioctl,
    .ready = tty_ready, .arm = tty_arm, .give = tty_give, .close = tty_close,
};

/* ---- making one ---------------------------------------------------------------------------- */

/* A description over the three handles (consumed). */
static long tty_new(const handle_t hs[3], uint32_t status, struct fd_desc **out)
{
    struct tty *t = calloc(1, sizeof(*t));
    struct fd_desc *d = t ? fd_new(&tty_ops, status, t) : NULL;
    if (!d) {
        free(t);
        for (unsigned i = 0; i < 3; i++)
            jam_handle_close(hs[i]);
        return -ENOMEM;
    }
    t->ch = hs[0];
    t->in = hs[1];
    t->out = hs[2];
    lock_take(&ctty_lock);
    if (!ctty) {
        fd_ref(d);
        ctty = d;
    }
    lock_give(&ctty_lock);
    *out = d;
    return 0;
}

long tty_open(handle_t con, uint32_t status, struct fd_desc **out)
{
    handle_t hs[3];
    status_t st = console_open_tty(con, &hs[0], &hs[1], &hs[2]);
    if (st != OK)
        return lx_err(st);
    return tty_new(hs, status, out);
}

long tty_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
               struct fd_desc **out)
{
    return tty_new(hs, rec->status, out);
}

unsigned tty_stdfds(void)
{
    handle_t con = startup_handle(SR_CONSOLE);
    struct fd_desc *d;
    if (!con)
        return 0;
    long r = tty_open(con, O_RDWR, &d);
    if (r < 0) {
        static const char why[] = "posix: no terminal from the console channel\n";
        (void)jam_debug_write(why, sizeof(why) - 1);   /* the log: nothing else to do */
        return 0;
    }
    bool out_too = startup_handle(SR_STDOUT) == HANDLE_INVALID;
    unsigned set = 1u << 0 | 1u << 2 | (out_too ? 1u << 1 : 0);
    for (int fd = 2; fd >= 0; fd--) {
        if (!(set & 1u << fd))
            continue;
        if (fd)
            fd_ref(d);   /* one reference for each slot: fd 0's is the one we hold */
        (void)fd_replace(fd, d, false);   /* in range and free at the start */
    }
    return set;
}

struct fd_desc *tty_controlling(void)
{
    lock_take(&ctty_lock);
    struct fd_desc *d = ctty;
    if (d)
        fd_ref(d);
    lock_give(&ctty_lock);
    return d;
}

/* ---- the events ------------------------------------------------------------------------------ */

static void on_size(void *ctx, uint16_t cols, uint16_t rows)
{
    *(uint64_t *)ctx |= 1ull << SIGWINCH;
}

static void on_signal(void *ctx, uint8_t sig)
{
    if (sig == TTY_SIG_INT)
        *(uint64_t *)ctx |= 1ull << SIGINT;
    else if (sig == TTY_SIG_QUIT)
        *(uint64_t *)ctx |= 1ull << SIGQUIT;
}

handle_t tty_event_handle(struct fd_desc *d)
{
    return d->ops == &tty_ops ? ((const struct tty *)d->priv)->ch : HANDLE_INVALID;
}

uint64_t tty_take_signals(struct fd_desc *d)
{
    if (d->ops != &tty_ops)
        return 0;
    static const struct tty_events events = { .size = on_size, .signal = on_signal };
    const struct tty *t = d->priv;
    uint64_t sigs = 0;
    for (unsigned guard = 0; guard < 1024; guard++) {   /* a channel holds at most 1024 */
        status_t st = tty_read_event(t->ch, &events, &sigs);
        if (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED)
            break;
    }
    return sigs;
}

/* ttyname_r's override (user/posix/musl/src/unistd/ttyname_r.c). */
int __jam_ttyname(int fd, char *name, unsigned long size)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return EBADF;
    bool tty = d->ops == &tty_ops;
    fd_put(d);
    if (!tty)
        return ENOTTY;
    if (size < sizeof(TTY_NAME))
        return ERANGE;
    memcpy(name, TTY_NAME, sizeof(TTY_NAME));
    return 0;
}
