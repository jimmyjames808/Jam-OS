/* inotify (docs/INOTIFY-PLAN.md section 3): a descriptor whose watches the
 * file services keep. A change happens in the service that serves the
 * volume, so the service makes the events (abi/idl/fswatch.idl); libc
 * keeps the descriptor:
 *
 *   - the **bell**: an event made at inotify_init. Every watcher channel
 *     gets a duplicate (RIGHT_SIGNAL | RIGHT_TRANSFER) as its first message
 *     (fswatch_bell.give), and the service sets FSWATCH_BELL when its queue
 *     for the channel gets a first event. libc sets INO_HELD, a second user
 *     bit, while it holds records no read has taken. The two are the kind's
 *     arm (readable when either is up), so poll, select and epoll wait on
 *     one handle whatever the descriptor watches;
 *   - the **watcher channels**, one per mount it watches through (fs.watch
 *     on the mount's `fs` channel the first time), keyed by that channel's
 *     koid, so a mount replaced meanwhile gets a new one. A service that
 *     has no watches (ERR_NOT_SUPPORTED) is remembered too;
 *   - the **watches**: a wd for each (channel, the service's id), or for a
 *     quiet watch (below). wds come from a counter that only grows (to
 *     INT_MAX, then from 1 again, skipping those in use), so an ended
 *     watch's wd isn't handed out again soon (Chromium's rule, 1.2);
 *   - the **buffer**: records taken from the services and not yet read, in
 *     Linux's form (struct inotify_event, the name NUL-padded to a multiple
 *     of 16 bytes), at most INO_BUF_MAX of them taken at a time.
 *
 * **Taking.** FSWATCH_BELL is cleared first, then each channel is taken
 * until its `more` is 0, so an event queued after a take always rings
 * again. A channel left with records (the buffer full) rings the bell
 * itself. Records are converted as they come: id 0 is IN_Q_OVERFLOW (wd
 * -1), and a wd is forgotten when its IN_IGNORED is converted. A channel
 * that closed (the mount went, or the service's successor started fresh or
 * had no bell for it: fswatch.idl) gives IN_UNMOUNT and IN_IGNORED for each
 * of its wds, and the next add on that mount asks fs.watch again.
 *
 * **Quiet watches.** "/", /svc, libc's own trees (/dev, /etc, /proc, /sys)
 * and a read-only mount whose service has no watches never change, or
 * change only as Linux's /proc does, without events: a watch there checks
 * the path is there and never fires. A writable mount whose service has no
 * watches (fat: the owner's Q1) is EOPNOTSUPP.
 *
 * **Locks.** The description's lock guards everything here but the bell
 * and is held across a service call, as a file's is, never across the
 * bell's wait.
 *
 * **fork and exec** (Q3): as an epoll set, absent in a forked child and
 * never given to a spawned one or across execve. */
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <idl/fswatch.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include "files.h"

#define INO_HELD      (1u << 25)        /* the bell's other bit: libc holds records */
#define INO_BUF_MAX   65536u            /* the most bytes of records a take fills to */
#define INO_CHANS     32u               /* mounts watched through (replaced ones too) */
#define INO_WATCHES   8192u             /* wds held at once */
#define INO_QUIET     0xffu             /* a quiet watch's channel */
#define INO_LOOK_NS   NS_PER_S          /* a blocked read looks for closed channels this often */
#define INO_ALL_BITS  (IN_ALL_EVENTS | IN_UNMOUNT | IN_Q_OVERFLOW | IN_IGNORED | IN_ONLYDIR | \
                       IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD | \
                       IN_ISDIR | IN_ONESHOT)

/* The services speak Linux's bits. */
_Static_assert(FSWATCH_IN_ACCESS == IN_ACCESS && FSWATCH_IN_MODIFY == IN_MODIFY &&
               FSWATCH_IN_ATTRIB == IN_ATTRIB && FSWATCH_IN_CLOSE_WRITE == IN_CLOSE_WRITE &&
               FSWATCH_IN_CLOSE_NOWRITE == IN_CLOSE_NOWRITE && FSWATCH_IN_OPEN == IN_OPEN &&
               FSWATCH_IN_MOVED_FROM == IN_MOVED_FROM && FSWATCH_IN_MOVED_TO == IN_MOVED_TO &&
               FSWATCH_IN_CREATE == IN_CREATE && FSWATCH_IN_DELETE == IN_DELETE &&
               FSWATCH_IN_DELETE_SELF == IN_DELETE_SELF && FSWATCH_IN_MOVE_SELF == IN_MOVE_SELF &&
               FSWATCH_IN_ALL_EVENTS == IN_ALL_EVENTS, "fswatch's events are Linux's");
