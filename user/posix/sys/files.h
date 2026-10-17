/* Files and directories through the namespace (docs/M13-PLAN.md section
 * 3), what path.c, file.c, dir.c and meta.c share:
 *
 *   - path.c: a path as the layer resolves it (struct path): joined to the
 *     current directory or a directory descriptor's path, cleaned, and
 *     split into its mount and the path inside it; the current directory;
 *     the calls to a mount's `fs` service (abi/idl/fs.idl), whose statuses
 *     become errno values here (path_err: a refused change on a read-only
 *     mount is EROFS, a refused read EACCES); what stat says about a path;
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
 * **Mounts with links** (docs/M13.6-PLAN.md, "Paths and links": extfs,
 * whose fs.statfs says FS_FEAT_LINKS). Only the names up to the mount are
 * cleaned; the rest goes to the service as the program wrote it, and the
 * service resolves it, ".." physically and the links inside its view. A
 * link that leads out (an absolute one, or ".." above the mount's root)
 * comes back as ERR_LINK_OUT, which path_err turns into PATH_LINK_OUT: the
 * call asks fs.resolve where the path goes on (path_follow) and starts
 * again from there in our own namespace, "/" for a ".." above the mount
 * (so "/jam/.." is "/", as POSIX has it; FAT's mounts keep B1's rule),
 * the links counted to 40 (ELOOP). Every call that sends a path runs
 * `do r = op(&p); while (path_again(&p, &r));`. The current directory and
 * a directory descriptor's path are kept physical (path_place: links
 * followed, ".." resolved), so getcwd says where a chdir through a link
 * led. stat there is fs.getattr's (the inode's mode, links, owner and
 * times); a file's own open may be one of many writers.
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
    char            abs[PATH_BYTES];    /* absolute and clean: "/", "/data", "/data/a/b"; on a
                                         * mount with links clean up to the mount only */
    enum path_where where;
    uint32_t        mount_len;          /* abs[0 .. mount_len) is the mount ("/data") */
    bool            must_dir;           /* written with a trailing '/', "." or "..": only a
                                         * directory will do */
    bool            last_dot;           /* its last name as written was "." */
    bool            last_dotdot;        /* ... or ".." */
    bool            links;              /* the mount has symbolic links (FS_FEAT_LINKS): what
                                         * follows it in abs is as written, uncleaned */
    bool            nofollow;           /* a link as the last name is not followed (the
                                         * caller's to set: lstat, unlink, O_NOFOLLOW, ...) */
    uint8_t         hops;               /* links followed out of a mount (path_follow) */
};

/* A call's answer that a link led out of the mount (ERR_LINK_OUT): never
 * a program's errno; path_again follows it. */
#define PATH_LINK_OUT (-4000L)

