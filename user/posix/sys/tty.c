/* The terminal descriptor: a kind of the descriptor table (fd.h) over the
 * console's POSIX side (tty.h; docs/M13-PLAN.md, "The terminal layer").
 *
 * A terminal is two handles: the `tty` channel (abi/idl/console.idl: the
 * settings, the size, drain and flush; the size and signal events) and the
 * data channel, a pipe both ways: what is typed comes in on it, after the
 * console's line discipline (in canonical mode one line per message, an
 * empty message the end of the file), and what the program writes goes
 * out on it, in messages of at most TTY_OUT_MAX bytes. Its one handle says
 * readable, writable and hung up: poll's arm.
 *
 * read takes the data channel's messages as a pipe's read does (what
 * doesn't fit waits in the description's buffer), but never past one
 * message in canonical mode, and by MIN and TIME otherwise (<ldisc.h>'s
 * ld_read_deadline: the settings are asked of the console at each read,
 * since another process on the terminal may have changed them). write
 * cuts the bytes into messages. tcgetattr, tcsetattr, tcdrain, tcflush
 * and TIOCGWINSZ are `tty` calls: musl's struct termios is the protocol's
 * fields as they are (Linux's). isatty is TIOCGWINSZ answering; ttyname
 * is the terminal's name (musl's ttyname_r is overridden: no /proc):
 * TTY_NAME, or a pseudo-terminal slave's /dev/pts/N (pty.c), whose
 * TIOCSWINSZ sets its size (a console's own terminal has the size of its
 * window, or of its remote terminal: EINVAL).
 *
 * The process's terminal (its controlling terminal) is the first terminal
 * description it gets, but a slave opened with O_NOCTTY; TIOCSCTTY makes
 * one its terminal if it has none and leads its session. The foreground
 * process group (TIOCGPGRP, TIOCSPGRP: tcgetpgrp, tcsetpgrp) is libc's
 * bookkeeping on the description, as the groups are (proc.c): our own
 * group until set to another group of our session; the console knows no
 * groups (the focus is its foreground).
 *
 * Locks: d->lock guards the buffer's bookkeeping, never a wait; two
 * threads reading one terminal at once is the program's race, as with a
 * pipe. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/console.h>
#include <ldisc.h>
#include <posixfork.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include "proc.h"
#include "pty.h"
#include "sig.h"
#include "tty.h"

#define TTY_BUF     LD_REC_MAX   /* the longest message the console sends */
#define TTY_HANDLES 2            /* the tty channel, the data channel */

_Static_assert(sizeof(((struct termios *)0)->c_cc) == LD_NCCS, "c_cc is the protocol's cc");
_Static_assert(ICANON == LD_ICANON && ECHO == LD_ECHO && ISIG == LD_ISIG && ICRNL == LD_ICRNL &&
               OPOST == LD_OPOST && ONLCR == LD_ONLCR && VMIN == LD_VMIN && VTIME == LD_VTIME,
               "musl's termios bits are the line discipline's (Linux's)");
_Static_assert(TCSADRAIN - TCSANOW == TTY_WHEN_DRAIN && TCSAFLUSH - TCSANOW == TTY_WHEN_FLUSH &&
               TCIFLUSH == TTY_QUEUE_IN && TCOFLUSH == TTY_QUEUE_OUT &&
               TCIOFLUSH == TTY_QUEUE_BOTH, "the protocol's constants are Linux's");