_Static_assert(FSWATCH_IN_UNMOUNT == IN_UNMOUNT && FSWATCH_IN_Q_OVERFLOW == IN_Q_OVERFLOW &&
               FSWATCH_IN_IGNORED == IN_IGNORED && FSWATCH_IN_ONLYDIR == IN_ONLYDIR &&
               FSWATCH_IN_DONT_FOLLOW == IN_DONT_FOLLOW &&
               FSWATCH_IN_EXCL_UNLINK == IN_EXCL_UNLINK &&
               FSWATCH_IN_MASK_CREATE == IN_MASK_CREATE && FSWATCH_IN_MASK_ADD == IN_MASK_ADD &&
               FSWATCH_IN_ISDIR == IN_ISDIR && FSWATCH_IN_ONESHOT == IN_ONESHOT,
               "fswatch's flags are Linux's");
_Static_assert(FSWATCH_BELL != INO_HELD && (INO_HELD & SIG_USER_ALL), "two user bits");

/* A mount watched through. */
struct ino_chan {
    uint64_t koid;   /* the mount's `fs` channel's; 0: a free entry */
    handle_t ch;     /* its watcher channel; HANDLE_INVALID: its service has no watches */
};

/* A wd. */
struct ino_watch {
    int      wd;
    uint8_t  chan;      /* its channel's entry, or INO_QUIET */
    bool     removed;   /* inotify_rm_watch'd: forgotten when its IN_IGNORED comes */
    uint32_t id;        /* the service's id (0 for a quiet watch) */
    uint32_t mask;      /* a quiet watch's (IN_MASK_ADD adds to it) */
    char    *path;      /* a quiet watch's path, its key (malloc'd) */
};

struct ino {
    handle_t         bell;              /* FSWATCH_BELL (the services) and INO_HELD (ours) */
    struct ino_chan  chans[INO_CHANS];
    struct ino_watch *w;                /* nw of them, room for room */
    uint32_t         nw, room;
    int              next_wd;           /* where the next wd is looked for */
    uint8_t         *buf;               /* records: [head, len) unread; cap bytes */
    uint32_t         head, len, cap;
    uint32_t         bytes;             /* len - head, for ready (no lock: atomic) */
    bool             held;              /* INO_HELD is up */
};

static struct ino *ino_of(struct fd_desc *d)
{
    return d->priv;
}

/* ---- the buffer -------------------------------------------------------------- */

/* Room for need more bytes at the buffer's end: false if out of memory. */
static bool room_locked(struct ino *n, uint32_t need)
{
    if (n->cap - n->len >= need)
        return true;
    if (n->head) {
        memmove(n->buf, n->buf + n->head, n->len - n->head);
        n->len -= n->head;
        n->head = 0;
        if (n->cap - n->len >= need)
            return true;
    }
    uint32_t cap = n->cap ? n->cap : 4096;
    while (cap - n->len < need)
        cap *= 2;
    uint8_t *b = realloc(n->buf, cap);
    if (!b)
        return false;
    n->buf = b;
    n->cap = cap;
    return true;
}

/* A record at the buffer's end (dropped if out of memory). */
static void put_locked(struct ino *n, int wd, uint32_t mask, uint32_t cookie, const char *name,
                       uint32_t len)
{
    uint32_t pad = len ? (len + 1 + 15) & ~15u : 0;   /* Linux's: a NUL, then to 16 bytes */
    struct inotify_event e = { .wd = wd, .mask = mask, .cookie = cookie, .len = pad };
    if (!room_locked(n, sizeof(e) + pad))
        return;
    memcpy(n->buf + n->len, &e, sizeof(e));
    memset(n->buf + n->len + sizeof(e), 0, pad);
    if (len)
        memcpy(n->buf + n->len + sizeof(e), name, len);
    n->len += sizeof(e) + pad;
}

