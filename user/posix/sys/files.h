/* Files and directories through the namespace (docs/M13-PLAN.md section
 * 3), what path.c, file.c, dir.c and meta.c share:
 *
 *   - path.c: a path as the layer resolves it (struct path): joined to the
 *     current directory or a directory descriptor's path, cleaned, and
 *     split into its mount and the path inside it; the current directory;
 *     the calls to a mount's `fs` service (abi/idl/fs.idl), whose statuses
 *     become errno values here (path_err: a refusal on a read-only mount
 *     is EROFS); what stat says about a path;
 *   - file.c: the file kind and open;
 *   - dir.c: the directory kind (getdents64), mkdir, unlink, rmdir,
 *     rename, chdir and getcwd;
 *   - meta.c: the stat family, statfs, access and the calls FAT has no
 *     answer for (chmod, link, readlink, ...).
 *
 * **Paths.** libc keeps the current directory as an absolute path. A path
 * is joined to it (or to a directory descriptor's path, for the *at
 * calls), and cleaned as libos's namespace does: "." and ".." before the
 * first name stay at "/", the first name is the mount ("/data", or "svc"
 * for the services), and ".." stops at the mount's root ("/data/../boot"
 * is /data/boot). "/" itself is a read-only directory listing the mounts
 * and "svc"; "/svc" lists the services, each a socket-like node that
 * can't be opened. Paths are up to PATH_BYTES with their NUL, the `fs`
 * protocol's own limit. Directory descriptors are paths too: a directory
 * renamed under one is not followed (FAT has no inode to follow).
 *
 * **The mount's channel.** Each call asks libjam's namespace for a
 * duplicate of the mount's `fs` channel (ns_channel) and closes it after,
 * so a mount replaced meanwhile is seen at the next call. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include "fd.h"
#include "kinds.h"

#define PATH_BYTES   4096u                 /* a path with its NUL (Linux's PATH_MAX) */
#define FILE_TIMEOUT FS_CALL_TIMEOUT       /* every call to a mount: libos's limit */

enum path_where {
    PATH_ROOT = 1,   /* "/" */
    PATH_SVC,        /* "/svc" or "/svc/<name>" */
    PATH_MOUNT,      /* inside a mount (its root included) */
};

/* A resolved path. */
struct path {
    char            abs[PATH_BYTES];    /* absolute and clean: "/", "/data", "/data/a/b" */
    enum path_where where;
    uint32_t        mount_len;          /* abs[0 .. mount_len) is the mount ("/data") */
    bool            must_dir;           /* written with a trailing '/', "." or "..": only a
                                         * directory will do */
    bool            last_dot;           /* its last name as written was "." */
    bool            last_dotdot;        /* ... or ".." */
};

/* What stat says about a path, as the service answers it. */
struct path_info {
    bool     is_dir;
    bool     is_svc;     /* a service under /svc: neither file nor directory */
    uint64_t size;       /* bytes; 0 for a directory */
    uint64_t mtime;      /* Unix seconds, 0 if unknown */
    uint64_t id;         /* the service's id (fs.stat), or the layer's own for "/" and /svc */
};

/* ---- path.c ----------------------------------------------------------------------- */

/* Resolve `in` (a C string from the program) against dirfd (AT_FDCWD:
 * the current directory; else a directory descriptor, needed only when
 * in is relative). 0, or -EFAULT (NULL), -ENOENT (empty), -ENAMETOOLONG,
 * -EBADF (dirfd isn't open), -ENOTDIR (dirfd isn't a directory). */
long path_get(long dirfd, const char *in, struct path *out);
/* Resolve an absolute path the layer made itself (a description's). */
long path_set(const char *abs, struct path *out);
/* The path inside the mount as fs.idl wants it ("/" for its root), and
 * its length. */
const char *path_rel(const struct path *p, uint32_t *len);
/* A duplicate of p's mount's `fs` channel (the caller closes it): 0, or
 * -ENOENT (no such mount). Only for PATH_MOUNT. */
long path_fs(const struct path *p, handle_t *fs);
/* What stat says about p: 0, or -errno. */
long path_info(const struct path *p, struct path_info *out);
/* Is p's mount read-only (fs.statfs, remembered a second per mount)?
 * "/" and /svc are. */
bool path_read_only(const struct path *p);
/* errno for a mount's answer st to a call on p: ERR_ACCESS_DENIED is
 * EROFS on a read-only mount (else EACCES), and the `fs` protocol's
 * other errors as fs.idl lists them. */
long path_err(const struct path *p, status_t st);
/* *st for p as path_info found it (fstat's and stat's shared part). */
void path_fill_stat(const struct path *p, const struct path_info *i, struct stat *st);

/* The current directory: copied into out (PATH_BYTES); and set (an
 * absolute clean path, already checked to be a directory). */
void path_cwd(char out[PATH_BYTES]);
void path_cwd_set(const char *abs);

/* ---- fobj.c: a file's one open in this process -------------------------------- */

/* Every description of one file in this process shares this (fobj.c says
 * why: fat's one writable open). */
struct fobj {
    struct fobj *next;               /* fobj.c's list; its lock */
    unsigned     refs;               /* the descriptions using it; fobj.c's lock */
    char         mount[NS_NAME_MAX]; /* its mount ("/data"; "" if not known: never shared) */
    uint64_t     id;                 /* fs.stat's id there */
    lock_t       lock;               /* jf and size: held across each transfer */
    struct jfile jf;                 /* the open: FS_READ, or FS_READ | FS_WRITE; jf.ch 0
                                      * if it was lost (EIO) */
    uint64_t     size;               /* the file's size, kept by our writes when jf may write */
    bool         lent;               /* our writable open was closed for a child's: jf is
                                      * empty until fobj_back_locked opens it again; lock */
    uint32_t     lent_flags;         /* ... with these FS_* flags */
    uint64_t     lent_end;           /* the file's end when lent, or when last looked at
                                      * since: a description there follows the end (file.c) */
};

