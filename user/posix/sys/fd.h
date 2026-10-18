/* The descriptor table and open file descriptions (docs/M13-PLAN.md
 * section 3): what every kind of descriptor (a file, a directory, a pipe
 * end, a socket, a terminal, /dev/null, an eventfd, an epoll set, ...)
 * plugs into. fd.c holds the table and the calls that only route to a
 * description (read, write, close, lseek, fstat, ...); each kind is a
 * struct fd_ops in its own area's file.
 *
 * The model, as POSIX has it:
 *   - a **descriptor** is a slot of the table (0 .. FD_MAX - 1) pointing
 *     to a description, with FD_CLOEXEC, the slot's own flag;
 *   - an **open file description** (struct fd_desc) holds the kind, the
 *     status flags (O_APPEND, O_NONBLOCK, the access mode, ...), the
 *     offset and the kind's own state. dup and a hand-over's descriptors
 *     share one; it is refcounted and ends (ops->close) when the last
 *     slot and the last call using it let go.
 *
 * **Locks.** fd.c's table lock guards the slots: taken for a lookup, an
 * install or a close, never across a kind's operation. Each description
 * has its own lock (d->lock) for its offset and whatever of the kind's
 * state a call changes: a file holds it across a read or write, so two
 * threads sharing a description never interleave inside one call (POSIX
 * 2.9.7). A kind that blocks for long (a pipe waiting for a writer) holds
 * it only around its own bookkeeping, never across the wait. The order:
 * the table lock, then a description's lock; never two descriptions'
 * locks at once.
 *
 * **References.** fd_get gives the caller a reference, fd_put drops it:
 * a description is never freed under a call that is using it, even if
 * another thread closes the descriptor meanwhile (Linux's semantics too:
 * close doesn't interrupt a read in progress).
 *
 * **Who calls what.** A kind's file makes a description with fd_new and
 * installs it with fd_install (or fd_alloc and fd_place when the slot must
 * be reserved before a side effect: open with O_CREAT|O_EXCL, pipe2's two
 * ends). dup2 and dup3 use fd_replace. posix_spawn walks the table with
 * fd_next and asks each description what a child gets (fd_give). The
 * hand-over a program starts with (<posixfds.h>) is read before musl runs
 * (user/posix/crt/start.c, through the fd_stage_* calls below: no malloc
 * there) and made into descriptions by fd_adopters (fd.c) once the C
 * library is up, before any constructor of the program runs.
 *
 * The kinds libc makes without a server (pipes, /dev, /etc, memfd,
 * eventfd) and dup and fcntl are in kinds.h's files. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>
#include "layer.h"

/* <posixfds.h> includes <os.h> itself: after layer.h, which includes
 * <os.h> the way musl's headers need (<jamos/libjam.h>). */
#include <netwait.h>
#include <posixfds.h>

struct stat;

#define FD_MAX        POSIXFDS_FD_MAX   /* descriptors 0 .. FD_MAX - 1 (RLIMIT_NOFILE) */
#define FD_RW_MAX     0x7ffff000L       /* the most one read or write moves (Linux's
                                         * MAX_RW_COUNT); a longer one is cut to it */
#define FD_AT_OFFSET  (-1)              /* ops->read/write's pos: at the description's offset */

struct fd_desc;
struct fd_fork;

/* What a description gives a child (fd_give, ops->give): a hand-over
 * record's contents (<posixfds.h>). The handles are new ones (a
 * duplicate, or for a file a fresh open), the caller's to send or close;
 * data is from the layer's heap (or NULL), the caller's to jam_free.
 * fd_given_drop does both. */
struct fd_given {
    uint16_t kind;                              /* POSIXFDS_* */
    uint32_t status;                            /* the description's O_* status flags */
    uint64_t offset;                            /* a file's or directory's; else 0 */
    handle_t handles[POSIXFDS_KIND_HANDLES];    /* the kind's count of them, in its order */
    uint32_t nhandles;
    void    *data;                              /* data_len bytes (a string kind's with its NUL) */
    uint32_t data_len;
};

/* What poll, select and epoll wait on for a description (ops->arm; B5's,
 * over <netwait.h>): a handle and which of its signals mean readable,
 * writable and hung up, or a socket's rings, events and channel. */
enum fd_wait_kind {
    FD_WAIT_HANDLE = 1,   /* .h */
    FD_WAIT_SOCK = 2,     /* .sock */
};
struct fd_wait {
    enum fd_wait_kind kind;
    union {
        struct netwait_handle h;
        struct netwait_sock   sock;
    };
};

/* A kind's operations. Every hook runs with a reference to d held and no
 * lock of fd.c's taken, and returns Linux's way: a value, or -errno. A
 * NULL hook gives the answer Linux gives a kind without it (in
 * brackets). */
