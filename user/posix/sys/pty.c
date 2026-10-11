/* Pseudo-terminals and init's sessions (docs/M13-PLAN.md, "The terminal
 * layer"; "X1 notes"): the master descriptor, /dev/ptmx, /dev/pts/N and
 * jamos_session_open (<jamos/session.h>).
 *
 * A pseudo-terminal is a connection console of the program's own
 * (bin/console "conn" "endidle", <termconn.h>) in a job under the
 * program's, which needs no grant and holds no authority: open of
 * /dev/ptmx (posix_openpt, openpty) starts one and gives its master; the
 * slave is a terminal of it (tty.c), opened as /dev/pts/N through the
 * console's ADMIN channel, which this process keeps. N is the slot in this
 * process's table, so only this process can open it (another process gets
 * a slave as a descriptor it is handed). The console ends when the master
 * is closed (its programs see their terminal hang up: SIGHUP), or by
 * itself once every slave has closed ("endidle"): a read of the master
 * then gives what was left, and EIO, as Linux's master does.
 *
 * A session is init's (/svc/session, abi/idl/session.idl): a Jam OS shell
 * on a connection console of init's. jamos_session_open opens one and
 * gives its master as a descriptor of the same kind, and its shell's
 * process as a child (proc.c's lx_session_add: waitpid reports its end,
 * kill hangs it up). The two kinds differ only in their ioctls: a
 * session's size goes to its session_ctl, a pseudo-terminal's to its
 * console's conn_ctl.
 *
 * The master's bytes are pipe.c's: a socketpair end over the master
 * channel (read, write, poll, O_NONBLOCK), which this kind wraps.
 *
 * Locks: pty_lock guards the table. Calls to a console are made with a
 * duplicate of its channel taken under the lock, never with the lock held. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <idl/conn_ctl.h>
#include <idl/session.h>
#include <jamos/session.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <termconn.h>
#include "kinds.h"
#include "proc.h"
#include "pty.h"
#include "tty.h"

#define PTS_DIR     "/dev/pts"
#define PTMX        "/dev/ptmx"
#define DEF_COLS    80
#define DEF_ROWS    24
#define PTS_MAJOR   136   /* Linux's Unix98 slaves */

struct pty {
    bool     used;
    uint32_t gen;                /* bumped each time the slot is taken */
    handle_t admin;              /* the console's ADMIN channel: the slaves' open_tty */
    handle_t conn;               /* its conn_ctl channel, the client end */
    handle_t job, proc;          /* its job and process */
    uint16_t cols, rows;         /* the size set last */
};

struct master {
    struct fd_desc *bytes;       /* pipe.c's socketpair end over the master channel */
    int             pty;         /* our pseudo-terminal's slot, or -1: a session */
    uint32_t        gen;         /* the slot's generation */
    handle_t        sctl;        /* a session's session_ctl (0: a pseudo-terminal) */
    uint16_t        cols, rows;  /* a session's size set last */
};

static lock_t     pty_lock;
static struct pty ptys[PTY_MAX];   /* pty_lock */

/* ---- the master descriptor --------------------------------------------------------- */

static struct master *master_of(struct fd_desc *d)
{
    return d->priv;
}

/* The end of a master's output is EIO, as on Linux (the terminal has
 * hung up); so is a write once the console has gone. */
static long master_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    struct fd_desc *b = master_of(d)->bytes;
    long r = b->ops->read(b, iov, n, pos);
    return r == 0 && fd_iov_total(iov, n) > 0 ? -EIO : r;
}

static long master_write(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    struct fd_desc *b = master_of(d)->bytes;
    long r = b->ops->write(b, iov, n, pos);
    return r == -EPIPE ? -EIO : r;
}

static long master_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_rdev = makedev(5, 2);   /* Linux's /dev/ptmx */
    return 0;
}

/* The size from a TIOCSWINSZ: to the session's control channel, or the
 * pseudo-terminal's console. */
static long master_resize(struct master *m, const struct winsize *w)
{
    struct winsize ws = *w;   /* read once */
    if (!ws.ws_col || !ws.ws_row)
        return -EINVAL;
    if (m->pty >= 0)
        return pty_resize(m->pty, m->gen, ws.ws_col, ws.ws_row);
    status_t st = session_ctl_resize(m->sctl, ws.ws_col, ws.ws_row);
    if (st != OK)
        return st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
    m->cols = ws.ws_col;
    m->rows = ws.ws_row;
    return 0;
}

