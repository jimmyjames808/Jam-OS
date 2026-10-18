/* /dev (docs/M13-PLAN.md section 3): libc's own, no service behind it.
 *
 *   /dev/null, /dev/zero, /dev/full, /dev/random, /dev/urandom
 *       as Linux's: null reads the end at once and takes every write;
 *       zero reads zeros; full reads zeros and every write is ENOSPC;
 *       random and urandom read the kernel's generator (seeded at boot,
 *       so neither ever blocks) and take writes as Linux does, changing
 *       nothing. Each seeks to 0, and reads and writes at any position.
 *   /dev/stdin, /dev/stdout, /dev/stderr, /dev/fd/N
 *       a dup of descriptor 0, 1, 2 or N (Linux reopens the file behind
 *       it; for a pipe or a terminal that is the same thing).
 *   /dev/shm/<name>
 *       shm_open's objects (musl opens them here): memory files
 *       (memfd.c) by name in this process. No service holds the names,
 *       so another process sees an object only through a descriptor it
 *       was given; shm_unlink drops the name, and the object lives while
 *       a descriptor or a mapping holds it.
 *   /dev/tty  the process's terminal (tty.c's tty_controlling: its
 *       description shared, as a dup), ENXIO if it has none.
 *   /dev/ptmx, /dev/pts, /dev/pts/N
 *       pseudo-terminals: pty.c's.
 *
 * /dev, /dev/fd and /dev/shm stat as directories; listing them is not
 * built. The nodes are static: a description's priv is the node's entry
 * in `nodes`, and closing one frees nothing. The shm names are under
 * shm_lock; an object's own state is memfd.c's. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/sysmacros.h>
#include "kinds.h"
#include "memfd.h"
#include "pty.h"
#include "tty.h"

#define SHM_DIR     "/dev/shm/"
#define SHM_MAX     64          /* names at once */
#define ZERO_CHUNK  4096u

enum node_kind { N_NULL, N_ZERO, N_FULL, N_RANDOM };

struct node {
    const char    *name;    /* under /dev */
    enum node_kind kind;
    unsigned       minor;   /* Linux's (major 1, the memory devices) */
};

static const struct node nodes[] = {
    { "null", N_NULL, 3 },
    { "zero", N_ZERO, 5 },
    { "full", N_FULL, 7 },
    { "random", N_RANDOM, 8 },
    { "urandom", N_RANDOM, 9 },
};

struct shm_name {
    char           name[NAME_MAX + 1];   /* "" for a free slot */
    struct memobj *m;                    /* one reference: the name's */
};

static lock_t          shm_lock;
static struct shm_name shm[SHM_MAX];
static const uint8_t   zeros[ZERO_CHUNK];

void shm_forked(struct lx_fork_io *io)
{
    shm_lock = (lock_t){ 0 };
    for (unsigned i = 0; i < SHM_MAX; i++)
        if (shm[i].name[0] && !memobj_forked_name(io, shm[i].m))
            shm[i] = (struct shm_name){ 0 };   /* its object is the parent's alone here */
}

static const struct node *node_of(struct fd_desc *d)
{
    return d->priv;
}

static long node_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0 || node_of(d)->kind == N_NULL)
        return n < 0 ? n : 0;
    bool random = node_of(d)->kind == N_RANDOM;
    uint8_t chunk[512];   /* random bytes made here, then copied out */
    size_t step = random ? sizeof(chunk) : ZERO_CHUNK;
    for (size_t at = 0; at < (size_t)n; at += step) {
        size_t k = (size_t)n - at < step ? (size_t)n - at : step;
        if (random)
            os_random(chunk, k);
        if (fd_iov_scatter(iov, iovcnt, at, random ? chunk : zeros, k))
            return at ? (long)at : -EFAULT;
    }
    return n;
}

static long node_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    long n = fd_iov_total(iov, iovcnt);
    return n > 0 && node_of(d)->kind == N_FULL ? -ENOSPC : n;
}

static long node_seek(struct fd_desc *d, int64_t off, int whence)
{
    return 0;
}

static void node_stat_of(const struct node *n, struct stat *st)
{
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_rdev = makedev(1, n->minor);
    st->st_ino = n->minor;
    st->st_blksize = 4096;
}