struct fd_ops {
    const char *name;   /* the kind, for log lines: "file", "pipe", ... */

    /* Read into, or write from, the iovcnt pieces of iov (1 .. IOV_MAX,
     * their total already checked: at most FD_RW_MAX), at pos, or at the
     * description's offset (moving it) for FD_AT_OFFSET. A kind with no
     * offset returns -ESPIPE for any other pos. A short count only at the
     * end of the file, or when a pipe or socket had less. The access mode
     * is checked before (EBADF). [read: -EINVAL; write: -EINVAL] */
    long (*read)(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos);
    long (*write)(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos);
    /* pwritev2 with RWF_NOAPPEND: write at pos (>= 0) even with O_APPEND
     * (musl's pwrite asks so). [write's] */
    long (*write_at)(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos);
    /* lseek: the new offset. [-ESPIPE] */
    long (*seek)(struct fd_desc *d, int64_t off, int whence);
    /* fstat into *st (zeroed by the caller). [a character device, 0600] */
    long (*stat)(struct fd_desc *d, struct stat *st);
    /* An ioctl the table doesn't answer itself (it does FIONBIO, FIOCLEX
     * and FIONCLEX). [-ENOTTY] */
    long (*ioctl)(struct fd_desc *d, unsigned long req, long arg);
    /* fsync and fdatasync. [-EINVAL] */
    long (*sync)(struct fd_desc *d);
    /* ftruncate to len (>= 0). [-EINVAL] */
    long (*truncate)(struct fd_desc *d, int64_t len);
    /* getdents64 into buf (len bytes): bytes filled, 0 at the end.
     * [-ENOTDIR] */
    long (*getdents)(struct fd_desc *d, void *buf, size_t len);
    /* The status flags are about to change from old to new (fcntl's
     * F_SETFL, ioctl's FIONBIO): 0 lets them (fd_status_set then stores
     * them), -errno refuses. [0] */
    long (*setfl)(struct fd_desc *d, uint32_t old, uint32_t new);
    /* The absolute path a file or directory was opened by (fstatfs,
     * fchdir, the *at calls); NULL for other kinds. Valid while the
     * reference is held. [NULL] */
    const char *(*path)(struct fd_desc *d);

    /* poll, select and epoll (B5): the readiness libc knows without the
     * kernel (NETWAIT_* bits for interest: a pipe's leftover bytes are
     * readable whatever its channel says). [nothing] */
    uint32_t (*ready)(struct fd_desc *d, uint32_t interest);
    /* What to wait on for interest. A NULL arm means always ready, for
     * reading and writing as the access mode allows (files, directories,
     * /dev, /etc, memfds). */
    long (*arm)(struct fd_desc *d, uint32_t interest, struct fd_wait *out);

    /* What a child gets (posix_spawn, B3): -ENOTSUP for a kind a child
     * never gets (the descriptor is left out, as FD_CLOEXEC would). [the
     * same] */
    long (*give)(struct fd_desc *d, struct fd_given *out);
    /* fork (docs/FORK-PLAN.md section 1.6): what a forked child's copy of
     * d needs. In the parent (fd_fork_child false) it puts the handles
     * the copy needs (fd_fork_put, fd_fork_dup); in the child, on the
     * copy, it takes them by the same subs (fd_fork_take) and makes the
     * copy the child's: the parent's handles in it replaced or forgotten,
     * never closed (they were the parent's), what the parent's side has
     * taken off a channel (a pipe's leftover) dropped, so nothing is read
     * twice. Both run with fork's locks held, the child's before any other
     * thread is there: neither allocates, frees or takes a lock (d's own
     * is made fresh by fd.c), and the child's half decides only from the
     * copy, so both halves put and take alike. Returns whether the child
     * keeps d (the parent's answer is ignored); one it doesn't keep is
     * left out of its table, its memory left in the child's heap
     * (nothing may free there until fork returns, and its close could
     * touch what isn't the child's: a ring's addresses). [kept as it is:
     * a kind that holds no handle] */
    bool (*fork)(struct fd_desc *d, struct fd_fork *f);
    /* The last reference went: release the kind's state (its handles, its
     * memory). d itself is freed by fd.c. [nothing] */
    void (*close)(struct fd_desc *d);
};

/* An open file description. Kinds keep their state behind priv (fd_new
 * takes it; ops->close frees it). */
