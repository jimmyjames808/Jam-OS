/* posixtest's inotify section (docs/INOTIFY-PLAN.md section 3, IL1): the
 * inotify descriptor (user/posix/sys/inotify.c).
 *
 * Most of it runs against fake file services of this process's own,
 * mounted in our namespace: /ifake keeps its watches in libos's
 * <fswatch.h> as a service does (the test says what changed, under the
 * fake's lock), /ifakero is read-only without watches (the boot image's
 * answer: its watches are quiet) and /ifakefat is writable without watches
 * (fat's, the owner's Q1: EOPNOTSUPP). Each fake channel has a thread of
 * its own serving it. Then the real namespace: quiet watches on "/",
 * libc's trees and /boot; /data and /tmp, whose services serve fs.watch
 * from IE1 (until then EOPNOTSUPP, said and skipped; a writable fat /data
 * must say it); a forked and a spawned child without the descriptor. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fswatch.h>
#include <jamos/libjam.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"
/* jl-names.h renames the layer's file_map (not used here) and fs_idl.h
 * names the `file` stub so: the stub's name wins. */
#undef file_map
#include <fs_idl.h>

#define SELF        "/boot/bin/posixtest"
#define POLL_NS     (20 * NS_PER_MS)   /* the fake's threads look at their flags this often */
#define CR_MASK     (IN_ATTRIB | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVE | IN_ONLYDIR)
#define EXT_MAGIC   0xef53
#define FAT_MAGIC   0x4d44
#define EV_MAX      1100

extern char **environ;

/* ---- the fake services -------------------------------------------------------- */

/* The fake volume's inodes. */
static const struct node {
    const char    *path;
    struct fsw_key key;
    bool           dir;
} nodes[] = {
    { "/", { 2, 0 }, true },    { "/d", { 10, 1 }, true }, { "/d/f", { 11, 1 }, false },
    { "/e", { 12, 1 }, true },  { "/g", { 13, 1 }, false },
};
#define K_D (&nodes[1].key)
#define K_F (&nodes[2].key)
#define K_E (&nodes[3].key)
#define K_G (&nodes[4].key)

struct fake {
    const char     *mount;
    bool            ro;
    struct fsw     *w;                    /* NULL: no watches (fs.watch not served) */
    pthread_mutex_t mu;                   /* w and the rest */
    handle_t        srv;                  /* the mount's `fs` channel, our end */
    unsigned        watches;              /* fs.watch calls answered */
    uint32_t        last_chan;            /* the last one's channel */
    uint32_t        last_id;              /* the last id add answered */
    uint32_t        reuse_id;             /* not 0: the next add answers it, existed 0 */
    bool            close_req[FSW_CHANNELS];   /* the channel's thread closes it */
    int             threads;              /* serving now (atomic) */
};

static struct fsw_table tab;
static struct fsw       w;
static struct fake      fk = { .mount = "/ifake", .w = &w, .mu = PTHREAD_MUTEX_INITIALIZER };
static struct fake      fkro = { .mount = "/ifakero", .ro = true, .mu = PTHREAD_MUTEX_INITIALIZER };
static struct fake      fkfat = { .mount = "/ifakefat", .mu = PTHREAD_MUTEX_INITIALIZER };

static const struct node *lookup(const char *path, uint32_t len)
{
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
        if (strlen(nodes[i].path) == len && !memcmp(nodes[i].path, path, len))
            return &nodes[i];
    return NULL;
}

/* A watcher channel's requests (fswatch.idl), the fake's lock held. */
struct wctx {
    struct fake *f;
    uint32_t     c;
};

static status_t w_add(void *ctx, const char *path, uint32_t len, uint32_t mask, uint32_t *id,
                      uint8_t *existed)
{
    struct wctx *x = ctx;
    status_t st = fsw_mask_check(mask);
    const struct node *n = st == OK ? lookup(path, len) : NULL;
    if (st != OK)
        return st;
    if (!n)
        return ERR_NOT_FOUND;
    if ((mask & IN_ONLYDIR) && !n->dir)
        return ERR_NOT_DIR;
    if (x->f->reuse_id) {   /* an id the client holds, as if its watch ended unseen */
        *id = x->f->reuse_id;
        *existed = 0;
        x->f->reuse_id = 0;
        return OK;
    }
    st = fsw_add(x->f->w, x->c, &n->key, n->dir, mask, id, existed);
    if (st == OK)
        x->f->last_id = *id;
    return st;
}

static status_t w_remove(void *ctx, uint32_t id)
{
    struct wctx *x = ctx;
    return fsw_remove(x->f->w, x->c, id);
}

static status_t w_take(void *ctx, uint32_t room, uint8_t *more, uint8_t *out, uint32_t *len)
{
    struct wctx *x = ctx;
    return fsw_take(x->f->w, x->c, room, more, out, len);
}

static const struct fswatch_ops watch_ops = { .add = w_add, .remove = w_remove, .take = w_take };

/* Wait up to POLL_NS for h to be readable or closed: OK and *seen, or
 * ERR_TIMED_OUT, or the wait's failure. */
static status_t look(handle_t h, signals_t *seen)
{
    return jam_object_wait_one(h, SIG_READABLE | SIG_PEER_CLOSED, now() + POLL_NS, seen);
}

