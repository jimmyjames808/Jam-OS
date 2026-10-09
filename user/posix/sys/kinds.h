/* The descriptor kinds libc makes without a file server (docs/M13-PLAN.md
 * sections 3 and 4), each a struct fd_ops (fd.h) in its own file:
 *
 *   pipe.c     pipe ends and socketpair ends: a channel end, and the bytes
 *              read from it that no read has taken yet
 *   dev.c      the /dev nodes and /dev/shm's names
 *   memfd.c    memfd_create's and shm_open's memory files (kept VMOs)
 *   eventfd.c  eventfd's counter and its event
 *   etc.c      libc's own /etc files
 *
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
extern const struct fd_ops etc_ops;       /* a file of libc's /etc */

/* The hand-over's kinds (fd.c's fd_adopters): a child's descriptors of
 * kind POSIXFDS_PIPE, _DEV, _ETC, _MEMFD and _EVENTFD as descriptions. */
fd_adopt_fn pipe_adopt, dev_adopt, etc_adopt, memfd_adopt, eventfd_adopt;

/* A pipe end's description on the channel end ch (consumed, whatever
 * happens), one reference held; NULL if out of memory. */
struct fd_desc *pipe_desc_new(handle_t ch, uint32_t status);

/* No hand-over (docs/M13-PLAN.md section 5; dev.c): 0 is /dev/null; 1 and
 * 2 share one description, a pipe end on the SR_STDOUT channel if the
 * starter gave one, else the log (debug_write, as a native program's
 * printf without SR_STDOUT; it can't be read). A terminal (SR_CONSOLE)
 * becomes 0 to 2 once terminals have a POSIX side; until then such a
 * program writes to the log, as a native program in the foreground does.
 * Called by fd.c's start, after musl is up. */
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
/* F_ADD_SEALS and F_GET_SEALS (dup.c's fcntl): -EINVAL if d is no memfd. */
long memfd_add_seals(struct fd_desc *d, unsigned seals);
long memfd_get_seals(struct fd_desc *d);

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