static long master_winsize(struct master *m, struct winsize *w)
{
    uint16_t c = m->cols, r = m->rows;
    if (m->pty >= 0) {
        lock_take(&pty_lock);
        c = ptys[m->pty].cols;
        r = ptys[m->pty].rows;
        lock_give(&pty_lock);
    }
    *w = (struct winsize){ .ws_row = r, .ws_col = c };
    return 0;
}

static long master_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    struct master *m = master_of(d);
    void *p = (void *)(uintptr_t)arg;
    switch (req) {
    case TIOCSWINSZ: return master_resize(m, p);
    case TIOCGWINSZ: return master_winsize(m, p);
    case TIOCGPTN:
        if (m->pty < 0)
            return -ENOTTY;
        *(unsigned *)p = (unsigned)m->pty;
        return 0;
    case TIOCSPTLCK:   /* nothing to unlock: only this process can open the slave */
        return m->pty < 0 ? -ENOTTY : 0;
    case FIONREAD:   return m->bytes->ops->ioctl(m->bytes, req, arg);
    }
    return -ENOTTY;
}

static long master_setfl(struct fd_desc *d, uint32_t old, uint32_t new)
{
    return fd_status_set(master_of(d)->bytes, O_NONBLOCK, new & O_NONBLOCK);
}

static uint32_t master_ready(struct fd_desc *d, uint32_t interest)
{
    struct fd_desc *b = master_of(d)->bytes;
    return b->ops->ready(b, interest);
}

static long master_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    struct fd_desc *b = master_of(d)->bytes;
    return b->ops->arm(b, interest, out);
}

/* A child gets the bytes alone (a socketpair end): the ioctls are ours. */
static long master_give(struct fd_desc *d, struct fd_given *out)
{
    struct fd_desc *b = master_of(d)->bytes;
    return b->ops->give(b, out);
}

/* Pseudo-terminal i is over: its console killed, the slot free. */
static void pty_release(int i, uint32_t gen)
{
    lock_take(&pty_lock);
    struct pty p = ptys[i];
    bool ours = p.used && p.gen == gen;
    if (ours)
        ptys[i] = (struct pty){ .gen = p.gen };
    lock_give(&pty_lock);
    if (!ours)
        return;
    if (p.job)
        jam_job_kill(p.job);   /* the console, if the master's close hasn't ended it yet */
    handle_t hs[] = { p.admin, p.conn, p.proc, p.job };
    for (unsigned k = 0; k < sizeof(hs) / sizeof(hs[0]); k++)
        if (hs[k])
            jam_handle_close(hs[k]);
}

/* m and what it holds: its bytes (the master channel: the terminal hangs
 * up), its pseudo-terminal's slot or its session's control channel. */
static void master_free(struct master *m)
{
    if (m->bytes)
        fd_put(m->bytes);
    if (m->pty >= 0)
        pty_release(m->pty, m->gen);
    if (m->sctl)
        jam_handle_close(m->sctl);
    free(m);
}

static void master_close(struct fd_desc *d)
{
    master_free(master_of(d));
}

static const struct fd_ops master_ops = {
    .name = "pty master", .read = master_read, .write = master_write, .stat = master_stat,
    .ioctl = master_ioctl, .setfl = master_setfl, .ready = master_ready, .arm = master_arm,
    .give = master_give, .close = master_close,
};

/* A master descriptor over channel end ch for m (both consumed, its
 * bytes filled in here): the descriptor, or -errno (m is undone then, as
 * a close would). */
static long master_install(handle_t ch, struct master *m, uint32_t status, bool cloexec)
{
    struct posixfds_fd rec = { .status = O_RDWR | (status & O_NONBLOCK) };
    long r = pipe_adopt(&rec, NULL, &ch, &m->bytes);   /* ch goes to it */
    struct fd_desc *d = r ? NULL : fd_new(&master_ops, status, m);
    if (!d) {
        master_free(m);
        return r ? r : -ENOMEM;
    }
    return fd_install(d, 0, cloexec);   /* on failure master_close undoes m */
}

/* ---- /dev/ptmx: a new pseudo-terminal ------------------------------------------- */