/* INO_HELD and bytes as the buffer is now. */
static void held_locked(struct ino *n)
{
    uint32_t bytes = n->len - n->head;
    if (!bytes)
        n->head = n->len = 0;
    __atomic_store_n(&n->bytes, bytes, __ATOMIC_RELAXED);
    if (n->held == (bytes != 0))
        return;
    n->held = bytes != 0;
    /* An event we made, with all its rights: nothing it can refuse. */
    (void)jam_event_signal(n->bell, n->held ? 0 : INO_HELD, n->held ? INO_HELD : 0);
}

/* ---- the watches ---------------------------------------------------------------- */

static struct ino_watch *by_wd(struct ino *n, int wd)
{
    for (uint32_t i = 0; i < n->nw; i++)
        if (n->w[i].wd == wd)
            return &n->w[i];
    return NULL;
}

static struct ino_watch *by_id(struct ino *n, unsigned c, uint32_t id)
{
    for (uint32_t i = 0; i < n->nw; i++)
        if (n->w[i].chan == c && n->w[i].id == id)
            return &n->w[i];
    return NULL;
}

static struct ino_watch *by_path(struct ino *n, const char *path)
{
    for (uint32_t i = 0; i < n->nw; i++)
        if (n->w[i].chan == INO_QUIET && !strcmp(n->w[i].path, path))
            return &n->w[i];
    return NULL;
}

/* Room for one more watch: 0, or -ENOSPC (the most), -ENOMEM. */
static long watch_room_locked(struct ino *n)
{
    if (n->nw < n->room)
        return 0;
    if (n->nw >= INO_WATCHES)
        return -ENOSPC;
    uint32_t room = n->room ? n->room * 2 : 16;
    struct ino_watch *w = realloc(n->w, room * sizeof(*w));
    if (!w)
        return -ENOMEM;
    n->w = w;
    n->room = room;
    return 0;
}

/* A new watch (watch_room_locked made room): its wd. */
static int watch_new_locked(struct ino *n, unsigned c, uint32_t id, uint32_t mask, char *path)
{
    int wd;
    do {
        wd = n->next_wd;
        n->next_wd = wd == INT32_MAX ? 1 : wd + 1;   /* an int's most */
    } while (by_wd(n, wd));
    n->w[n->nw++] = (struct ino_watch){ .wd = wd, .chan = (uint8_t)c, .id = id, .mask = mask,
                                        .path = path };
    return wd;
}

static void forget_locked(struct ino *n, struct ino_watch *w)
{
    free(w->path);
    *w = n->w[--n->nw];
}

/* Channel c is gone (its peer closed): every watch on it ends, IN_UNMOUNT
 * then IN_IGNORED (IN_IGNORED alone for one already removed). */
static void chan_gone_locked(struct ino *n, unsigned c)
{
    for (uint32_t i = 0; i < n->nw;) {
        struct ino_watch *w = &n->w[i];
        if (w->chan != c) {
            i++;
            continue;
        }
        if (!w->removed)
            put_locked(n, w->wd, IN_UNMOUNT, 0, NULL, 0);
        put_locked(n, w->wd, IN_IGNORED, 0, NULL, 0);
        forget_locked(n, w);
    }
    jam_handle_close(n->chans[c].ch);
    n->chans[c] = (struct ino_chan){ 0 };
}

/* ---- taking ------------------------------------------------------------------- */

/* The room to ask a take for: records of RECORD_FIXED + len bytes become
 * Linux's of at most 16 + len + 16, so 14 bytes asked may need 32 here. */
static uint32_t take_room(const struct ino *n)
{
    uint32_t held = n->len - n->head;
    uint32_t room = held >= INO_BUF_MAX ? 0 : (INO_BUF_MAX - held) / 32 * 14;
    return room < FSWATCH_TAKE_BYTES ? room : FSWATCH_TAKE_BYTES;
}

