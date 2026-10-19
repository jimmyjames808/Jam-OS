/* The descriptor kinds libc makes without a file server (docs/M13-PLAN.md
 * sections 3 and 4), each a struct fd_ops (fd.h) in its own file:
 *
 *   pipe.c     pipe ends and socketpair ends: a channel end, and the bytes
 *              read from it that no read has taken yet (pipe.h)
 *   sockpair.c a socketpair end's own calls: the descriptors it carries
 *              (SCM_RIGHTS), SOCK_SEQPACKET, shutdown
 *   dev.c      the /dev nodes and /dev/shm's names
 *   memfd.c    memfd_create's and shm_open's memory files (kept VMOs)
 *   eventfd.c  eventfd's counter and its event
 *   inotify.c  inotify's watches (the file services keep them) and its bell
 *   etc.c      libc's own /etc files
 *   svcsock.c  AF_UNIX stream sockets: connected to a service socket
 *              (/svc/wayland), each with its framing (wlsock.c's JWL2),
 *              or named (bind, listen, accept, connect through the
 *              broker /svc/unix: unixsock.c); svcsock.h
 *
 * and fdpass.c, the descriptors such a socket carries (fdpass.h);
 * and dup.c, the calls on descriptors themselves (dup, dup2, dup3,
 * fcntl). A kind keeps its state behind the description's priv and its
 * per-call state under the description's lock (fd.h's rules: never
 * across a wait).
 *
 * The /dev and /etc paths are libc's own, so the path calls (fd.c,
 * file.c, dir.c) ask these files first, with a clean absolute path. */
#pragma once

#include <sys/stat.h>
#include "fd.h"

/* ---- the kinds ---------------------------------------------------------- */

extern const struct fd_ops pipe_ops;      /* a pipe end */
extern const struct fd_ops sockp_ops;     /* a socketpair end */
extern const struct fd_ops dev_ops;       /* /dev/null, /dev/zero, ... */
extern const struct fd_ops memfd_ops;     /* memfd_create, shm_open */
extern const struct fd_ops eventfd_ops;   /* eventfd */
extern const struct fd_ops inotify_ops;   /* inotify_init1 */

/* inotify's limits (docs/INOTIFY-PLAN.md 3.5): what inotify.c enforces and
 * /proc/sys/fs/inotify says. The services' own are the bounds
 * (user/include/fswatch.h): 1,024 watches from one process on a volume,
 * all its channels (FSW_PROCESS_WATCHES), and 8 channels from one process
 * on a volume (FSW_PER_PROCESS), so a process holds no more watches than
 * that, all its descriptors (Linux's max_user_watches is a user's, all
 * theirs), and no more descriptors. A service queues 16 KiB of records
 * (FSWATCH_QUEUE_BYTES: 14 bytes and the name each) for a descriptor
 * on its mount before an IN_Q_OVERFLOW: 512 events whose names are 18
 * bytes (shorter ones fit more, 255-byte ones 60). */
#define INOTIFY_MAX_WATCHES    1024u   /* most watches one process holds, all descriptors */
#define INOTIFY_MAX_INSTANCES  8u      /* most descriptors a process holds */
#define INOTIFY_MAX_QUEUED     512u    /* events queued for a mount, with that name */
extern const struct fd_ops etc_ops;       /* a file of libc's /etc */
extern const struct fd_ops usock_ops;     /* an AF_UNIX stream socket (svcsock.c) */

/* The hand-over's kinds (fd.c's fd_adopters): a child's descriptors of
 * kind POSIXFDS_PIPE, _SEQPACKET, _DEV, _ETC, _MEMFD and _EVENTFD as
 * descriptions. */
fd_adopt_fn pipe_adopt, seqp_adopt, dev_adopt, etc_adopt, memfd_adopt, eventfd_adopt;

/* A pipe or socketpair end as fdpass.c sends it: a duplicate of its
 * channel end (POSIXFDS_PIPE, or _SEQPACKET), its status, no data (the
 * leftover stays with this description). */
long pipe_pass(struct fd_desc *d, struct fd_given *out);
/* A pipe end's description on the channel end ch (consumed, whatever
 * happens), one reference held; NULL if out of memory. */
struct fd_desc *pipe_desc_new(handle_t ch, uint32_t status);
/* The channel end of a pipe end or a socketpair end: the description's
 * own (valid while the caller's reference is, until a shutdown), not a
 * duplicate. HANDLE_INVALID for another kind. */
handle_t pipe_channel(struct fd_desc *d);

/* ---- socketpair ends (sockpair.c) ------------------------------------------ */

struct msghdr;

/* sendmsg and recvmsg on a socketpair end (sockio.c's calls, after their
 * flags are checked): Linux's answers, SCM_RIGHTS and all. send, write,
 * recv and read are these without control data. */