/* What stat says about a path, as the service answers it. */
struct path_info {
    bool     is_dir;
    bool     is_svc;     /* a service under /svc: neither file nor directory */
    bool     is_link;    /* a symbolic link (p->nofollow, on a mount with links) */
    uint64_t size;       /* bytes; 0 for a directory */
    uint64_t mtime;      /* Unix seconds, 0 if unknown */
    uint64_t id;         /* the service's id (fs.stat), or the layer's own for "/" and /svc */
    bool     has_attr;   /* the rest is the inode's own (fs.getattr on a mount with modes);
                          * else path_fill_stat makes them up as B1 did */
    uint32_t mode, nlink, uid, gid;
    uint64_t blocks;     /* 512-byte units */
    int64_t  atime_ns, mtime_ns, ctime_ns;
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
/* What stat says about p (lstat's answer if p->nofollow): 0, or -errno,
 * or PATH_LINK_OUT. */
long path_info(const struct path *p, struct path_info *out);
/* The same, the links that lead out followed (p becomes where it led). */
long path_info_follow(struct path *p, struct path_info *out);
/* Is p's mount read-only (fs.statfs, remembered a second per mount)?
 * "/" and /svc are. */
bool path_read_only(const struct path *p);
/* p's mount's FS_FEAT_* (fs.statfs, remembered with the read-only flag);
 * 0 for "/", /svc and anything that isn't a mount. */
uint32_t path_features(const struct path *p);
/* errno for a mount's answer st to a call on p: ERR_ACCESS_DENIED is
 * EROFS on a read-only mount (else EACCES), ERR_LINK_OUT is PATH_LINK_OUT
 * (p not NULL), and the `fs` protocol's other errors as fs.idl lists
 * them. */
long path_err(const struct path *p, status_t st);
/* After PATH_LINK_OUT from a call on p: where the path goes on, asked of
 * p's mount (fs.resolve), resolved again into p from our own "/" (an
 * absolute link) or from "/" (".." above the mount's root). 0, or -ELOOP
 * past 40 links, or the service's answer. */
long path_follow(struct path *p);
/* *r was a call's answer on p: if it was PATH_LINK_OUT, follow it and
 * say whether to call again (*r then 0, or path_follow's error). */
static inline bool path_again(struct path *p, long *r)
{
    if (*r != PATH_LINK_OUT)
        return false;
    *r = path_follow(p);
    return *r == 0;
}
/* p in its physical form, on a mount with links: every link followed
 * (the last one too unless p->nofollow), ".." resolved, as fs.resolve
 * answers (links out followed in our namespace). Nothing for other
 * mounts, whose paths are clean already. 0 or -errno. */
long path_place(struct path *p);
/* The same for a call that changes nothing (stat, readdir, statfs, an
 * open to read): a refusal there is always EACCES, never EROFS. */
long path_err_read(const struct path *p, status_t st);
/* A program's file for spawn (abs: absolute and clean): its path with the
 * links followed into out (PATH_BYTES; abs itself on a mount without
 * links), so its approval is looked up where it leads (M13.6's "The
 * executable bit and approvals"). On a mount that keeps modes a file
 * without an execute bit is EACCES, before anything else. 0 or -errno. */
long path_program(const char *abs, char *out);
/* The permission bits that count for the one user (rwx as 4, 2, 1): a
 * file's owner bits if it is the one user's, else its others' (extfs
 * judges them the same way, M13.6's Q4). */
unsigned path_perm(uint32_t mode, uint32_t uid);
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
    bool         multi;              /* its mount lets a file have many writers
                                      * (FS_FEAT_MULTI_WRITER): never lent, its size the
                                      * service's, an append the service's (FS_APPEND);
                                      * or its mount isn't known (a file sent over a
                                      * socket): its size the service's, asked when needed */
    bool         append;             /* opened FS_APPEND (multi only): every write at the end;
                                      * shared only by descriptions with O_APPEND */
    bool         attrs;              /* its mount keeps modes (FS_FEAT_MODES): fstat is
                                      * file.getattr's */
    uint32_t     fork_gen;           /* the last fork whose walk met it (fobj_fork) */
};

/* The object for an open of p with ff (FS_READ, FS_WRITE, FS_CREATE,
 * FS_EXCL, FS_TRUNCATE, FS_NOFOLLOW; FS_APPEND, kept only on a mount with
 * many writers) and mode (the permission bits of a file FS_CREATE makes):
 * a new open, or ours shared (opened again to write if it must), one
 * reference to it in *out. -errno, -EISDIR for a directory, -EBUSY if
 * another process holds a writable open (FAT), PATH_LINK_OUT. */
long fobj_open(const struct path *p, uint32_t ff, uint32_t mode, struct fobj **out);
/* The object for an open another process gave us (consumed) of path
 * (NULL: not known) with ff (FS_APPEND as fobj_open's). */
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
/* fork's walk in the child (docs/FORK-PLAN.md section 1.6). fobj_fork: o,
 * which a file description the child keeps holds, counted again from its
 * first description (one reference each), its lock fresh and its open
 * lent (forgotten, never closed: the parent's), so its next use opens the
 * file again (fobj_back_locked). fobj_forked, after the descriptors: the
 * objects no description of the child's holds leave the list, unclosed;
 * objs_lock fresh. */