/* Its console, with our ends into p. */
static status_t pty_console(struct pty *p, handle_t *master)
{
    handle_t a = HANDLE_INVALID, m = HANDLE_INVALID, c = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &p->job);
    if (st == OK)
        st = jam_channel_create(&p->admin, &a);
    if (st == OK)
        st = jam_channel_create(master, &m);
    if (st == OK)
        st = jam_channel_create(&p->conn, &c);
    if (st != OK) {
        handle_t left[] = { a, m, c };
        for (unsigned k = 0; k < 3; k++)
            if (left[k])
                jam_handle_close(left[k]);
        return st;
    }
    char cols[12], rows[12];
    snprintf(cols, sizeof(cols), "cols=%u", (unsigned)p->cols);
    snprintf(rows, sizeof(rows), "rows=%u", (unsigned)p->rows);
    const char *argv[] = { "console", "conn", "endidle", cols, rows };
    struct spawn_handle x[] = { { SR_USER + 0, a }, { CONN_MASTER_ROLE, m }, { CONN_CTL_ROLE, c } };
    struct spawn_args sa = { .path = "bin/console", .name = "pty-console", .argc = 5,
                             .argv = argv, .job = p->job, .extra = x, .nextra = 3 };
    return spawn(&sa, &p->proc);   /* moves x whatever happens */
}

static long ptmx_open(int flags)
{
    lock_take(&pty_lock);
    int i = 0;
    while (i < PTY_MAX && ptys[i].used)
        i++;
    uint32_t gen = i < PTY_MAX ? ++ptys[i].gen : 0;
    if (i < PTY_MAX)
        ptys[i].used = true;   /* reserved: filled below */
    lock_give(&pty_lock);
    if (i == PTY_MAX)
        return -ENOSPC;
    struct pty p = { .used = true, .gen = gen, .cols = DEF_COLS, .rows = DEF_ROWS };
    handle_t master = HANDLE_INVALID;
    struct master *m = calloc(1, sizeof(*m));
    status_t st = m ? pty_console(&p, &master) : ERR_NO_MEMORY;
    lock_take(&pty_lock);
    ptys[i] = p;   /* all of it, or the parts made: released below if it failed */
    lock_give(&pty_lock);
    if (st != OK) {
        if (master)
            jam_handle_close(master);
        free(m);
        pty_release(i, gen);
        return st == ERR_NO_MEMORY ? -ENOMEM : -EAGAIN;
    }
    *m = (struct master){ .pty = i, .gen = gen };
    return master_install(master, m, (uint32_t)flags & OPEN_STATUS, flags & O_CLOEXEC);
}

/* ---- /dev/pts/N: a slave -------------------------------------------------------- */

/* "/dev/pts/N": N (0 to PTY_MAX - 1), or -1. */
static int pts_index(const char *abs)
{
    const char *n = abs + sizeof(PTS_DIR);
    if (strncmp(abs, PTS_DIR "/", sizeof(PTS_DIR)) || n[0] < '0' || n[0] > '9' || n[1])
        return -1;
    return n[0] - '0' < PTY_MAX ? n[0] - '0' : -1;
}

/* A duplicate of slot i's ADMIN channel and its generation: false if it
 * isn't ours. */
static bool slot_admin(int i, handle_t *admin, uint32_t *gen)
{
    lock_take(&pty_lock);
    bool ok = i >= 0 && ptys[i].used && ptys[i].admin &&
              jam_handle_duplicate(ptys[i].admin, RIGHT_SAME, admin) == OK;
    *gen = ok ? ptys[i].gen : 0;
    lock_give(&pty_lock);
    return ok;
}

static long pts_open(int i, int flags)
{
    handle_t admin;
    uint32_t gen;
    if (!slot_admin(i, &admin, &gen))
        return -ENOENT;
    if (flags & O_DIRECTORY) {
        jam_handle_close(admin);
        return -ENOTDIR;
    }
    struct fd_desc *d;
    long r = tty_open_pty(admin, (uint32_t)flags & OPEN_STATUS, i, gen, !(flags & O_NOCTTY), &d);
    jam_handle_close(admin);
    if (r)
        return r == -EPIPE ? -EIO : r;   /* its console has ended */
    return fd_install(d, 0, flags & O_CLOEXEC);
}

long pty_resize(int index, uint32_t gen, uint16_t cols, uint16_t rows)
{
    handle_t conn = HANDLE_INVALID;
    lock_take(&pty_lock);
    bool ours = index >= 0 && index < PTY_MAX && ptys[index].used && ptys[index].gen == gen &&
                jam_handle_duplicate(ptys[index].conn, RIGHT_SAME, &conn) == OK;
    lock_give(&pty_lock);
    if (!ours)
        return -EIO;
    status_t st = conn_ctl_resize(conn, cols, rows);
    jam_handle_close(conn);
    if (st != OK)
        return st == ERR_PEER_CLOSED ? -EIO : lx_err(st);
    lock_take(&pty_lock);
    if (ptys[index].used && ptys[index].gen == gen) {
        ptys[index].cols = cols < CONN_COLS_MAX ? cols : CONN_COLS_MAX;
        ptys[index].rows = rows < CONN_ROWS_MAX ? rows : CONN_ROWS_MAX;
    }
    lock_give(&pty_lock);
    return 0;
}