/* got bytes of channel c's records into the buffer. */
static void convert_locked(struct ino *n, unsigned c, const uint8_t *b, uint32_t got)
{
    for (uint32_t o = 0; got - o >= FSWATCH_RECORD_FIXED;) {
        uint32_t id, mask, cookie;
        uint16_t len;
        memcpy(&id, b + o, 4);
        memcpy(&mask, b + o + 4, 4);
        memcpy(&cookie, b + o + 8, 4);
        memcpy(&len, b + o + 12, 2);
        o += FSWATCH_RECORD_FIXED;
        if (len > got - o)
            return;   /* a broken service: the rest dropped */
        const char *name = (const char *)b + o;
        o += len;
        if (!id) {
            put_locked(n, -1, mask, 0, NULL, 0);   /* IN_Q_OVERFLOW */
            continue;
        }
        struct ino_watch *w = by_id(n, c, id);
        if (!w)
            continue;   /* no watch of ours (a broken service) */
        put_locked(n, w->wd, mask, cookie, name, len);
        if (mask & IN_IGNORED)
            forget_locked(n, w);
    }
}

/* Take what every channel has queued, as far as the buffer goes (the
 * file's header). */
static void take_locked(struct ino *n)
{
    (void)jam_event_signal(n->bell, FSWATCH_BELL, 0);
    uint8_t *tmp = NULL;
    bool again = false;
    for (unsigned c = 0; c < INO_CHANS; c++) {
        handle_t ch = n->chans[c].ch;
        while (n->chans[c].koid && ch != HANDLE_INVALID) {
            uint32_t room = take_room(n), got = 0;
            uint8_t more = 0;
            if (!tmp)
                tmp = malloc(FSWATCH_TAKE_BYTES);
            status_t st = !tmp || room < FSWATCH_RECORD_FIXED ? ERR_BUFFER_TOO_SMALL
                          : fswatch_take_within(ch, FILE_TIMEOUT, room, &more, tmp, &got);
            if (st == ERR_PEER_CLOSED) {
                chan_gone_locked(n, c);
                break;
            }
            if (st != OK) {
                again = true;   /* left for the next take: ring for it */
                break;
            }
            convert_locked(n, c, tmp, got);
            if (!more)
                break;
        }
    }
    free(tmp);
    if (again)
        (void)jam_event_signal(n->bell, 0, FSWATCH_BELL);
    held_locked(n);
}

/* Channels whose peer closed, without asking their services (a blocked
 * read's look): whether any was. */
static bool reap_locked(struct ino *n)
{
    bool any = false;
    for (unsigned c = 0; c < INO_CHANS; c++)
        if (n->chans[c].koid && n->chans[c].ch != HANDLE_INVALID &&
            jam_object_wait_one(n->chans[c].ch, SIG_PEER_CLOSED, 0, NULL) == OK) {
            chan_gone_locked(n, c);
            any = true;
        }
    held_locked(n);
    return any;
}

/* ---- the kind's operations ----------------------------------------------------- */

/* As many whole records as fit in total bytes into iov: the bytes, 0 for
 * none held, -EINVAL if the first doesn't fit, -EFAULT (kept then). */
static long out_locked(struct ino *n, const struct iovec *iov, int iovcnt, size_t total)
{
    size_t take = 0;
    while (n->head + take < n->len) {
        struct inotify_event e;
        memcpy(&e, n->buf + n->head + take, sizeof(e));
        size_t one = sizeof(e) + e.len;
        if (take + one > total)
            break;
        take += one;
    }
    if (!take)
        return n->head == n->len ? 0 : -EINVAL;
    long r = fd_iov_check(iov, iovcnt, 0, take, true);
    if (!r)
        r = fd_iov_scatter(iov, iovcnt, 0, n->buf + n->head, take);
    if (r)
        return r;
    n->head += (uint32_t)take;
    held_locked(n);
    return (long)take;
}