struct fd_desc {
    const struct fd_ops *ops;
    void     *priv;     /* the kind's state */
    uint32_t  refs;     /* slots and calls using it; atomic */
    uint32_t  status;   /* O_* status flags: the access mode, O_APPEND, O_NONBLOCK, ...;
                         * atomic (fd_status, fd_status_set) */
    lock_t    lock;     /* the offset and the kind's per-call state (above) */
    int64_t   offset;   /* a file's or directory's position; lock */
    uint64_t  serial;   /* never 0, never given twice: poll's cache key (ready.h) */
    uint32_t  fork_gen; /* the last fork whose walk met it (fd_fork_first); written by
                         * fork alone, with the table's lock held */
};

/* What a kind's fork hook works with (fd.c makes it): the stream, and
 * which description's handles these are. */
struct fd_fork {
    struct lx_fork_io *io;
    uint32_t           item;   /* <posixfork.h>'s: the description's lowest descriptor */
    uint32_t           sub0;   /* added to each sub: 8 inside fd_fork_inner */
};

/* d's last reference is going: take it out of every epoll set, before
 * its kind closes its handles (epoll.c; fd_put calls it). */
void epoll_forget(struct fd_desc *d);

/* ---- descriptions -------------------------------------------------------------- */

/* A new description of kind ops with status flags status and the kind's
 * state priv, one reference held. NULL: no memory (priv stays the
 * caller's). */
struct fd_desc *fd_new(const struct fd_ops *ops, uint32_t status, void *priv);
/* Another reference to d. */
void            fd_ref(struct fd_desc *d);
/* Drop a reference; the last one closes it (ops->close) and frees it. */
void            fd_put(struct fd_desc *d);
/* The status flags now; and set the bits of mask to bits' (after
 * ops->setfl allows it: its -errno, or 0). */
uint32_t        fd_status(const struct fd_desc *d);
long            fd_status_set(struct fd_desc *d, uint32_t mask, uint32_t bits);
/* The access-mode bits of open flags and of a status: O_RDONLY, O_WRONLY or
 * O_RDWR. musl's O_ACCMODE is 03 | O_PATH (O_SEARCH is O_PATH), so a test
 * of the mode alone ("is it read-only?") masks with this: with O_ACCMODE
 * an O_PATH open reads as not read-only. */
#define FD_ACCMODE 03

/* d's access mode allows reading / writing (O_PATH allows neither). */
bool            fd_can_read(const struct fd_desc *d);
bool            fd_can_write(const struct fd_desc *d);

/* ---- the table ----------------------------------------------------------------- */

/* Descriptor fd's description with a reference (the caller fd_puts it),
 * or NULL (no such descriptor: EBADF). */
struct fd_desc *fd_get(long fd);
/* The lowest free descriptor >= min, reserved (nobody else gets it; it
 * reads as not open), or -EMFILE (none free), -EINVAL (min out of
 * range). */
long            fd_alloc(long min);
/* Put d in a descriptor fd_alloc reserved (d's reference goes to it). */
void            fd_place(long fd, struct fd_desc *d, bool cloexec);
/* Give back a reserved descriptor unused. */
void            fd_free(long fd);
/* fd_alloc then fd_place: the descriptor, or -errno (d's reference is
 * dropped then). d's reference is consumed either way. */
long            fd_install(struct fd_desc *d, long min, bool cloexec);
/* Put d at fd, closing what was there (dup2, dup3, the hand-over): fd,
 * or -EBADF (out of range), -EBUSY (fd is reserved by an open in
 * another thread: Linux's dup2 answer). d's reference is consumed either
 * way. */
long            fd_replace(long fd, struct fd_desc *d, bool cloexec);
/* Close fd: 0, or -EBADF. */
long            fd_close(long fd);
/* FD_CLOEXEC of fd: 0 or 1, or -EBADF; and set it. */
long            fd_cloexec(long fd);
long            fd_set_cloexec(long fd, bool on);
/* The first open descriptor >= from: its number, its description (a
 * reference, the caller's) and FD_CLOEXEC; -1 when there are no more. */
long            fd_next(long from, struct fd_desc **d, bool *cloexec);

/* What a child gets for descriptor fd (ops->give): 0 and *out, or -EBADF,
 * or the kind's -ENOTSUP (leave it out). */
long            fd_give(long fd, struct fd_given *out);

/* ---- fork (FL2) ------------------------------------------------------------------
 * For the kinds' fork hooks (ops->fork above). Subs are 0 .. 7. */

/* Which side runs: true in the child. */
bool            fd_fork_child(const struct fd_fork *f);
/* Parent: h (consumed), or a duplicate of h if it may be passed, as sub.
 * Child: what came as sub, or HANDLE_INVALID. */