static long node_stat(struct fd_desc *d, struct stat *st)
{
    node_stat_of(node_of(d), st);
    return 0;
}

static long node_give(struct fd_desc *d, struct fd_given *out)
{
    const char *name = node_of(d)->name;
    long r = 0;
    void *data = kind_data(name, strlen(name) + 1, &r);
    if (r)
        return r;
    out->kind = POSIXFDS_DEV;
    out->status = fd_status(d);
    out->offset = 0;
    out->nhandles = 0;
    out->data = data;
    out->data_len = (uint32_t)strlen(name) + 1;
    return 0;
}

const struct fd_ops dev_ops = {
    .name = "/dev node",
    .read = node_read,
    .write = node_write,
    .seek = node_seek,
    .stat = node_stat,
    /* .ready, .arm: none (always ready, as the access mode allows) */
    .give = node_give,
};

/* ---- the log: 1 and 2 when the starter gave no channel ---------------------- */

static long log_write(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    long done = 0;
    for (int i = 0; i < iovcnt; i++)
        for (size_t at = 0; at < iov[i].iov_len;) {
            size_t k = iov[i].iov_len - at > DEBUG_WRITE_MAX ? DEBUG_WRITE_MAX
                                                             : iov[i].iov_len - at;
            if (jam_debug_write((const char *)iov[i].iov_base + at, k) != OK)
                return done ? done : -EIO;
            at += k;
            done += (long)k;
        }
    return done;
}

static long log_stat(struct fd_desc *d, struct stat *st)
{
    st->st_mode = S_IFCHR | 0222;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

/* The log is no /dev node a path reaches, but a child is given it as one
 * (posix_spawn, and an exec in place: FJ1, where a program whose 1 and 2
 * were the log lost them at its execve): /dev's kind with the name
 * LOG_NAME, which node_named doesn't know, so only dev_adopt makes it a
 * log again. A C library from before that leaves the descriptor out
 * (ENOENT), as it was before. */
#define LOG_NAME "log"

static long log_give(struct fd_desc *d, struct fd_given *out)
{
    long r = 0;
    void *data = kind_data(LOG_NAME, sizeof(LOG_NAME), &r);
    if (r)
        return r;
    out->kind = POSIXFDS_DEV;
    out->status = fd_status(d);
    out->offset = 0;
    out->nhandles = 0;
    out->data = data;
    out->data_len = sizeof(LOG_NAME);
    return 0;
}

static const struct fd_ops log_ops = {
    .name = "log",
    .write = log_write,
    .stat = log_stat,
    .give = log_give,
};

static const struct node *node_named(const char *name);

void stdfd_default(void)
{
    unsigned term = tty_stdfds();   /* the console's terminal, where the starter gave one */
    if (term & 1u << 1)
        return;   /* 0 to 2 */
    struct fd_desc *in = term ? NULL : fd_new(&dev_ops, O_RDONLY, (void *)node_named("null"));
    if (in)
        (void)fd_replace(0, in, false);   /* 0 is in range and free at the start */
    handle_t ch = startup_handle(SR_STDOUT);
    struct fd_desc *out = ch == HANDLE_INVALID ? fd_new(&log_ops, O_WRONLY, NULL)
                                               : pipe_desc_new(ch, O_WRONLY);
    if (!out)
        return;
    if (term & 1u << 2) {   /* 2 is the terminal, 1 the starter's channel */
        (void)fd_replace(1, out, false);
        return;
    }
    fd_ref(out);   /* 1 and 2 share it, as a shell's 2>&1 */
    (void)fd_replace(1, out, false);
    (void)fd_replace(2, out, false);
}

static const struct node *node_named(const char *name)
{
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
        if (!strcmp(nodes[i].name, name))
            return &nodes[i];
    return NULL;
}

/* abs names something under a node that is no directory (/dev/null/x,
 * /dev/tty/x): ENOTDIR, as on Linux, not ENOENT. */
static bool under_node(const char *abs)
{
    const char *rest = abs + 5;   /* past "/dev/": dev_path's */
    const char *slash = strchr(rest, '/');
    char name[16];
    if (!slash || (size_t)(slash - rest) >= sizeof(name))
        return false;
    memcpy(name, rest, (size_t)(slash - rest));
    name[slash - rest] = '\0';
    return node_named(name) || !strcmp(name, "tty");
}

/* A description of node n: nodes[] is never written through it (dev_ops
 * only reads its priv). */
static struct fd_desc *node_desc(const struct node *n, uint32_t status)
{
    return fd_new(&dev_ops, status, (void *)n);
}

long dev_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
               struct fd_desc **out)
{
    if (!strcmp(data, LOG_NAME)) {   /* the starter's log (log_give): write-only, as ours */
        *out = fd_new(&log_ops, (rec->status & ~(uint32_t)O_ACCMODE) | O_WRONLY, NULL);
        return *out ? 0 : -ENOMEM;
    }
    const struct node *n = node_named(data);   /* a string: posixfds_read checked */
    if (!n)
        return -ENOENT;
    *out = node_desc(n, rec->status);
    return *out ? 0 : -ENOMEM;
}