static long ino_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    if (pos != FD_AT_OFFSET)
        return -ESPIPE;
    long total = fd_iov_total(iov, iovcnt);
    if (total <= 0)
        return total;   /* 0 bytes: 0 at once, never a wait (Chromium's reader, 1.1) */
    struct ino *n = ino_of(d);
    for (;;) {
        lock_take(&d->lock);
        if (n->head == n->len)
            take_locked(n);
        long r = out_locked(n, iov, iovcnt, (size_t)total);
        lock_give(&d->lock);
        if (r)
            return r;
        if (fd_status(d) & O_NONBLOCK)
            return -EAGAIN;
        for (;;) {
            status_t st = jam_object_wait_one(n->bell, FSWATCH_BELL | INO_HELD,
                                              now() + INO_LOOK_NS, NULL);
            if (st == OK)
                break;
            if (st != ERR_TIMED_OUT)
                return lx_err(st);   /* EINTR */
            lock_take(&d->lock);
            bool gone = reap_locked(n);
            lock_give(&d->lock);
            if (gone)
                break;
        }
    }
}

static long ino_ioctl(struct fd_desc *d, unsigned long req, long arg)
{
    if (req != FIONREAD)
        return -ENOTTY;
    struct ino *n = ino_of(d);
    lock_take(&d->lock);
    take_locked(n);
    int bytes = (int)(n->len - n->head);
    lock_give(&d->lock);
    return uc_put((void *)(uintptr_t)arg, &bytes, sizeof(bytes));
}

static long ino_stat(struct fd_desc *d, struct stat *st)
{
    struct handle_info hi;
    st->st_mode = 0600;   /* no file type: Linux's anonymous inode */
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;
    st->st_blksize = 4096;
    st->st_ino = jam_handle_info(ino_of(d)->bell, &hi) == OK ? (ino_t)hi.koid : 0;
    return 0;
}

/* poll's hooks (ready.h): the records held, and the bell's two bits. */
static uint32_t ino_ready(struct fd_desc *d, uint32_t interest)
{
    return __atomic_load_n(&ino_of(d)->bytes, __ATOMIC_RELAXED) ? NETWAIT_READ : 0;
}

static long ino_arm(struct fd_desc *d, uint32_t interest, struct fd_wait *out)
{
    out->kind = FD_WAIT_HANDLE;
    out->h = (struct netwait_handle){ .h = ino_of(d)->bell, .read = FSWATCH_BELL | INO_HELD };
    return 0;
}

static void ino_close(struct fd_desc *d)
{
    struct ino *n = ino_of(d);
    for (unsigned c = 0; c < INO_CHANS; c++)
        if (n->chans[c].koid && n->chans[c].ch != HANDLE_INVALID)
            jam_handle_close(n->chans[c].ch);   /* the service ends the channel's watches */
    for (uint32_t i = 0; i < n->nw; i++)
        free(n->w[i].path);
    jam_handle_close(n->bell);
    free(n->w);
    free(n->buf);
    free(n);
}

/* No give (a spawned child, or across execve, never gets it) and absent
 * after fork: Q3, as an epoll set. */
const struct fd_ops inotify_ops = {
    .name = "inotify",
    .read = ino_read,
    .stat = ino_stat,
    .ioctl = ino_ioctl,
    .ready = ino_ready,
    .arm = ino_arm,
    .fork = fd_fork_absent,
    .close = ino_close,
};

/* ---- the calls ------------------------------------------------------------------- */

long lx_inotify_init1(long flags, long b, long c, long d, long e, long f)
{
    if (flags & ~(long)(IN_NONBLOCK | IN_CLOEXEC))
        return -EINVAL;
    struct ino *n = calloc(1, sizeof(*n));
    if (!n || jam_event_create(&n->bell) != OK) {
        free(n);
        return -ENOMEM;
    }
    n->next_wd = 1;
    uint32_t status = O_RDONLY | (flags & IN_NONBLOCK ? O_NONBLOCK : 0);
    return kind_install(&inotify_ops, n, status, flags & IN_CLOEXEC);
}

long lx_inotify_init(long a, long b, long c, long d, long e, long f)
{
    return lx_inotify_init1(0, 0, 0, 0, 0, 0);
}

/* Is p there, for a quiet watch: 0 and *dir, or -errno (PATH_LINK_OUT
 * for a link out of a mount). */
static long quiet_there(struct path *p, bool *dir)
{
    enum path_libc which = path_libc(p);
    if (which != LIBC_NONE) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        long r = which == LIBC_PROC && p->nofollow ? proc_stat(p, false, &st)
                                                   : path_libc_stat(p, which, &st);
        *dir = S_ISDIR(st.st_mode);
        return r;
    }
    struct path_info i;
    long r = path_info(p, &i);
    *dir = i.is_dir;
    return r;
}