/* The object for an open of p with ff (FS_READ, FS_WRITE, FS_CREATE,
 * FS_EXCL, FS_TRUNCATE): a new open, or ours shared (opened again to
 * write if it must), one reference to it in *out. -errno, -EISDIR for a
 * directory, -EBUSY if another process holds a writable open. */
long fobj_open(const struct path *p, uint32_t ff, struct fobj **out);
/* The object for an open another process gave us (consumed) of path
 * (NULL: not known) with ff. */
long fobj_adopt(handle_t ch, handle_t buf, uint32_t ff, const char *path, struct fobj **out);
/* Drop a reference; the last closes the open. */
void fobj_put(struct fobj *o);
/* A child is about to open o's file for writing (a description given to
 * it): our writable open, the file's one, is closed so the child's can
 * be made (o->lock held). Nothing for an open that only reads. */
void fobj_lend_locked(struct fobj *o);
/* o's open, made again if it was lent, from the file's absolute path
 * (o->lock held): OK, or the service's answer (ERR_BUSY: a child still
 * holds the file after the wait for its close). Every use of o->jf
 * calls it first. */
status_t fobj_back_locked(struct fobj *o, const char *path);
/* fs.open of p with ff, tried again for a while if the service answers
 * ERR_BUSY (our own close, or an ended child's, may not have reached it
 * yet): OK and *ch, *buf, or the last answer. */
status_t fobj_open_settled(const struct path *p, uint32_t ff, handle_t *ch, handle_t *buf);

/* ---- file.c ----------------------------------------------------------------------- */

/* The kind's adopter (fd_adopters' POSIXFDS_FILE line). */
fd_adopt_fn file_adopt_fd;
/* mmap of descriptor fd (mem.c): the VMO to map (the caller's, closed by
 * it) and where in it the mapping starts. MAP_PRIVATE is a copy: a new
 * VMO of len bytes (whole pages) holding the file from off, zero past its
 * end (a file, or a memfd). MAP_SHARED is a memfd's own VMO (B2's
 * memfd_vmo) at off; of a file it is ENODEV until M12.5's pager. 0, or
 * -EBADF, -EACCES (not open for reading), -ENODEV (no mapping for its
 * kind), -ENOMEM, -EIO. */
long file_map(long fd, int64_t off, uint64_t len, bool shared, handle_t *vmo, uint64_t *vmo_off);

/* ---- libc's own trees: /dev, /etc and /proc ------------------------------------
 * A clean absolute path under them is kinds.h's (dev_path, etc_path) or
 * procfs.c's (proc_path): open, stat and unlink go there (readlink and
 * lstat too for /proc), and any other change there is refused here
 * (EACCES under /dev and /proc, EROFS under /etc). */
enum path_libc { LIBC_NONE, LIBC_DEV, LIBC_ETC, LIBC_PROC };
/* Is p under /dev, /etc or /proc as libc serves them? */
enum path_libc path_libc(const struct path *p);
/* stat of such a path (a /proc link followed); and the errno for a
 * change other than B2's. */
long path_libc_stat(const struct path *p, enum path_libc which, struct stat *st);
long path_libc_refuse(enum path_libc which);
/* Is m ("/etc") a mount of the namespace? Then the tree is the mount's. */
bool path_is_mount(const char *m);

/* ---- procfs.c: libc's /proc --------------------------------------------------------
 * What a Linux program reads about itself there (docs/CR-PLAN.md section
 * 3): /proc/self (a link to /proc/<our pid>, the one pid served), its exe
 * (a link to the program's file) and its fd (a directory of links, one
 * per open descriptor); every other path under /proc is ENOENT. */
/* Is abs (clean, absolute) /proc or under it, and libc's? */
bool proc_path(const char *abs);
/* open of such a path with open's flags: the new descriptor or -errno.
 * The links are followed: exe opens the program's file, fd/N opens N's
 * file again (a new description: its own offset and access mode). */
long proc_open(const struct path *p, long flags);
/* stat (follow: the link's target, as stat) or lstat (a link is a link)
 * of such a path into *st (zeroed here): 0 or -errno. */
long proc_stat(const struct path *p, bool follow, struct stat *st);
/* readlink of such a path: the link's bytes copied (at most size, no
 * NUL), -EINVAL for a directory, -ENOENT for anything not there. */
long proc_readlink(const struct path *p, char *buf, size_t size);
/* The program's own file, absolute ("/boot/bin/x" for a boot program),
 * as /proc/self/exe names it; NULL if it can't be named. The first call
 * finds it (procfs.c says how), the answer is kept for the process. */
const char *proc_exe(void);

/* ---- dir.c ------------------------------------------------------------------------ */

/* The directory kind: is d one, and a new one for p at position 0. */
bool dir_is(const struct fd_desc *d);
long dir_new(const struct path *p, uint32_t status, struct fd_desc **out);
/* The kind's adopter (fd_adopters' POSIXFDS_DIR line). */
fd_adopt_fn dir_adopt_fd;
/* One getdents64 record (Linux's linux_dirent64, padded to 8 bytes) for
 * name at buf, d_off next: its length, or 0 if it doesn't fit in room
 * bytes (nothing written then). */
size_t dir_put(void *buf, size_t room, const char *name, uint64_t ino, uint8_t type,
               int64_t next);