bool dev_path(const char *abs)
{
    return !strcmp(abs, "/dev") || !strncmp(abs, "/dev/", 5);
}

static bool is_dir(const char *abs)
{
    return !strcmp(abs, "/dev") || !strcmp(abs, "/dev/fd") || !strcmp(abs, "/dev/shm");
}

/* "/dev/fd/N", "/dev/stdin", ...: N, or -1. */
static long fd_node(const char *abs)
{
    static const char *const std[] = { "/dev/stdin", "/dev/stdout", "/dev/stderr" };
    for (int i = 0; i < 3; i++)
        if (!strcmp(abs, std[i]))
            return i;
    if (strncmp(abs, "/dev/fd/", 8) || !abs[8] || strlen(abs + 8) > 9)
        return -1;
    long n = 0;
    for (const char *p = abs + 8; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

/* The name after /dev/shm/, or NULL if abs is no shm path; *err for a
 * name that is empty or too long. */
static const char *shm_name_of(const char *abs, long *err)
{
    if (strncmp(abs, SHM_DIR, sizeof(SHM_DIR) - 1))
        return NULL;
    const char *name = abs + sizeof(SHM_DIR) - 1;
    size_t len = strlen(name);
    *err = !len || strchr(name, '/') ? -ENOENT : len > NAME_MAX ? -ENAMETOOLONG : 0;
    return name;
}

static struct shm_name *shm_find_locked(const char *name)
{
    for (unsigned i = 0; i < SHM_MAX; i++)
        if (shm[i].name[0] && !strcmp(shm[i].name, name))
            return &shm[i];
    return NULL;
}

/* A new name in a free slot, with a new object: 0 or -errno. */
static long shm_create_locked(const char *name, struct shm_name **out)
{
    struct shm_name *s = NULL;
    for (unsigned i = 0; i < SHM_MAX && !s; i++)
        if (!shm[i].name[0])
            s = &shm[i];
    if (!s)
        return -ENFILE;
    long err = 0;
    struct memobj *m = memobj_new(name, true, &err);
    if (!m)
        return err;
    s->m = m;
    memcpy(s->name, name, strlen(name) + 1);
    *out = s;
    return 0;
}

/* The object called name for an open with flags, with a reference for
 * the caller: 0 or -errno. */
static long shm_lookup(const char *name, int flags, struct memobj **out)
{
    lock_take(&shm_lock);
    struct shm_name *s = shm_find_locked(name);
    long r = s && (flags & O_CREAT) && (flags & O_EXCL) ? -EEXIST :
             !s && !(flags & O_CREAT) ? -ENOENT : 0;
    if (!r && !s)
        r = shm_create_locked(name, &s);
    if (!r) {
        memobj_ref(s->m);
        *out = s->m;
    }
    lock_give(&shm_lock);
    return r;
}

static long shm_open_path(const char *name, int flags)
{
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    struct memobj *m;
    long r = shm_lookup(name, flags, &m);
    if (r)
        return r;
    if ((flags & (O_TRUNC | O_PATH)) == O_TRUNC && (flags & FD_ACCMODE) != O_RDONLY)
        r = memobj_truncate(m, 0);
    void *priv = r ? NULL : memfd_priv_new(m);
    memobj_unref(m);
    if (r || !priv)
        return r ? r : -ENOMEM;
    return kind_install(&memfd_ops, priv, (uint32_t)flags & OPEN_STATUS,
                        flags & O_CLOEXEC);
}

/* /dev/fd/N and its kin: a dup of N. */
static long dup_open(long fd, int flags)
{
    struct fd_desc *d = fd_get(fd);
    return d ? fd_install(d, 0, flags & O_CLOEXEC) : -ENOENT;
}

/* stat of /dev/tty: the process's terminal's, or ENXIO without one. */
static long tty_stat_of(struct stat *st)
{
    struct fd_desc *t = tty_controlling();
    if (!t)
        return -ENXIO;
    memset(st, 0, sizeof(*st));
    long r = t->ops->stat(t, st);
    fd_put(t);
    return r;
}

long dev_open(const char *abs, int flags, mode_t mode)
{
    long fd = fd_node(abs);
    if (fd >= 0)
        return dup_open(fd, flags);
    long err = 0;
    const char *name = shm_name_of(abs, &err);
    if (name)
        return err ? err : shm_open_path(name, flags);
    if (is_dir(abs))
        return (flags & FD_ACCMODE) != O_RDONLY ? -EISDIR : -ENOTSUP;   /* no listing */
    if (pty_path(abs))
        return pty_open(abs, flags);
    if (!strcmp(abs, "/dev/tty")) {
        struct fd_desc *t = tty_controlling();
        return t ? fd_install(t, 0, flags & O_CLOEXEC) : -ENXIO;
    }
    const struct node *n = node_named(abs + 5);
    if (!n)
        return under_node(abs) ? -ENOTDIR : (flags & O_CREAT) ? -EACCES : -ENOENT;
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    if ((flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;
    struct fd_desc *d = node_desc(n, (uint32_t)flags & OPEN_STATUS);
    return d ? fd_install(d, 0, flags & O_CLOEXEC) : -ENOMEM;
}

static long shm_stat(const char *name, struct stat *st)
{
    lock_take(&shm_lock);
    struct shm_name *s = shm_find_locked(name);
    uint64_t size = s ? memobj_size(s->m) : 0;
    lock_give(&shm_lock);
    if (!s)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_size = (off_t)size;
    st->st_uid = st->st_gid = 1000;
    st->st_blksize = 4096;
    return 0;
}

/* stat of /dev/fd/N: fstat of N. */
static long fd_stat(long fd, struct stat *st)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    long r = 0;
    if (d->ops->stat)
        r = d->ops->stat(d, st);
    else
        st->st_mode = S_IFCHR | 0600;   /* fd.h's default for a kind without one */
    fd_put(d);
    return r;
}

long dev_stat(const char *abs, struct stat *st)
{
    if (is_dir(abs)) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | (strcmp(abs, "/dev/shm") ? 0755 : 01777);
        st->st_nlink = 2;
        st->st_blksize = 4096;
        return 0;
    }
    long err = 0;
    const char *name = shm_name_of(abs, &err);
    if (name)
        return err ? err : shm_stat(name, st);
    if (pty_path(abs))
        return pty_stat(abs, st);
    if (fd_node(abs) >= 0)
        return fd_stat(fd_node(abs), st);
    const struct node *n = node_named(abs + 5);
    if (!n) {
        if (!strcmp(abs, "/dev/tty"))
            return tty_stat_of(st);
        return under_node(abs) ? -ENOTDIR : -ENOENT;
    }
    memset(st, 0, sizeof(*st));
    node_stat_of(n, st);
    return 0;
}

long dev_unlink(const char *abs)
{
    long err = 0;
    const char *name = shm_name_of(abs, &err);
    if (!name)
        return is_dir(abs) ? -EISDIR : -EACCES;   /* the nodes are fixed */
    if (err)
        return err;
    lock_take(&shm_lock);
    struct shm_name *s = shm_find_locked(name);
    struct memobj *m = s ? s->m : NULL;
    if (s)
        s->name[0] = '\0';
    lock_give(&shm_lock);
    if (!m)
        return -ENOENT;
    memobj_unref(m);
    return 0;
}