/* A quiet watch on p (there: checked), keyed by its path: its wd, or
 * -errno. */
static long quiet_add(struct fd_desc *d, const struct path *p, uint32_t mask)
{
    struct ino *n = ino_of(d);
    long r = 0;
    lock_take(&d->lock);
    struct ino_watch *w = by_path(n, p->abs);
    if (w && (mask & IN_MASK_CREATE)) {
        r = -EEXIST;
    } else if (w) {
        w->mask = (mask & IN_MASK_ADD ? w->mask : 0) | mask;
        r = w->wd;
    } else if (!(r = watch_room_locked(n))) {
        char *path = strdup(p->abs);
        r = path ? watch_new_locked(n, INO_QUIET, 0, mask, path) : -ENOMEM;
    }
    lock_give(&d->lock);
    return r;
}

/* The entry of the mount whose `fs` channel is fs (koid), made the first
 * time (fs.watch, then the bell): its index, or -errno. */
static long chan_locked(struct ino *n, const struct path *p, handle_t fs, uint64_t koid)
{
    unsigned c, free_c = INO_CHANS;
    for (c = 0; c < INO_CHANS; c++) {
        if (n->chans[c].koid == koid)
            return c;
        if (!n->chans[c].koid && free_c == INO_CHANS)
            free_c = c;
    }
    for (c = 0; c < INO_CHANS && free_c == INO_CHANS; c++) {   /* full: one nothing uses */
        bool used = false;
        for (uint32_t i = 0; i < n->nw && !used; i++)
            used = n->w[i].chan == c;
        if (!used) {
            if (n->chans[c].ch != HANDLE_INVALID)
                jam_handle_close(n->chans[c].ch);
            n->chans[c] = (struct ino_chan){ 0 };
            free_c = c;
        }
    }
    if (free_c == INO_CHANS)
        return -ENOSPC;
    handle_t ch = HANDLE_INVALID, give;
    status_t st = fs_watch_within(fs, FILE_TIMEOUT, &ch);
    if (st == ERR_NOT_SUPPORTED) {
        n->chans[free_c] = (struct ino_chan){ .koid = koid, .ch = HANDLE_INVALID };
        return free_c;
    }
    if (st == ERR_NO_RESOURCES)
        return -ENOSPC;   /* the service's watcher channels: Linux's answer for a limit */
    if (st != OK)
        return path_err_read(p, st);
    st = jam_handle_duplicate(n->bell, RIGHT_SIGNAL | RIGHT_TRANSFER, &give);
    if (st == OK)
        st = fswatch_bell_give(ch, give);
    if (st != OK) {
        jam_handle_close(ch);
        return lx_err(st);
    }
    n->chans[free_c] = (struct ino_chan){ .koid = koid, .ch = ch };
    return free_c;
}

/* fswatch.add's answer id (existed) on channel c as a wd. An id we hold
 * that comes back with existed 0 is a new watch: the old one ended and its
 * IN_IGNORED was lost to an overflow (fswatch.idl), so it is made here. */
static long add_done_locked(struct ino *n, unsigned c, uint32_t id, uint8_t existed)
{
    struct ino_watch *w = by_id(n, c, id);
    if (w && existed && !w->removed)
        return w->wd;
    if (w) {
        put_locked(n, w->wd, IN_IGNORED, 0, NULL, 0);
        forget_locked(n, w);
    }
    return watch_new_locked(n, c, id, 0, NULL);   /* room made before the add */
}

/* An add on p's mount: the wd, -errno, PATH_LINK_OUT, or -EOPNOTSUPP with
 * *none set when the mount's service has no watches. */