long sockp_sendmsg(struct fd_desc *d, const struct msghdr *m, int flags);
long sockp_recvmsg(struct fd_desc *d, struct msghdr *m, int flags);
/* shutdown (sockconn.c's): SHUT_RDWR closes the end for every descriptor
 * of the description, and wakes every thread asleep on it (a read, a
 * write, poll, epoll_wait); SHUT_RD and SHUT_WR alone are EOPNOTSUPP. */
long sockp_shutdown(struct fd_desc *d, int how);
/* SOCK_STREAM or SOCK_SEQPACKET (getsockopt's SO_TYPE). */
int  sockp_type(struct fd_desc *d);
/* SO_PASSCRED (sockopt.c's): with it on, every recvmsg that takes a
 * message's bytes gets SCM_CREDENTIALS, its writer's. */
void sockp_set_passcred(struct fd_desc *d, bool on);
bool sockp_passcred(struct fd_desc *d);

/* No hand-over (docs/M13-PLAN.md section 5; dev.c): 0 is /dev/null; 1 and
 * 2 share one description, a pipe end on the SR_STDOUT channel if the
 * starter gave one, else the log (debug_write, as a native program's
 * printf without SR_STDOUT; it can't be read). A console channel
 * (SR_CONSOLE) makes its terminal 0 and 2, and 1 too without SR_STDOUT
 * (tty.c's tty_stdfds). Called by fd.c's start, after musl is up. */
void stdfd_default(void);

/* ---- /dev and /etc paths ------------------------------------------------- */

/* Is abs (a clean absolute path) /dev or under it? */
bool dev_path(const char *abs);
/* open of a /dev path: the new descriptor (O_CLOEXEC from flags), or
 * -errno. /dev/stdin, /dev/stdout, /dev/stderr and /dev/fd/N are a dup. */
long dev_open(const char *abs, int flags, mode_t mode);
/* stat of a /dev path into *st; -errno. */
long dev_stat(const char *abs, struct stat *st);
/* unlink of a /dev path (a /dev/shm name: shm_unlink); -errno. */
long dev_unlink(const char *abs);

/* fork's walk, in the child (fd.c's fd_fork_walk, after the descriptors):
 * /dev/shm's names kept for the objects the child got with a descriptor
 * (their memory's handle came then), the rest dropped unclosed: a name
 * holds no handle of its own to pass. shm_lock fresh. */
void shm_forked(struct lx_fork_io *io);

/* Is abs /etc or under it, and libc's (the namespace has no /etc mount)? */
bool etc_path(const char *abs);
/* open and stat of an /etc path, as dev_open and dev_stat. Every file is
 * read only: writing, creating or removing is EROFS. */
long etc_open(const char *abs, int flags);
long etc_stat(const char *abs, struct stat *st);

/* ---- memfd for mmap ------------------------------------------------------- */

/* The VMO behind a memfd description (a duplicate, the caller's; write
 * rights only if the description may write) and the file's size, for a
 * MAP_SHARED mapping. -ENODEV if d is no memfd. */
long memfd_vmo(struct fd_desc *d, handle_t *vmo, uint64_t *size);
/* A memfd's description on vmo (consumed, whatever happens) with status
 * flags status, one reference held: the file's size the VMO's, F_SEAL_SEAL
 * set (who made it, and its seals, are unknown here). 0; -EINVAL: not a
 * VMO; -ENOMEM. */
long memfd_from_vmo(handle_t vmo, uint32_t status, struct fd_desc **out);
/* F_ADD_SEALS and F_GET_SEALS (dup.c's fcntl): -EINVAL if d is no memfd. */
long memfd_add_seals(struct fd_desc *d, unsigned seals);
long memfd_get_seals(struct fd_desc *d);

/* ---- record locks (reclock.c) ----------------------------------------------- */

/* fcntl's lock commands (F_GETLK, F_SETLK, F_SETLKW and the F_OFD_ ones)
 * on d, arg the caller's struct flock; and flock's op (LOCK_*). */
long lock_fcntl(struct fd_desc *d, long cmd, long arg);
long lock_flock(struct fd_desc *d, long op);

/* ---- for the kinds' files ------------------------------------------------- */

/* The status flags an open of a /dev or /etc path keeps from its flags. */
#define OPEN_STATUS (O_ACCMODE | O_APPEND | O_NONBLOCK)

/* A description of ops on priv with status flags status, installed at the
 * lowest free descriptor: the descriptor, or -errno (priv is closed with
 * ops->close then, so the caller never frees it after the call). */
long kind_install(const struct fd_ops *ops, void *priv, uint32_t status, bool cloexec);
/* The descriptions' given data: a malloc'd copy of n bytes of src, or
 * NULL for n 0 (-ENOMEM: *err). */
void *kind_data(const void *src, size_t n, long *err);
