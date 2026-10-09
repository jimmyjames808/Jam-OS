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
 * installs it with fd_install (or fd_alloc and fd_set when the slot must
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

/* What a description gives a child (fd_give, ops->give): a hand-over
 * record's contents (<posixfds.h>). The handles are new ones (a
 * duplicate, or for a file a fresh open), the caller's to send or close;
 * data is malloc'd (or NULL), the caller's to free. fd_given_drop does
 * both. */
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
};

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
void            fd_set(long fd, struct fd_desc *d, bool cloexec);
/* Give back a reserved descriptor unused. */
void            fd_free(long fd);
/* fd_alloc then fd_set: the descriptor, or -errno (d's reference is
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
/* Copy n bytes into iov from src, skipping the first `skip` bytes of
 * iov; and out of iov into dst. They stop at iov's end. */
void   fd_iov_scatter(const struct iovec *iov, int iovcnt, size_t skip, const void *src,
                      size_t n);
void   fd_iov_gather(const struct iovec *iov, int iovcnt, size_t skip, void *dst, size_t n);