static long mount_add(struct fd_desc *d, struct path *p, uint32_t mask, bool *none)
{
    struct ino *n = ino_of(d);
    handle_t fs;
    long r = path_fs(p, &fs);
    if (r)
        return r;
    struct handle_info hi;
    if (jam_handle_info(fs, &hi) != OK || !hi.koid) {
        jam_handle_close(fs);
        return -EIO;
    }
    uint64_t koid = hi.koid;
    uint32_t len;
    const char *rel = path_rel(p, &len);
    lock_take(&d->lock);
    for (int tries = 0;; tries++) {
        long c = chan_locked(n, p, fs, koid);
        if (c < 0) {
            r = c;
            break;
        }
        if (n->chans[c].ch == HANDLE_INVALID) {
            *none = true;
            r = -EOPNOTSUPP;
            break;
        }
        if ((r = watch_room_locked(n)))
            break;
        uint32_t id = 0;
        uint8_t existed = 0;
        status_t st = fswatch_add_within(n->chans[c].ch, FILE_TIMEOUT, rel, len, mask, &id,
                                         &existed);
        if (st == ERR_PEER_CLOSED) {
            chan_gone_locked(n, (unsigned)c);   /* its watches ended: ask fs.watch again */
            if (!tries)
                continue;
        }
        if (st == OK && !id)
            st = ERR_IO;   /* a broken service */
        r = st == OK ? add_done_locked(n, (unsigned)c, id, existed) : path_err_read(p, st);
        break;
    }
    held_locked(n);
    lock_give(&d->lock);
    jam_handle_close(fs);
    return r;
}

/* inotify_add_watch of p (resolved) with mask (checked). */
static long add_path(struct fd_desc *d, struct path *p, uint32_t mask)
{
    for (;;) {
        bool none = false, dir = false;
        long r;
        if (path_libc(p) == LIBC_NONE && p->where == PATH_MOUNT) {
            r = mount_add(d, p, mask, &none);
            if (!none) {
                if (path_again(p, &r))
                    continue;
                return r;
            }
        }
        r = quiet_there(p, &dir);
        if (path_again(p, &r))
            continue;
        if (!r && (mask & IN_ONLYDIR) && !dir)
            r = -ENOTDIR;
        if (!r && none && !path_read_only(p))
            r = -EOPNOTSUPP;   /* changes would go unseen (fat: Q1) */
        return r ? r : quiet_add(d, p, mask);
    }
}

long lx_inotify_add_watch(long fd, long pathname, long mask_l, long d, long e, long f)
{
    uint32_t mask = (uint32_t)mask_l;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE))
        return -EINVAL;
    if (!(mask & INO_ALL_BITS))
        return -EINVAL;
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    struct path *p = NULL;
    long r = desc->ops != &inotify_ops ? -EINVAL : 0;
    if (!r && !(p = malloc(sizeof(*p))))
        r = -ENOMEM;
    if (!r)
        r = path_get(AT_FDCWD, (const char *)(uintptr_t)pathname, p);
    if (!r) {
        p->nofollow = (mask & IN_DONT_FOLLOW) != 0;
        r = add_path(desc, p, mask);
    }
    free(p);
    fd_put(desc);
    return r;
}

long lx_inotify_rm_watch(long fd, long wd, long c, long d, long e, long f)
{
    struct fd_desc *desc = fd_get(fd);
    if (!desc)
        return -EBADF;
    if (desc->ops != &inotify_ops) {
        fd_put(desc);
        return -EINVAL;
    }
    struct ino *n = ino_of(desc);
    long r = -EINVAL;
    lock_take(&desc->lock);
    struct ino_watch *w = by_wd(n, (int)wd);
    if (w && w->removed) {
        w = NULL;   /* removed already: its IN_IGNORED is on its way */
    } else if (w && w->chan == INO_QUIET) {
        put_locked(n, w->wd, IN_IGNORED, 0, NULL, 0);
        forget_locked(n, w);
        r = 0;
    } else if (w) {
        unsigned ch = w->chan;
        status_t st = fswatch_remove_within(n->chans[ch].ch, FILE_TIMEOUT, w->id);
        if (st == OK || st == ERR_PEER_CLOSED)
            w->removed = true;   /* the service queued IN_IGNORED */
        if (st == ERR_PEER_CLOSED)
            chan_gone_locked(n, ch);   /* ... or it is gone, the watch with it */
        r = st == OK || st == ERR_PEER_CLOSED ? 0 : st == ERR_TIMED_OUT ? -EIO : lx_err(st);
    }
    held_locked(n);
    lock_give(&desc->lock);
    fd_put(desc);
    return r;
}