/* A watcher channel's thread: its bell, then its requests, until the
 * client closes it or the test asks the fake to (a successor's close). */
static void *watcher_main(void *arg)
{
    struct wctx x = *(struct wctx *)arg;
    struct fake *f = x.f;
    free(arg);
    for (bool done = false; !done;) {
        pthread_mutex_lock(&f->mu);
        handle_t ch = f->w->chans[x.c].ch;
        done = f->close_req[x.c];
        f->close_req[x.c] = false;
        if (done)
            fsw_chan_close(f->w, x.c);
        pthread_mutex_unlock(&f->mu);
        signals_t seen = 0;
        status_t st = done ? OK : look(ch, &seen);
        if (done || st == ERR_TIMED_OUT)
            continue;
        pthread_mutex_lock(&f->mu);
        if (st != OK || !(seen & SIG_READABLE)) {
            done = true;
        } else if (!fsw_belled(f->w, x.c)) {
            st = fsw_chan_bell(f->w, x.c);
            done = st != OK && st != ERR_SHOULD_WAIT;
        } else {
            (void)fswatch_serve_one(ch, &watch_ops, &x);
        }
        if (done)
            fsw_chan_close(f->w, x.c);
        pthread_mutex_unlock(&f->mu);
    }
    __atomic_sub_fetch(&f->threads, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static bool start(void *(*fn)(void *), void *arg, int *threads)
{
    pthread_t t;
    __atomic_add_fetch(threads, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&t, NULL, fn, arg)) {
        __atomic_sub_fetch(threads, 1, __ATOMIC_SEQ_CST);
        return false;
    }
    pthread_detach(t);
    return true;
}

/* The mount's requests (fs.idl), the fake's lock held. */
static status_t f_stat(void *ctx, const char *path, uint32_t len, uint64_t *size, uint8_t *is_dir,
                       uint64_t *mtime, uint64_t *id)
{
    (void)ctx;
    const struct node *n = lookup(path, len);
    if (!n)
        return ERR_NOT_FOUND;
    *size = 0;
    *is_dir = n->dir;
    *mtime = 0;
    *id = n->key.ino;
    return OK;
}

static status_t f_statfs(void *ctx, uint64_t *total, uint64_t *free_bytes, uint8_t *ro,
                         uint8_t label[16], uint32_t *magic, uint32_t *name_max,
                         uint32_t *features)
{
    struct fake *f = ctx;
    *total = 1u << 20;
    *free_bytes = 1u << 19;
    *ro = f->ro;
    memset(label, 0, 16);
    *magic = 0x01021994;
    *name_max = 255;
    *features = 0;
    return OK;
}

static status_t f_watch(void *ctx, handle_t *out)
{
    struct fake *f = ctx;
    if (!f->w)
        return ERR_NOT_SUPPORTED;
    handle_t client, srv;
    uint32_t c;
    struct wctx *x = malloc(sizeof(*x));
    status_t st = x ? jam_channel_create(&client, &srv) : ERR_NO_MEMORY;
    if (st == OK && (st = fsw_chan_new(f->w, 0, 0, &nodes[0].key, srv, &c)) != OK)
        jam_handle_close(client);
    if (st != OK) {
        free(x);
        return st;
    }
    *x = (struct wctx){ f, c };
    if (!start(watcher_main, x, &f->threads)) {
        free(x);
        fsw_chan_close(f->w, c);
        jam_handle_close(client);
        return ERR_NO_RESOURCES;
    }
    f->watches++;
    f->last_chan = c;
    *out = client;
    return OK;
}

static const struct fs_ops mount_ops = { .stat = f_stat, .statfs = f_statfs, .watch = f_watch };

/* The mount's thread: until the client end goes (ns_unmount). */
static void *mount_main(void *arg)
{
    struct fake *f = arg;
    for (;;) {
        signals_t seen = 0;
        status_t st = look(f->srv, &seen);
        if (st == ERR_TIMED_OUT)
            continue;
        if (st != OK || !(seen & SIG_READABLE))
            break;
        pthread_mutex_lock(&f->mu);
        (void)fs_serve_one(f->srv, &mount_ops, f);
        pthread_mutex_unlock(&f->mu);
    }
    jam_handle_close(f->srv);
    __atomic_sub_fetch(&f->threads, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static bool mount_fake(struct fake *f)
{
    handle_t client;
    if (jam_channel_create(&client, &f->srv) != OK)
        return false;
    if (!start(mount_main, f, &f->threads)) {
        jam_handle_close(client);
        jam_handle_close(f->srv);
        return false;
    }
    return ns_mount(f->mount, client) == OK;
}

/* Unmount f and wait (2 s at most) for its threads to end. */
static void unmount_fake(struct fake *f)
{
    (void)ns_unmount(f->mount);
    for (int i = 0; i < 200 && __atomic_load_n(&f->threads, __ATOMIC_SEQ_CST); i++)
        usleep(10000);
    CHECK(__atomic_load_n(&f->threads, __ATOMIC_SEQ_CST) == 0);
}

/* Something changed on /ifake: fsw_event's or fsw_dir_event's. */
static void change(const struct fsw_key *dir, const char *name, const struct fsw_key *self,
                   uint32_t mask)
{
    pthread_mutex_lock(&fk.mu);
    if (self)
        fsw_event(fk.w, dir, name, name ? (uint32_t)strlen(name) : 0, self, mask, 0);
    else
        fsw_dir_event(fk.w, dir, name, (uint32_t)strlen(name), mask, 0, 0);
    pthread_mutex_unlock(&fk.mu);
}

/* ---- reading records -------------------------------------------------------------- */

struct ev {
    int      wd;
    uint32_t mask, cookie, len;
    char     name[32];
};

static struct ev evs[EV_MAX];
static char      rbuf[80 * 1024];

/* n bytes of records into evs from at: how many, or -1 if one is malformed
 * (its len not a multiple of 16, past the end, a name not NUL-padded). */
static int parse(const char *b, long n, int at)
{
    int k = at;
    for (long o = 0; o < n; k++) {
        struct inotify_event e;
        if (n - o < (long)sizeof(e) || k == EV_MAX)
            return -1;
        memcpy(&e, b + o, sizeof(e));
        o += sizeof(e);
        if (e.len % 16 || n - o < (long)e.len)
            return -1;
        size_t nl = e.len ? strnlen(b + o, e.len) : 0;
        if (e.len && nl == e.len)
            return -1;   /* no NUL */
        for (size_t i = nl; i < e.len; i++)
            if (b[o + i])
                return -1;   /* the padding is NULs */
        evs[k] = (struct ev){ e.wd, e.mask, e.cookie, e.len, "" };
        snprintf(evs[k].name, sizeof(evs[k].name), "%.*s", (int)nl, b + o);
        o += e.len;
    }
    return k - at;
}

/* Everything readable now (fd non-blocking): records into evs, how many,
 * -1 on a malformed one. */
static int drain(int fd)
{
    int k = 0;
    for (;;) {
        long n = read(fd, rbuf, sizeof(rbuf));
        if (n < 0) {
            CHECK(errno == EAGAIN);
            return k;
        }
        int m = parse(rbuf, n, k);
        if (m < 0)
            return -1;
        k += m;
    }
}

static bool is(const struct ev *e, int wd, uint32_t mask, const char *name)
{
    return e->wd == wd && e->mask == mask && !strcmp(e->name, name);
}

static long fionread(int fd)
{
    int n = -1;
    return ioctl(fd, FIONREAD, &n) ? -1 : n;
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}

/* A thread that makes one change (or asks the fake to close channel c)
 * after a short sleep. */
struct later {
    long     ms;
    int      close_c;   /* >= 0: close that channel instead */
    uint32_t mask;
    char     name[16];
};

static void *later_main(void *arg)
{
    struct later *l = arg;
    nap_ms(l->ms);
    if (l->close_c >= 0) {
        pthread_mutex_lock(&fk.mu);
        fk.close_req[l->close_c] = true;
        pthread_mutex_unlock(&fk.mu);
    } else {
        change(K_E, l->name, NULL, l->mask);
    }
    return NULL;
}

/* ---- cases on the fake ---------------------------------------------------------------- */

static void init_flags(void)
{
    errno = 0;
    CHECK(inotify_init1(0x40) == -1 && errno == EINVAL);
    CHECK(inotify_init1(IN_CLOEXEC | 1) == -1 && errno == EINVAL);
    int a = inotify_init(), b = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    CHECK(a >= 0 && b >= 0);
    CHECK(fcntl(a, F_GETFD) == 0 && fcntl(b, F_GETFD) == FD_CLOEXEC);
    CHECK((fcntl(a, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == O_RDONLY);
    CHECK((fcntl(b, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDONLY | O_NONBLOCK));
    struct stat st;
    CHECK(fstat(a, &st) == 0 && (st.st_mode & 0777) == 0600 && !S_ISREG(st.st_mode));
    char c;
    CHECK(write(a, "x", 1) == -1 && errno == EBADF);   /* opened read-only, as Linux's */
    CHECK(lseek(a, 0, SEEK_SET) == -1 && errno == ESPIPE);
    CHECK(pread(a, &c, 1, 0) == -1 && errno == ESPIPE);
    CHECK(read(b, &c, 0) == 0);   /* 0 bytes: at once */
    CHECK(read(b, rbuf, sizeof(rbuf)) == -1 && errno == EAGAIN);
    CHECK(close(a) == 0 && close(b) == 0);
}

static void add_errors(int fd)
{
    int p[2];
    CHECK(pipe(p) == 0);
    errno = 0;
    CHECK(inotify_add_watch(fd, "/ifake/d", 0) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(fd, "/ifake/d", 0x8000000) == -1 && errno == EINVAL);   /* no IN_* bit */
    CHECK(inotify_add_watch(fd, "/ifake/d", IN_MASK_ADD | IN_MASK_CREATE | IN_CREATE) == -1 &&
          errno == EINVAL);
    CHECK(inotify_add_watch(p[0], "/ifake/d", IN_CREATE) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(999, "/ifake/d", IN_CREATE) == -1 && errno == EBADF);
    CHECK(inotify_add_watch(fd, "/ifake/zz", IN_CREATE) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, "", IN_CREATE) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, NULL, IN_CREATE) == -1 && errno == EFAULT);
    CHECK(inotify_add_watch(fd, "/ifake/g", IN_CREATE | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    CHECK(inotify_rm_watch(p[0], 1) == -1 && errno == EINVAL);
    CHECK(inotify_rm_watch(fd, 12345) == -1 && errno == EINVAL);
    close(p[0]);
    close(p[1]);
}

/* Records, their form, FIONREAD, short and empty reads, readiness. */
static void records(int fd)
{
    int wd = inotify_add_watch(fd, "/ifake/d", IN_ALL_EVENTS | IN_ONLYDIR);
    int wf = inotify_add_watch(fd, "/ifake/d/f", IN_CLOSE_WRITE);
    CHECK(wd > 0 && wf > 0 && wd != wf);
    CHECK(inotify_add_watch(fd, "/ifake/d", IN_CREATE) == wd);   /* the same inode: its wd */
    CHECK(inotify_add_watch(fd, "/ifake/d", IN_CREATE | IN_MASK_CREATE) == -1 && errno == EEXIST);
    CHECK(inotify_add_watch(fd, "/ifake/d", IN_ALL_EVENTS) == wd);
    struct pollfd pf = { fd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0);
    CHECK(fionread(fd) == 0);
    change(K_D, "f", K_F, IN_CLOSE_WRITE);
    CHECK(poll(&pf, 1, 1000) == 1 && pf.revents == POLLIN);
    CHECK(fionread(fd) == 32 + 16);   /* "f": 16 + 16; the file's own: 16 */
    CHECK(read(fd, rbuf, 31) == -1 && errno == EINVAL);   /* the first doesn't fit */
    CHECK(read(fd, rbuf, 0) == 0);
    CHECK(read(fd, rbuf, 40) == 32);   /* whole records only */
    CHECK(parse(rbuf, 32, 0) == 1 && is(&evs[0], wd, IN_CLOSE_WRITE, "f") && evs[0].len == 16);
    CHECK(poll(&pf, 1, 0) == 1);   /* the file's own, held */
    CHECK(read(fd, rbuf, sizeof(rbuf)) == 16);
    CHECK(parse(rbuf, 16, 0) == 1 && is(&evs[0], wf, IN_CLOSE_WRITE, "") && evs[0].len == 0);
    CHECK(poll(&pf, 1, 0) == 0 && fionread(fd) == 0);
    /* A long name: padded to 16 with a NUL after it. */
    char name[40] = "a-name-of-exactly-31-bytes-long";
    change(K_D, name, NULL, IN_CREATE);
    CHECK(read(fd, rbuf, sizeof(rbuf)) == 16 + 32);
    CHECK(parse(rbuf, 48, 0) == 1 && is(&evs[0], wd, IN_CREATE, name) && evs[0].len == 32);
    name[15] = '\0';   /* 15 bytes: 16 with its NUL */
    change(K_D, name, NULL, IN_CREATE);
    change(K_D, name, NULL, IN_CREATE);   /* the same again: merged by the service */
    CHECK(read(fd, rbuf, sizeof(rbuf)) == 32);
    CHECK(parse(rbuf, 32, 0) == 1 && evs[0].len == 16 && is(&evs[0], wd, IN_CREATE, name));
    CHECK(inotify_rm_watch(fd, wf) == 0);
    CHECK(drain(fd) == 1 && is(&evs[0], wf, IN_IGNORED, ""));
    CHECK(inotify_rm_watch(fd, wf) == -1 && errno == EINVAL);
}

/* poll, epoll, select and a blocking read woken by another thread's change. */
static void waits(int fd, int ep)
{
    int we = inotify_add_watch(fd, "/ifake/e", IN_CREATE | IN_DELETE);
    CHECK(we > 0);
    struct later l = { .ms = 30, .close_c = -1, .mask = IN_CREATE };
    pthread_t t;
    struct pollfd pf = { fd, POLLIN, 0 };
    strcpy(l.name, "p");
    CHECK(pthread_create(&t, NULL, later_main, &l) == 0);
    CHECK(poll(&pf, 1, 5000) == 1 && pf.revents == POLLIN);
    pthread_join(t, NULL);
    CHECK(drain(fd) == 1 && is(&evs[0], we, IN_CREATE, "p"));
    struct epoll_event ee = { .events = EPOLLIN }, got;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ee) == 0);
    CHECK(epoll_wait(ep, &got, 1, 0) == 0);
    strcpy(l.name, "e");
    CHECK(pthread_create(&t, NULL, later_main, &l) == 0);
    CHECK(epoll_wait(ep, &got, 1, 5000) == 1 && got.events == EPOLLIN);
    pthread_join(t, NULL);
    CHECK(drain(fd) == 1 && is(&evs[0], we, IN_CREATE, "e"));
    CHECK(epoll_wait(ep, &got, 1, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == 0);
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = { 5, 0 };
    strcpy(l.name, "s");
    CHECK(pthread_create(&t, NULL, later_main, &l) == 0);
    CHECK(select(fd + 1, &rs, NULL, NULL, &tv) == 1 && FD_ISSET(fd, &rs));
    pthread_join(t, NULL);
    CHECK(drain(fd) == 1 && is(&evs[0], we, IN_CREATE, "s"));
    /* A blocking read (the descriptor without O_NONBLOCK for it). */
    CHECK(fcntl(fd, F_SETFL, 0) == 0);
    strcpy(l.name, "b");
    l.mask = IN_DELETE;
    CHECK(pthread_create(&t, NULL, later_main, &l) == 0);
    long n = read(fd, rbuf, sizeof(rbuf));
    pthread_join(t, NULL);
    CHECK(n == 32 && parse(rbuf, n, 0) == 1 && is(&evs[0], we, IN_DELETE, "b"));
    CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    /* IN_MASK_ADD adds; a plain add replaces. */
    CHECK(inotify_add_watch(fd, "/ifake/e", IN_CREATE) == we);
    change(K_E, "x", NULL, IN_DELETE);
    CHECK(drain(fd) == 0);
    CHECK(inotify_add_watch(fd, "/ifake/e", IN_DELETE | IN_MASK_ADD) == we);
    change(K_E, "y", NULL, IN_DELETE);
    change(K_E, "z", NULL, IN_CREATE);
    CHECK(drain(fd) == 2 && is(&evs[0], we, IN_DELETE, "y") && is(&evs[1], we, IN_CREATE, "z"));
}

/* Chromium's reader (base's inotify_reader): poll, FIONREAD, a read of
 * that size, over 1,000 changes another thread makes (paced so the
 * service's queue never overflows). */
static int seen_n;

static void *changer_main(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        char name[16];
        for (int t = 0; i - __atomic_load_n(&seen_n, __ATOMIC_SEQ_CST) >= 200 && t < 5000; t++)
            usleep(1000);
        snprintf(name, sizeof(name), "c%d", i);
        change(K_E, name, NULL, IN_CREATE);
    }
    return NULL;
}

static void reader_loop(int fd)
{
    int we = inotify_add_watch(fd, "/ifake/e", IN_CREATE);
    pthread_t t;
    seen_n = 0;
    CHECK(pthread_create(&t, NULL, changer_main, NULL) == 0);
    bool ok = true;
    int zero = 0, rounds = 0;
    while (ok && seen_n < 1000) {
        struct pollfd pf = { fd, POLLIN, 0 };
        long size = poll(&pf, 1, 5000) == 1 ? fionread(fd) : -1;
        long n = size >= 0 && size <= (long)sizeof(rbuf) ? read(fd, rbuf, (size_t)size) : -1;
        ok = n == size;
        zero += n == 0;
        rounds++;
        int k = ok ? parse(rbuf, n, 0) : -1;
        for (int i = 0; ok && i < k; i++) {
            char want[16];
            snprintf(want, sizeof(want), "c%d", seen_n);
            ok = is(&evs[i], we, IN_CREATE, want);
            __atomic_add_fetch(&seen_n, 1, __ATOMIC_SEQ_CST);
        }
        ok = ok && k >= 0;
    }
    pthread_join(t, NULL);
    CHECK(ok && seen_n == 1000);
    printf("posixtest: inotify: the reader loop: 1000 changes in %d rounds (%d of 0 bytes)\n",
           rounds, zero);
}

/* A full service queue: one IN_Q_OVERFLOW (wd -1) after what fitted. */
static void overflow(int fd)
{
    int we = inotify_add_watch(fd, "/ifake/e", IN_CREATE);
    for (int i = 0; i < 1100; i++) {
        char name[16];
        snprintf(name, sizeof(name), "o%d", i);
        change(K_E, name, NULL, IN_CREATE);
    }
    int k = drain(fd);
    CHECK(k > 500 && k < 1100);
    CHECK(k > 0 && is(&evs[k - 1], -1, IN_Q_OVERFLOW, "") && evs[k - 1].cookie == 0);
    CHECK(k > 1 && evs[k - 2].wd == we && evs[k - 2].mask == IN_CREATE);
}

/* IR's 2: with the service's queue full, a removal's IN_IGNORED is dropped,
 * and so are the IN_DELETE_SELF and IN_IGNORED of a watch that ends (whose
 * removal then finds nothing). Each wd ends once its channel is taken
 * empty, with an IN_IGNORED of libc's, and is no longer held. */
static void lost_ignored(int fd)
{
    int wf = inotify_add_watch(fd, "/ifake/d/f", IN_ATTRIB);
    int wg = inotify_add_watch(fd, "/ifake/g", IN_ATTRIB | IN_DELETE_SELF);
    int we = inotify_add_watch(fd, "/ifake/e", IN_CREATE);
    CHECK(wf > 0 && wg > 0 && we > 0);
    for (int i = 0; i < 1100; i++) {
        char name[16];
        snprintf(name, sizeof(name), "l%d", i);
        change(K_E, name, NULL, IN_CREATE);
    }
    CHECK(inotify_rm_watch(fd, wf) == 0);   /* its IN_IGNORED: past the queue's room */
    pthread_mutex_lock(&fk.mu);
    fsw_gone(fk.w, K_G);                    /* g's two records: dropped too */
    pthread_mutex_unlock(&fk.mu);
    CHECK(inotify_rm_watch(fd, wg) == 0);   /* nothing left to remove: OK */
    int k = drain(fd), over = -1, ignf = -1, igng = -1, others = 0;
    for (int i = 0; i < k; i++) {
        if (is(&evs[i], -1, IN_Q_OVERFLOW, ""))
            over = i;
        else if (is(&evs[i], wf, IN_IGNORED, ""))
            ignf = i;
        else if (is(&evs[i], wg, IN_IGNORED, ""))
            igng = i;
        else if (!is(&evs[i], we, IN_CREATE, evs[i].name))
            others++;
    }
    if (over < 0 || ignf < over || igng < over || others)
        printf("posixtest: inotify: lost_ignored: %d records, the overflow at %d, f's "
               "IN_IGNORED at %d, g's at %d, %d others\n", k, over, ignf, igng, others);
    CHECK(k > 500 && over >= 0 && ignf > over && igng > over && !others);
    CHECK(inotify_rm_watch(fd, wf) == -1 && errno == EINVAL);
    CHECK(inotify_rm_watch(fd, wg) == -1 && errno == EINVAL);
}

/* An id we hold back from an add with existed 0: its watch ended unseen
 * (its IN_IGNORED lost to an overflow), so its wd ends here and the add
 * is a new wd. */
static void reused_id(int fd)
{
    int wd = inotify_add_watch(fd, "/ifake/d", IN_CREATE);
    pthread_mutex_lock(&fk.mu);
    fk.reuse_id = fk.last_id;
    pthread_mutex_unlock(&fk.mu);
    int again = inotify_add_watch(fd, "/ifake/g", IN_CREATE);
    CHECK(wd > 0 && again > 0 && again != wd);
    CHECK(drain(fd) == 1 && is(&evs[0], wd, IN_IGNORED, ""));
    change(K_D, "r", NULL, IN_CREATE);   /* the id's watch: now the new wd's */
    CHECK(drain(fd) == 1 && is(&evs[0], again, IN_CREATE, "r"));
    CHECK(inotify_rm_watch(fd, wd) == -1 && errno == EINVAL);
}

/* The service closes our channel (a successor that started fresh, or had
 * no bell for it): IN_UNMOUNT, IN_IGNORED for each wd; the next add asks
 * fs.watch again. A blocked read sees it within about a second. */
static void channel_closed(void)
{
    int fd = inotify_init1(IN_CLOEXEC);
    int wd = inotify_add_watch(fd, "/ifake/d", IN_CREATE);
    int we = inotify_add_watch(fd, "/ifake/e", IN_CREATE);
    CHECK(wd > 0 && we > 0);
    pthread_mutex_lock(&fk.mu);
    unsigned asked = fk.watches;
    int c = (int)fk.last_chan;   /* this descriptor's: the last fs.watch */
    CHECK(fk.w->t->chans[c].live && fk.w->t->chans[c].watches == 2);
    pthread_mutex_unlock(&fk.mu);
    struct later l = { .ms = 30, .close_c = c };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, later_main, &l) == 0);
    uint64_t t0 = now();
    long n = read(fd, rbuf, sizeof(rbuf));
    pthread_join(t, NULL);
    int k = n > 0 ? parse(rbuf, n, 0) : -1;
    CHECK(k == 4 && now() - t0 < 3 * NS_PER_S);
    int unmounts = 0, ignored = 0;
    for (int i = 0; i < k; i++) {
        unmounts += (evs[i].wd == wd || evs[i].wd == we) && evs[i].mask == IN_UNMOUNT;
        ignored += (evs[i].wd == wd || evs[i].wd == we) && evs[i].mask == IN_IGNORED;
    }
    CHECK(unmounts == 2 && ignored == 2);
    int again = inotify_add_watch(fd, "/ifake/d", IN_CREATE);
    CHECK(again > 0 && again != wd && fk.watches == asked + 1);
    change(K_D, "n", NULL, IN_CREATE);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    CHECK(drain(fd) == 1 && is(&evs[0], again, IN_CREATE, "n"));
    close(fd);
}

/* wds aren't handed out again soon (Chromium's rule): each add after a
 * removal gets a new one. */
static void wds_grow(int fd)
{
    int last = 0;
    bool grew = true;
    for (int i = 0; i < 20; i++) {
        int wd = inotify_add_watch(fd, "/ifake/g", IN_ATTRIB);
        grew &= wd > last;
        last = wd;
        CHECK(inotify_rm_watch(fd, wd) == 0);
    }
    CHECK(grew);
    CHECK(drain(fd) == 20);
}

/* Quiet watches: there, never firing; and a writable mount without watches. */
static void quiet(int fd)
{
    static const char *const there[] = { "/", "/etc", "/proc", "/proc/self", "/dev", "/boot",
                                          "/boot/bin", "/ifakero", "/ifakero/d" };
    int wds[sizeof(there) / sizeof(there[0])];
    for (unsigned i = 0; i < sizeof(there) / sizeof(there[0]); i++) {
        wds[i] = inotify_add_watch(fd, there[i], IN_ALL_EVENTS | IN_ONLYDIR);
        if (wds[i] < 0)
            printf("posixtest: inotify: quiet %s: %s\n", there[i], strerror(errno));
        CHECK(wds[i] > 0);
    }
    CHECK(inotify_add_watch(fd, "/", IN_CREATE) == wds[0]);
    CHECK(inotify_add_watch(fd, "/", IN_CREATE | IN_MASK_CREATE) == -1 && errno == EEXIST);
    CHECK(inotify_add_watch(fd, "/dev/null", IN_ATTRIB) > 0);
    CHECK(inotify_add_watch(fd, "/dev/null", IN_ATTRIB | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    CHECK(inotify_add_watch(fd, SELF, IN_ATTRIB | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    CHECK(inotify_add_watch(fd, "/etc/no-such-file", IN_ATTRIB) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, "/boot/no-such-file", IN_ATTRIB) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, "/ifakero/zz", IN_ATTRIB) == -1 && errno == ENOENT);
    CHECK(inotify_add_watch(fd, "/ifakero/g", IN_ATTRIB | IN_ONLYDIR) == -1 && errno == ENOTDIR);
    struct stat st;
    if (stat("/svc", &st) == 0)
        CHECK(inotify_add_watch(fd, "/svc", IN_CREATE) > 0);
    /* fat: writable, its changes unseen (Q1). */
    CHECK(inotify_add_watch(fd, "/ifakefat/d", IN_CREATE) == -1 && errno == EOPNOTSUPP);
    CHECK(inotify_add_watch(fd, "/ifakefat/zz", IN_CREATE) == -1 && errno == ENOENT);
    CHECK(fionread(fd) == 0);   /* nothing ever fires */
    struct pollfd pf = { fd, POLLIN, 0 };
    CHECK(inotify_rm_watch(fd, wds[0]) == 0);
    CHECK(poll(&pf, 1, 0) == 1 && fionread(fd) == 16);   /* its IN_IGNORED, at once */
    CHECK(drain(fd) == 1 && is(&evs[0], wds[0], IN_IGNORED, ""));
    CHECK(inotify_rm_watch(fd, wds[0]) == -1 && errno == EINVAL);
    CHECK(inotify_add_watch(fd, "/", IN_CREATE) > wds[0]);
}

/* ---- the real namespace ---------------------------------------------------------------- */

/* Is each of want[] in evs[0 .. k) in order (others between allowed)? */
static bool in_order(int k, int wd, const uint32_t *masks, const char *const *names, int nwant)
{
    int j = 0;
    for (int i = 0; i < k && j < nwant; i++)
        j += is(&evs[i], wd, masks[j], names[j]);
    return j == nwant;
}

/* A watch on a directory of mount m whose service keeps watches (extfs from
 * IE1): Chromium's mask, and the events it reads. */
static void real_events(const char *m)
{
    char dir[64], a[80], b[80];
    snprintf(dir, sizeof(dir), "%s/inotify-t", m);
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    unlink(a);
    unlink(b);
    rmdir(dir);
    CHECK(mkdir(dir, 0755) == 0);
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = inotify_add_watch(fd, dir, CR_MASK | IN_DELETE_SELF);
    CHECK(wd > 0);
    int f = open(a, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(f >= 0 && write(f, "x", 1) == 1 && close(f) == 0);
    CHECK(rename(a, b) == 0);
    CHECK(chmod(b, 0600) == 0);
    CHECK(unlink(b) == 0);
    CHECK(rmdir(dir) == 0);
    int k = drain(fd);
    static const uint32_t masks[] = { IN_CREATE, IN_CLOSE_WRITE, IN_MOVED_FROM, IN_MOVED_TO,
                                      IN_ATTRIB, IN_DELETE, IN_DELETE_SELF, IN_IGNORED };
    static const char *const names[] = { "a", "a", "a", "b", "b", "b", "", "" };
    CHECK(in_order(k, wd, masks, names, 8));
    int from = -1, to = -1;
    for (int i = 0; i < k; i++) {
        from = evs[i].mask == IN_MOVED_FROM ? i : from;
        to = evs[i].mask == IN_MOVED_TO ? i : to;
    }
    CHECK(from >= 0 && to >= 0 && evs[from].cookie && evs[from].cookie == evs[to].cookie);
    close(fd);
    printf("posixtest: inotify: %s: events from its service (%d records)\n", m, k);
}

static long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

/* n cycles of what Chromium's RacyRecursiveWatch makes 2,000 of: a
 * directory made, a file written in it, the file and the directory
 * removed. With fd >= 0 a watch is added on each new directory, as
 * Chromium does when it reads the directory's IN_CREATE, and the records
 * are read after each cycle: 7 (the parent's 2, the directory's 5).
 * Returns the microseconds the cycles took. */
static long long racy_cycles(const char *dir, int n, int fd)
{
    char sub[96], file[112];
    int recs = 0;
    long long t0 = now_us();
    for (int i = 0; i < n; i++) {
        snprintf(sub, sizeof(sub), "%s/s%d", dir, i);
        snprintf(file, sizeof(file), "%s/f", sub);
        CHECK(mkdir(sub, 0755) == 0);
        CHECK(fd < 0 || inotify_add_watch(fd, sub, CR_MASK | IN_DELETE_SELF) > 0);
        int f = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(f >= 0 && write(f, "content", 7) == 7 && close(f) == 0);
        CHECK(unlink(file) == 0 && rmdir(sub) == 0);
        recs += fd >= 0 ? drain(fd) : 0;
    }
    long long took = now_us() - t0;
    CHECK(fd < 0 || recs == 7 * n);
    return took;
}

/* A thread that makes a directory after a nap, and says when it was made. */
struct mk {
    char      path[96];
    long long done;
};

static void *mk_main(void *arg)
{
    struct mk *k = arg;
    nap_ms(30);
    mkdir(k->path, 0755);
    k->done = now_us();
    return NULL;
}

/* The same cycles on mount m without a watch and with Chromium's, and how
 * long a `poll` blocked in one thread takes to see a directory another
 * thread made: what Chromium's RacyRecursiveWatch (100 rounds of 20 cycles,
 * 208 to 259 s on /data) is made of. A watch must cost a cycle next to
 * nothing (the cycle's own time is the file service's: a commit each
 * request) and the wake must be prompt; the bounds are loose, the PC's load
 * moves the times. */
static void racy_cost(const char *m)
{
    enum { N = 10, WAKES = 5 };
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/inotify-r", m);
    rmdir(dir);
    CHECK(mkdir(dir, 0755) == 0);
    long long plain = racy_cycles(dir, N, -1);
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0 && inotify_add_watch(fd, dir, CR_MASK) > 0);
    long long watched = racy_cycles(dir, N, fd);
    close(fd);
    fd = inotify_init1(IN_CLOEXEC);
    CHECK(fd >= 0 && inotify_add_watch(fd, dir, CR_MASK) > 0);
    long long sum = 0, worst = 0;
    for (int i = 0; i < WAKES; i++) {
        struct mk k = { .done = 0 };
        snprintf(k.path, sizeof(k.path), "%s/w%d", dir, i);
        pthread_t th;
        CHECK(pthread_create(&th, NULL, mk_main, &k) == 0);
        struct pollfd pf = { fd, POLLIN, 0 };
        CHECK(poll(&pf, 1, 5000) == 1);
        long long woke = now_us();
        pthread_join(th, NULL);
        char b[512];
        CHECK(read(fd, b, sizeof(b)) > 0);
        long long lag = woke - k.done;
        sum += lag;
        worst = lag > worst ? lag : worst;
        CHECK(rmdir(k.path) == 0);
        while (poll(&pf, 1, 20) == 1)
            CHECK(read(fd, b, sizeof(b)) > 0);
    }
    close(fd);
    CHECK(rmdir(dir) == 0);
    CHECK(watched < 3 * plain + 50000);
    CHECK(worst < 500000);
    printf("posixtest: inotify: %s: %d make-write-remove cycles: %lld us each with no watch, %lld with "
           "watches; a blocked poll woke %lld us after the mkdir returned (worst %lld)\n",
           m, (int)N, plain / N, watched / N, sum / WAKES, worst);
}

/* /data and /tmp: events if their services keep watches, else EOPNOTSUPP;
 * a writable fat mount must say EOPNOTSUPP. */
static void real_mounts(void)
{
    static const char *const mounts[] = { "/data", "/tmp" };
    for (unsigned i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        struct statfs s;
        if (statfs(mounts[i], &s) != 0) {
            printf("posixtest: inotify: %s: no statfs (%s): skipped\n", mounts[i], strerror(errno));
            continue;
        }
        int fd = inotify_init1(IN_CLOEXEC);
        int wd = inotify_add_watch(fd, mounts[i], CR_MASK);
        bool fat = s.f_type == FAT_MAGIC && !(s.f_flags & ST_RDONLY);
        close(fd);
        if (fat) {
            CHECK(wd == -1 && errno == EOPNOTSUPP);
            printf("posixtest: inotify: %s: fat, refused (EOPNOTSUPP)\n", mounts[i]);
        } else if (wd < 0 && errno == EOPNOTSUPP) {
            printf("posixtest: inotify: %s (%#lx): no watches from its service yet\n", mounts[i],
                   (unsigned long)s.f_type);
        } else if (!(s.f_flags & ST_RDONLY) && wd > 0) {
            real_events(mounts[i]);
            racy_cost(mounts[i]);
        } else {
            CHECK(wd > 0);   /* read-only: quiet */
            printf("posixtest: inotify: %s: read-only, a quiet watch\n", mounts[i]);
        }
    }
}

/* Absent in a forked child; not given to a spawned one even without
 * FD_CLOEXEC (Q3: as an epoll set, so execve's hand-over drops it too). */
static void children(void)
{
    int fd = inotify_init();
    CHECK(fd >= 0 && fcntl(fd, F_GETFD) == 0);
    pid_t pid = fork();
    if (pid == 0)
        _exit(fcntl(fd, F_GETFD) == -1 && errno == EBADF ? 0 : 1);
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    char num[16];
    snprintf(num, sizeof(num), "%d", fd);
    char *argv[] = { SELF, "--child-inotify", num, NULL };
    CHECK(posix_spawn(&pid, SELF, NULL, NULL, argv, environ) == 0);
    status = -1;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(fd);
}

int pt_child_inotify(int argc, char **argv)
{
    int fd = argc > 2 ? atoi(argv[2]) : -1;
    return fd >= 0 && fcntl(fd, F_GETFD) == -1 && errno == EBADF ? 0 : 1;
}

void pt_inotify(char **argv)
{
    (void)argv;
    fsw_init(&w, &tab);
    if (!mount_fake(&fk) || !mount_fake(&fkro) || !mount_fake(&fkfat)) {
        CHECK(!"the fake services mounted");
        return;
    }
    init_flags();
    int fd = inotify_init1(IN_NONBLOCK);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(fd >= 0 && ep >= 0);
    add_errors(fd);
    records(fd);
    waits(fd, ep);
    reader_loop(fd);
    overflow(fd);
    lost_ignored(fd);
    reused_id(fd);
    wds_grow(fd);
    quiet(fd);
    close(fd);
    close(ep);
    channel_closed();
    unmount_fake(&fk);
    unmount_fake(&fkro);
    unmount_fake(&fkfat);
    real_mounts();
    children();
}