/* ---- the paths -------------------------------------------------------------------- */

bool pty_path(const char *abs)
{
    return !strcmp(abs, PTMX) || !strcmp(abs, PTS_DIR) || !strncmp(abs, PTS_DIR "/", sizeof(PTS_DIR));
}

long pty_open(const char *abs, int flags)
{
    if (!strcmp(abs, PTMX))
        return (flags & O_DIRECTORY) ? -ENOTDIR : ptmx_open(flags);
    if (!strcmp(abs, PTS_DIR))
        return (flags & O_ACCMODE) != O_RDONLY ? -EISDIR : -ENOTSUP;   /* no listing */
    return pts_open(pts_index(abs), flags);
}

long pty_stat(const char *abs, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_blksize = 4096;
    if (!strcmp(abs, PTMX)) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = makedev(5, 2);
        return 0;
    }
    if (!strcmp(abs, PTS_DIR)) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }
    int i = pts_index(abs);
    lock_take(&pty_lock);
    bool ours = i >= 0 && ptys[i].used;
    lock_give(&pty_lock);
    if (!ours)
        return -ENOENT;
    st->st_mode = S_IFCHR | 0620;
    st->st_uid = st->st_gid = 1000;   /* the one user */
    st->st_rdev = makedev(PTS_MAJOR, (unsigned)i);
    return 0;
}

/* ---- init's sessions -------------------------------------------------------------- */

/* session.open on a channel of our own to /svc/session. */
static status_t session_start(const struct winsize *ws, const char *term, const char *cmd,
                              handle_t out[3])
{
    handle_t ch;
    status_t st = svc_open(SVC_SESSION, &ch);
    if (st != OK)
        return st;
    st = session_open(ch, ws ? ws->ws_col : DEF_COLS, ws ? ws->ws_row : DEF_ROWS, term,
                      (uint32_t)strlen(term), cmd, (uint32_t)strlen(cmd), &out[0], &out[1],
                      &out[2]);
    jam_handle_close(ch);
    return st;
}

/* The session's three handles (consumed) as a master descriptor and a
 * child: the descriptor, or -errno (the session hung up). */
static long session_adopt(handle_t hs[3], const struct winsize *ws, pid_t *pid)
{
    struct process_info info;
    handle_t ctl2 = HANDLE_INVALID;
    struct master *m = calloc(1, sizeof(*m));
    status_t st = m ? jam_process_get_info(hs[2], &info) : ERR_NO_MEMORY;
    if (st == OK)
        st = jam_handle_duplicate(hs[1], RIGHT_SAME, &ctl2);
    if (st != OK) {
        (void)session_ctl_hangup(hs[1]);
        for (unsigned k = 0; k < 3; k++)
            jam_handle_close(hs[k]);
        free(m);
        return lx_err(st);
    }
    long r = lx_session_add(hs[2], ctl2, (int32_t)info.pid);   /* moves both */
    if (r) {
        jam_handle_close(hs[0]);   /* lx_session_add hung it up */
        jam_handle_close(hs[1]);
        free(m);
        return r;
    }
    *m = (struct master){ .pty = -1, .sctl = hs[1], .cols = ws ? ws->ws_col : DEF_COLS,
                          .rows = ws ? ws->ws_row : DEF_ROWS };
    *pid = (pid_t)info.pid;
    /* The master never goes to a child unasked: its close is the hang-up. */
    return master_install(hs[0], m, O_RDWR, true);
}

int jamos_session_open(const struct winsize *ws, const char *term, const char *command,
                       int *master, pid_t *pid)
{
    handle_t hs[3];
    status_t st = session_start(ws, term ? term : "", command ? command : "", hs);
    long r = st == ERR_OUT_OF_RANGE ? -E2BIG : lx_err(st);
    if (st == OK) {
        pid_t p = 0;
        r = session_adopt(hs, ws, &p);
        if (r >= 0) {
            *master = (int)r;
            *pid = p;
            return 0;
        }
    }
    errno = (int)-r;
    return -1;
}