void            fd_fork_put(struct fd_fork *f, unsigned sub, handle_t h);
void            fd_fork_dup(struct fd_fork *f, unsigned sub, handle_t h);
handle_t        fd_fork_take(struct fd_fork *f, unsigned sub);
/* Is this fork's walk meeting an object for the first time (*gen not yet
 * this fork's)? It is marked met. The same answer on both sides: an object
 * several descriptions share (a memfd's memory, a file's open) has its
 * step once, at its first description's. */
bool            fd_fork_first(const struct fd_fork *f, uint32_t *gen);
/* A description held inside d's state (a connection's socketpair end, a
 * pseudo-terminal master's bytes): its own fork hook, under d's item with
 * subs from 8; in the child its lock fresh and its one reference d's.
 * Whether the child keeps it. */
bool            fd_fork_inner(struct fd_fork *f, struct fd_desc *in);
/* The hook of a kind a child never gets (a netstack socket, an epoll set:
 * docs/FORK-PLAN.md section 1.6): absent in the child. */
bool            fd_fork_absent(struct fd_desc *d, struct fd_fork *f);
/* The walk's step for d met at item (fd_fork_walk's, and the terminal's:
 * tty.c): its hook the first time this fork meets it; whether the child
 * keeps it. In the child d's references start again from 0 at its first
 * meeting: each holder that keeps it adds its own. */
bool            fd_fork_met(struct lx_fork_io *io, struct fd_desc *d, uint32_t item);
/* Close a fd_given's handles and free its data. */
void            fd_given_drop(struct fd_given *g);

/* ---- the hand-over at the start -------------------------------------------------
 * start.c's sink (<posixfds.h> struct posixfds_sink) and its default,
 * before musl runs: they only record. fd.c's constructor makes the
 * descriptors once malloc works, through fd_adopters: one function per
 * POSIXFDS_* kind (NULL: a kind not built yet, whose descriptor is left
 * out, its handles closed). An adopter consumes hs whatever happens and
 * gives a description with one reference. */
status_t fd_stage(const struct posixfds_fd *rec, const void *data, const handle_t *hs);
void     fd_stage_reset(void);
/* No hand-over: descriptors 0 to 2 as section 5 says (stdfd_default,
 * dev.c). */
void     fd_stage_default(void);

typedef long fd_adopt_fn(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
                         struct fd_desc **out);
extern fd_adopt_fn *const fd_adopters[POSIXFDS_KIND_COUNT];

/* ---- helpers for kinds ----------------------------------------------------------- */

/* The total of iov's lengths, or -EINVAL (iovcnt out of 0 .. IOV_MAX, or
 * the total past SSIZE_MAX); cut to FD_RW_MAX. */
long   fd_iov_total(const struct iovec *iov, int iovcnt);
/* Copy n bytes into iov's buffers (the caller's) from src, skipping the
 * first `skip` bytes of iov; and out of them into dst. They stop at iov's
 * end. 0, or -EFAULT (a buffer the caller can't write or read: some bytes
 * may have been copied), or -EIO (a mapped file's page its pager failed:
 * ucopy.h). */
long   fd_iov_scatter(const struct iovec *iov, int iovcnt, size_t skip, const void *src,
                      size_t n);
long   fd_iov_gather(const struct iovec *iov, int iovcnt, size_t skip, void *dst, size_t n);
/* Can n bytes of iov's buffers from byte skip on be read (or written:
 * write; uc_readable, uc_writable): 0, or -EFAULT (-EIO: a mapped file's
 * page its pager failed, which write() returns as it is). A write's and a send's
 * bytes are checked so before any goes: kinds hand a single buffer to the
 * kernel as it is (a pipe's message), and Linux's EFAULT for a buffer
 * that can't be read is how Chromium and the sanitizers ask whether
 * memory can be (a write to a pipe). A read checks only what it is about
 * to write, where a kind writes the caller's buffer through code that
 * isn't ours (a socket's ring). */
long   fd_iov_check(const struct iovec *iov, int iovcnt, size_t skip, size_t n, bool write);
/* The caller's array of iovcnt iovecs at uiov, copied into ours: into
 * fast if they fit (FD_IOV_FAST, Linux's UIO_FASTIOV), else into an array
 * malloc gives. *out is the copy (0 iovecs: fast). 0, -EINVAL (iovcnt out
 * of 0 .. IOV_MAX), -EFAULT or -ENOMEM; fd_iov_done frees it. A handler
 * reads a program's iovecs only through this (the buffers they name only
 * through the two above). */
#define FD_IOV_FAST 8
long   fd_iov_in(const struct iovec *uiov, long iovcnt, struct iovec fast[FD_IOV_FAST],
                 struct iovec **out);
void   fd_iov_done(struct iovec *iov, struct iovec fast[FD_IOV_FAST]);