void     fobj_fork(struct fd_fork *f, struct fobj *o);
void     fobj_forked(struct lx_fork_io *io);
/* fs.open of p with ff, tried again for a while if the service answers
 * ERR_BUSY (our own close, or an ended child's, may not have reached it
 * yet): OK and *ch, *buf, or the last answer. */
status_t fobj_open_settled(const struct path *p, uint32_t ff, handle_t *ch, handle_t *buf);

/* ---- file.c ----------------------------------------------------------------------- */

/* The kind's adopter (fd_adopters' POSIXFDS_FILE line). */
fd_adopt_fn file_adopt_fd;
/* Is d a file's description (fdpass.c: which kinds a socket carries)? */
bool file_desc_is(const struct fd_desc *d);
/* file.setattr (FS_ATTR_* in mask) on the open of d, a file's description
 * on a mount that keeps modes: 0, -errno, or -ENOTSUP when d has no such
 * open (another kind, O_PATH, a mount that keeps nothing). */
long file_setattr_fd(struct fd_desc *d, uint32_t mask, uint32_t mode, uint32_t uid, uint32_t gid,
                     int64_t atime_ns, int64_t mtime_ns);
/* mmap of a descriptor (mem.c): what to map. */
#define FMAP_SHARED (1u << 0)   /* MAP_SHARED (else MAP_PRIVATE) */
#define FMAP_WRITE  (1u << 1)   /* PROT_WRITE asked for */
struct file_mapping {
    handle_t vmo;       /* the VMO to map: the caller's, closed by it */
    uint64_t vmo_off;   /* where in it the mapping starts */
    bool     pager_ro;  /* a private read-only mapping straight on the file's own pages
                         * (its service's VMO, never writable): mprotect adding write
                         * must put a copy-on-write child in its place */
};
/* mmap of descriptor fd from off, len bytes (whole pages), how FMAP_*.
 * A file whose mount's service is a pager (file.map: fat) is mapped on its
 * own pages, fetched as they are touched: MAP_SHARED, and MAP_PRIVATE
 * read-only, map the file's one shared VMO itself, so they see what the
 * file holds (a write through the file is seen once the service keeps
 * mappings coherent); MAP_PRIVATE writable maps a copy-on-write child of
 * it (vmo_create_child, AT_LEAST_ON_WRITE: a page is the file's until
 * first touched, then the mapping's own), charged in full now. MAP_SHARED
 * with PROT_WRITE is ENODEV (EACCES if fd can't write): the pager never
 * writes pages back. A file with no pager (its service has none, or fat
 * refuses one open for writing) is mapped MAP_PRIVATE as a copy read now
 * (a new VMO of len bytes holding the file from off, zero past its end),
 * MAP_SHARED ENODEV. A memfd: MAP_PRIVATE a copy, MAP_SHARED its own VMO
 * (B2's memfd_vmo) at off. 0, or -EBADF, -EACCES (not open for reading),
 * -ENODEV (no mapping for its kind), -ENOMEM, -EIO. */
long file_map(long fd, int64_t off, uint64_t len, uint32_t how, struct file_mapping *out);

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
/* Is abs (clean) /proc/self/exe, or /proc/<our pid>/exe? */
bool proc_is_exe(const char *abs);

/* ---- dir.c ------------------------------------------------------------------------ */

/* The directory kind: is d one, and a new one for p at position 0. */
bool dir_is(const struct fd_desc *d);
long dir_new(const struct path *p, uint32_t status, struct fd_desc **out);
/* The kind's adopter (fd_adopters' POSIXFDS_DIR line). */
fd_adopt_fn dir_adopt_fd;
/* One getdents64 record (Linux's linux_dirent64, padded to 8 bytes) for
 * name at the caller's buf, d_off next: its length, 0 if it doesn't fit
 * in room bytes (nothing written then), or -EFAULT. */
long dir_put(void *buf, size_t room, const char *name, uint64_t ino, uint8_t type,
             int64_t next);