struct tty {
    handle_t ch, data;             /* the tty channel, the data channel */
    uint32_t have, at;             /* buf[at .. have) read from the input and not taken */
    int      pty;                  /* a pseudo-terminal slave's index (pty.c), or -1 */
    uint32_t pty_gen;              /* ... and its generation */
    int32_t  fg;                   /* the foreground process group set (TIOCSPGRP); 0: ours */
    uint64_t koid;                 /* the tty channel's: one terminal's descriptions share it */
    char     name[16];             /* ttyname's: TTY_NAME, or "/dev/pts/N" */
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
        status_t st = drv_channel_read(t->data, t->buf, TTY_BUF, &got, hs, CHANNEL_MAX_HANDLES,
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
        st = jam_object_wait_one(t->data, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
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
        if (fd_iov_scatter(iov, n, got, t->buf + t->at, k)) {
            lock_give(&d->lock);   /* the bytes stay for the next read */
            return got ? (long)got : -EFAULT;
        }
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
        status_t st = jam_channel_write(t->data, p + done, k, NULL, 0);
        if (st == ERR_SHOULD_WAIT) {
            if (fd_status(d) & O_NONBLOCK)
                return done ? (long)done : -EAGAIN;
            signals_t seen;
            st = jam_object_wait_one(t->data, SIG_WRITABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
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
    return uc_put(tio, &r, sizeof(r));
}

static long set_termios(struct fd_desc *d, unsigned when, const struct termios *tio)
{
    struct tty *t = d->priv;
    struct termios s;   /* the caller's, read once */
    if (uc_get(&s, tio, sizeof(s)))
        return -EFAULT;
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
    struct winsize ws = { .ws_row = r, .ws_col = c };
    return uc_put(w, &ws, sizeof(ws));
}

/* FIONREAD: what a read would get now without waiting (this message's
 * rest; with none, the next message's, read into the buffer). */
static long readable(struct fd_desc *d, int *out)
{
    struct tty *t = d->priv;
    lock_take(&d->lock);
    if (t->at == t->have)
        (void)fill(t, 0, true);   /* none: 0 */
    int n = (int)(t->have - t->at);
    lock_give(&d->lock);
    return uc_put_int((long)(uintptr_t)out, n);
}

/* TIOCSWINSZ: a pseudo-terminal slave's size; a console's own terminal
 * has its window's (or its remote terminal's). */
static long set_winsize(const struct tty *t, const struct winsize *w)
{
    struct winsize ws;   /* the caller's, read once */
    if (uc_get(&ws, w, sizeof(ws)))
        return -EFAULT;
    if (t->pty < 0 || !ws.ws_col || !ws.ws_row)
        return -EINVAL;
    return pty_resize(t->pty, t->pty_gen, ws.ws_col, ws.ws_row);
}

/* d is the process's terminal: its description, or another description
 * of the same terminal (a hand-over's 0 to 2 may come as three). */
static bool is_ctty(struct fd_desc *d)
{
    const struct tty *t = d->priv;
    lock_take(&ctty_lock);
    bool yes = ctty && (ctty == d || ((const struct tty *)ctty->priv)->koid == t->koid);
    lock_give(&ctty_lock);
    return yes;
}

/* TIOCSCTTY: d becomes the process's terminal, if it has none and leads
 * its session (Linux's rules; `force` changes nothing: no other session
 * can hold it). */
static long set_ctty(struct fd_desc *d)
{
    if (is_ctty(d))
        return 0;
    if (lx_sid() != lx_pid())
        return -EPERM;
    lock_take(&ctty_lock);
    bool took = !ctty;
    if (took) {
        fd_ref(d);
        ctty = d;
    }
    lock_give(&ctty_lock);
    if (!took)
        return -EPERM;
    sig_tty_attached();   /* the signal thread listens to it from now on */
    return 0;
}

/* The process-group calls: on the process's terminal only (ENOTTY else). */
static long group_ioctl(struct fd_desc *d, unsigned long req, void *p)
{
    struct tty *t = d->priv;
    if (!is_ctty(d))
        return -ENOTTY;
    if (req == TIOCGSID)
        return uc_put_int((long)(uintptr_t)p, lx_sid());
    if (req == TIOCGPGRP) {
        int32_t fg = __atomic_load_n(&t->fg, __ATOMIC_RELAXED);
        return uc_put_int((long)(uintptr_t)p, fg ? fg : lx_pgid());
    }
    pid_t g;   /* TIOCSPGRP: read once */
    if (uc_get(&g, p, sizeof(g)))
        return -EFAULT;
    if (g <= 0)
        return -EINVAL;
    if (!lx_group_exists(g))
        return -EPERM;
    __atomic_store_n(&t->fg, g == lx_pgid() ? 0 : g, __ATOMIC_RELAXED);
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
    case TIOCSWINSZ: return set_winsize(t, p);
    case FIONREAD:   return readable(d, p);
    case TIOCSCTTY:  return set_ctty(d);
    case TIOCGPGRP:
    case TIOCSPGRP:
    case TIOCGSID:   return group_ioctl(d, req, p);
    }
    return -ENOTTY;   /* TIOCNOTTY and the rest: not built */
}

/* ---- the rest of the kind ------------------------------------------------------------------ */

static long tty_stat(struct fd_desc *d, struct stat *st)
{
    const struct tty *t = d->priv;
    st->st_mode = S_IFCHR | 0620;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_uid = st->st_gid = 1000;   /* the one user */
    /* Linux's /dev/tty (major 5, minor 0), or its Unix98 slave N (136, N). */
    st->st_rdev = t->pty < 0 ? 5 << 8 : (136u << 8) | (unsigned)t->pty;
    return 0;
}

static uint32_t tty_ready(struct fd_desc *d, uint32_t interest)
{
    const struct tty *t = d->priv;
    return t->at < t->have ? interest & NETWAIT_READ : 0;   /* bytes read and not taken */
}

static long tty_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    const struct tty *t = d->priv;
    *out = (struct fd_wait){ .kind = FD_WAIT_HANDLE };
    out->h = (struct netwait_handle){ .h = t->data, .read = SIG_READABLE, .write = SIG_WRITABLE,
                                      .hup = SIG_PEER_CLOSED };
    return 0;
}

static long tty_give(struct fd_desc *d, struct fd_given *out)
{
    const struct tty *t = d->priv;
    handle_t hs[TTY_HANDLES] = { t->ch, t->data };
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < TTY_HANDLES; i++) {
        status_t st = jam_handle_duplicate(hs[i], RIGHT_SAME, &out->handles[i]);
        if (st != OK) {
            for (unsigned k = 0; k < i; k++)
                jam_handle_close(out->handles[k]);
            return lx_err(st);
        }
    }
    out->kind = POSIXFDS_TTY;
    out->status = fd_status(d);
    out->nhandles = TTY_HANDLES;
    return 0;   /* no data: what we read and didn't take stays ours */
}

/* fork (docs/FORK-PLAN.md section 1.6): as a spawned child's, duplicates
 * of both channels; what we read and didn't take stays ours. */
static bool tty_fork(struct fd_desc *d, struct fd_fork *f)
{
    struct tty *t = d->priv;
    if (!fd_fork_child(f)) {
        fd_fork_dup(f, 0, t->ch);
        fd_fork_dup(f, 1, t->data);
        return true;
    }
    t->ch = fd_fork_take(f, 0);
    t->data = fd_fork_take(f, 1);
    t->have = t->at = 0;
    if (t->ch && t->data)
        return true;
    if (t->ch)
        jam_handle_close(t->ch);   /* the child's own: the other one didn't come */
    if (t->data)
        jam_handle_close(t->data);
    t->ch = t->data = HANDLE_INVALID;
    return false;
}

void tty_fork_ctty(struct lx_fork_io *io)
{
    if (lx_fork_child(io))
        ctty_lock = (lock_t){ 0 };
    struct fd_desc *d = ctty;   /* the parent's: set once, read as fork's walk was */
    if (!d)
        return;
    bool keep = fd_fork_met(io, d, POSIXFORK_CTTY);
    if (lx_fork_child(io) && keep)
        d->refs++;   /* ctty's own reference */
    else if (lx_fork_child(io))
        ctty = NULL;
}

static void tty_close(struct fd_desc *d)
{
    struct tty *t = d->priv;
    jam_handle_close(t->ch);
    jam_handle_close(t->data);
    free(t);
}

static const struct fd_ops tty_ops = {
    .name = "tty", .read = tty_read, .write = tty_write, .stat = tty_stat, .ioctl = tty_ioctl,
    .ready = tty_ready, .arm = tty_arm, .give = tty_give, .fork = tty_fork, .close = tty_close,
};

/* ---- making one ---------------------------------------------------------------------------- */

/* A description over the two handles (consumed): a pseudo-terminal's
 * slave for pty >= 0. It is the process's terminal if it has none yet and
 * may_ctty. */
static long tty_new(const handle_t hs[TTY_HANDLES], uint32_t status, int pty, uint32_t gen,
                    bool may_ctty, struct fd_desc **out)
{
    struct tty *t = calloc(1, sizeof(*t));
    struct fd_desc *d = t ? fd_new(&tty_ops, status, t) : NULL;
    if (!d) {
        free(t);
        for (unsigned i = 0; i < TTY_HANDLES; i++)
            jam_handle_close(hs[i]);
        return -ENOMEM;
    }
    t->ch = hs[0];
    t->data = hs[1];
    struct handle_info hi;
    t->koid = jam_handle_info(t->ch, &hi) == OK ? hi.koid : (uint64_t)(uintptr_t)t;
    t->pty = pty;
    t->pty_gen = gen;
    if (pty >= 0)
        snprintf(t->name, sizeof(t->name), "/dev/pts/%d", pty);
    else
        memcpy(t->name, TTY_NAME, sizeof(TTY_NAME));
    lock_take(&ctty_lock);
    bool first = !ctty && may_ctty;
    if (first) {
        fd_ref(d);
        ctty = d;
    }
    lock_give(&ctty_lock);
    if (first)
        sig_tty_attached();   /* the signal thread listens to it from now on */
    *out = d;
    return 0;
}

long tty_open(handle_t con, uint32_t status, struct fd_desc **out)
{
    return tty_open_pty(con, status, -1, 0, true, out);
}

long tty_open_pty(handle_t con, uint32_t status, int pty, uint32_t gen, bool may_ctty,
                  struct fd_desc **out)
{
    handle_t hs[TTY_HANDLES];
    status_t st = console_open_tty(con, &hs[0], &hs[1]);
    if (st != OK)
        return lx_err(st);
    return tty_new(hs, status, pty, gen, may_ctty, out);
}

long tty_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
               struct fd_desc **out)
{
    return tty_new(hs, rec->status, -1, 0, true, out);
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
static int ttyname_of(int fd, char *name, unsigned long size);

int __jam_ttyname(int fd, char *name, unsigned long size)
{
    struct sig_thread *q = sig_quiet_begin();   /* the layer's own: no handler runs inside */
    int r = ttyname_of(fd, name, size);
    sig_quiet_end(q);
    return r;
}

static int ttyname_of(int fd, char *name, unsigned long size)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return EBADF;
    if (d->ops != &tty_ops) {
        fd_put(d);
        return ENOTTY;
    }
    const struct tty *t = d->priv;
    size_t n = strlen(t->name) + 1;
    int r = size < n ? ERANGE : 0;
    if (!r)
        memcpy(name, t->name, n);
    fd_put(d);
    return r;
}
