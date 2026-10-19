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
 * protocol's own limit.
 *
 * **Directory descriptors** (docs/DIRHANDLE-PLAN.md section 4) on a mount
 * whose fs.statfs says FS_FEAT_DIRS (extfs) hold the directory's own `fs`
 * channel (fs.opendir): a relative path from one is sent on it as the
 * program wrote it, however deep the directory lies, and the service
 * walks it from there (".." up to the view's root, then from the mount
 * point in our own namespace, as a link out); a directory renamed under
 * one is followed. Anywhere else (FAT, the boot image, "/", /svc, libc's
 * trees, a service with no room for more: 2.6) a directory description is
 * its path as text, as before.
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
 * so a mount replaced meanwhile is seen at the next call; a path from a
 * directory channel takes a duplicate of that one instead (path_fs).
 *
 * **A path's reference.** A call that made a struct path (path_get,
 * path_set, path_of, path_in_dir, file_where_path) ends it with path_done
 * on every way out, whatever the answer was (docs/DIRHANDLE-PLAN.md 4.2):
 * `at` is the directory description a relative path started from, held
 * until then. A call that forgets it leaks the description and its
 * channel, one of the volume's 256: tools/checkpathdone.py (make check)
 * finds a function that makes a path and never ends one; one that makes a
 * path for its caller says "(path_done: the caller's)". */
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

/* A resolved path. path_get and path_set make one (they set `at`), and
 * whoever called them ends it with path_done: a copy (`struct path q = *p`)
 * only borrows `at` and is not ended. */
struct path {
    struct fd_desc *at;                 /* the directory channel's description a relative path
                                         * starts from, held (path_done drops it); NULL for a
                                         * path from "/" (a text base was joined into abs) */
    char            abs[PATH_BYTES + NS_NAME_MAX];
                                        /* absolute and clean: "/", "/data", "/data/a/b"; on a
                                         * mount with links clean up to the mount only. With
                                         * `at`: its mount, a '/' and the path as the program
                                         * wrote it from the directory ("/data/" for the
                                         * directory itself), which only the service walks:
                                         * abs is then no path from "/" (path_rel) */
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
 * -EBADF (dirfd isn't open), -ENOTDIR (dirfd isn't a directory). A
 * relative path is walked on from the base's text where it stands in
 * out->abs (the current directory's and a text description's are kept
 * clean); from a directory channel's description it is that description's
 * (`at`), the path as written. */
long path_get(long dirfd, const char *in, struct path *out);
/* Resolve an absolute path the layer made itself (a description's). */
long path_set(const char *abs, struct path *out);
/* A path from dir (a directory channel's description: one more reference
 * to it in out->at), rel (n bytes, as written; "" for the directory
 * itself). 0, or -ENAMETOOLONG. */
long path_in_dir(struct fd_desc *dir, const char *rel, size_t n, struct path *out);
/* The path description d stands for (ops->place, else the text of
 * ops->path): 0 and *out; 1 if its kind names none (a pipe, a device:
 * *out is then "/", which ends with nothing to drop); or -errno. */
long path_of(struct fd_desc *d, struct path *out);
/* End a path path_get or path_set made (even one they refused): drop what
 * `at` holds. Every call ends its paths on every way out. */
void path_done(struct path *p);
/* The path inside the mount as fs.idl wants it ("/" for its root; with
 * `at` the path as written from the directory, "" for itself), and its
 * length. */
const char *path_rel(const struct path *p, uint32_t *len);
/* A duplicate of the `fs` channel p goes to (the caller closes it): its
 * mount's, or with `at` its directory's (dir_channel). 0, or -ENOENT (no
 * such mount, or a directory that is gone). Only for PATH_MOUNT. */
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
 * mounts, whose paths are clean already. A path from a directory channel
 * (`at`) is kept as it is while it stays inside the mount (the service
 * walks it; its text from the view's root may be too long to write): only
 * a link out makes it a whole path from "/". 0 or -errno. */
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

/* p (path_get's) as a whole path from "/" if it starts from a directory
 * channel: that directory's text now (getcwd's question) joined with the
 * path as written; for the requests a directory channel doesn't take
 * (fs.watch: inotify_add_watch's relative path). 0, or -ENOENT (the
 * directory removed), -ENAMETOOLONG, -errno. */
long path_whole(struct path *p);
/* path_get from base (a directory description, or a /proc directory's;
 * NULL: "/") in place of a descriptor: posix_spawn's file actions start
 * from the child's directory, which is no descriptor of ours. */
long path_get_at(struct fd_desc *base, const char *in, struct path *out);
/* The current directory (docs/DIRHANDLE-PLAN.md 4.4): a directory
 * description (dir.c's kind) with a reference for the caller to fd_put,
 * or NULL for "/"; and set to d (a directory description: its reference
 * goes to the current directory, the old one's is dropped). */
struct fd_desc *path_cwd(void);
void            path_cwd_set(struct fd_desc *d);
/* fork's walk (fd.c's fd_fork_walk, after the terminal): the current
 * directory's description under POSIXFORK_CWD, met as a descriptor's is
 * (the same one as a descriptor's after fchdir: kept with it). */
void            path_fork_cwd(struct lx_fork_io *io);

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
/* Where a file description's file is, to open it again (a lent open taken
 * back, a child's own open): its absolute text, or for a file opened from
 * a directory channel its name in a directory description of the file's
 * own (dir_copy), whose joined text may be too long or stale (docs/
 * DIRHANDLE-PLAN.md 4.5). file.c keeps one per description. */
struct file_where {
    char           *path;   /* absolute; with dir a hint (log lines, /proc); NULL: not known */
    struct fd_desc *dir;    /* the directory's description, or NULL */
    char           *name;   /* the name in dir as it was written */
};
/* w as a path (path_done after): 0, or 1 when w names nothing (*out is
 * then "/", ending with nothing to drop), or -errno. (path_done: the
 * caller's) */
long file_where_path(const struct file_where *w, struct path *out);
/* o's open, made again if it was lent, from where the file is (o->lock
 * held): OK, or the service's answer (ERR_BUSY: a child still holds the
 * file after the wait for its close). Every use of o->jf calls it first. */
status_t fobj_back_locked(struct fobj *o, const struct file_where *where);
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
/* The record locks' way to a file's description (reclock.c): its open
 * here, where the file is (for fobj_back_locked), and the description's
 * lock owner and token (OFD and flock locks; 0 until the first such
 * lock), which d->lock guards. 0, or -ENOTSUP: not a file with an open
 * (another kind, O_PATH). */
struct file_lockref {
    struct fobj             *o;
    const struct file_where *path;
    uint64_t                *owner;
    handle_t                *token;
};
long file_lockref(struct fd_desc *d, struct file_lockref *out);
/* The record locks' close rule (reclock.c; fd.c's close and dup2 call it
 * for the description a descriptor let go of): the process's fcntl locks
 * on d's file dropped, if it holds any (Linux's: any close of the file
 * drops them). */
void lock_closing(struct fd_desc *d);
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
 * of an fd open for writing asks the service for the file's writable VMO
 * (file.map, FILE_MAP_WRITE: ext) whatever PROT says, so mprotect can add
 * write later as Linux allows (a stored page is the file's, seen by read
 * and every mapping, written back by msync, fsync or in time); the
 * service refusing it (fat: no pages written back) is ENODEV with
 * PROT_WRITE and, without, a read-only mapping that mprotect can't make
 * writable (EACCES); a file mapped to run is ETXTBSY. MAP_SHARED with
 * PROT_WRITE on an fd that can't write is EACCES, checked first. A file
 * with no pager (its service has none, or fat refuses one open for
 * writing) is mapped MAP_PRIVATE as a copy read now (a new VMO of len
 * bytes holding the file from off, zero past its end), MAP_SHARED ENODEV.
 * A memfd: MAP_PRIVATE a copy, MAP_SHARED its own VMO (B2's memfd_vmo) at
 * off. 0, or -EBADF, -EACCES (not open for reading), -ENODEV (no mapping
 * for its kind), -ETXTBSY, -ENOMEM, -EIO. */
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
/* The path a /proc directory's descriptor stands for ("/proc/<pid>/task"),
 * where the *at calls given it start (path.c): true and base written, or
 * false if d is no /proc directory. A name under it is looked up afresh
 * each time, as a path. */
bool proc_dir_base(struct fd_desc *d, char base[PATH_BYTES]);
/* A /proc file's path as it was opened ("/proc/<pid>/stat"), or NULL if
 * d is no /proc file (for log lines). */
const char *proc_file_path(struct fd_desc *d);
/* A /proc file as a socketpair's record (POSIXFDS_PROC: its path, the pid
 * in it, so the receiver opens the same process's file where its own /proc
 * has it: Chromium's browser gives each renderer its own statm and status);
 * -EINVAL for any other kind. A child's hand-over leaves /proc files out
 * (no .give). proc_adopt is the kind's adopter: our /proc's file at that
 * path, read only, or -errno (ENOENT for a process ours doesn't show). */
long proc_give(struct fd_desc *d, struct fd_given *out);
fd_adopt_fn proc_adopt;

/* ---- dir.c ------------------------------------------------------------------------ */

/* The directory kind: is d one, and a new one for p at position 0 (a
 * text description, p->abs; never a path from `at`). */
bool dir_is(const struct fd_desc *d);
long dir_new(const struct path *p, uint32_t status, struct fd_desc **out);
/* openat's directory found at p (p->nofollow: a link as the last name is
 * ELOOP) with open's status flags (O_PATH: no read asked): a description
 * of its own channel (fs.opendir) on a mount with FS_FEAT_DIRS, else, and
 * when the service has no room (ERR_NO_RESOURCES, said once a process),
 * a text one (p placed). -errno, or PATH_LINK_OUT. */
long dir_open(struct path *p, uint32_t status, struct fd_desc **out);
/* A relative path in (n bytes) from directory d: 1 and *out made from
 * d's channel (path_in_dir), or 0 and d's text in out->abs (a text
 * description), for path.c's path_get. */
long dir_from(struct fd_desc *d, const char *in, size_t n, struct path *out);
/* A directory channel description's mount ("/data"). */
const char *dir_mount(const struct fd_desc *d);
/* A duplicate of d's channel (path_fs): 0, or -ENOENT (gone). A channel
 * whose service went (ERR_PEER_CLOSED: the mount went, or a fresh start)
 * is replaced first by an open of the last known text, the same directory
 * (its id) or none; failing that d is gone, ENOENT from then on. */
long dir_channel(struct fd_desc *d, handle_t *out);
/* A description of d's directory of its own (its channel duplicated): a
 * file opened from d keeps it (file_where). NULL: no memory, or gone. */
struct fd_desc *dir_copy(struct fd_desc *d);
/* d's absolute text now (PATH_BYTES): a channel's asked of its service
 * (fs.resolve of ""), else, or failing that, the last known; 0 or -errno
 * (-ENAMETOOLONG: deeper than PATH_BYTES from "/"). spawn's, where a
 * child gets text (a narrowed one's directory, a relative program). */
long dir_text_now(struct fd_desc *d, char *out);
/* A rename (fs.rename's flags) or a link of a to b, two paths on one
 * mount whose `at` differ (two directory channels, or one and a whole
 * path, whose directory is opened as a channel through its mount first):
 * fs.rename_at or fs.link_at on a's directory with b's by its id, EXDEV
 * when their volumes differ or the service can't place b's from a's view
 * (b's still there through its own channel). 0 or -errno. */
long dir_meet(const struct path *a, const struct path *b, uint32_t flags, bool link);
/* The kind's adopters (fd_adopters' POSIXFDS_DIR and POSIXFDS_DIR_CHANNEL
 * lines: a path opened again, or the channel itself, its text a hint). */
fd_adopt_fn dir_adopt_fd;
fd_adopt_fn dir_adopt_ch;
/* The hand-over's current directory as a description: its channel
 * (consumed) and data (a POSIXFDS_DIR_CHANNEL's), or its path (cleaned).
 * 0 and *out, or -errno. */
long dir_adopt_cwd(handle_t ch, const void *data, uint32_t len, struct fd_desc **out);
long dir_new_text(const char *abs, struct fd_desc **out);
/* chdir's directory: in (the program's string) from base (path_get_at's)
 * opened as the current directory is kept: a channel's description
 * without FS_READ, or a text one (found to be a directory). 0 and *out,
 * or -errno. */
long dir_cwd_open(struct fd_desc *base, const char *in, struct fd_desc **out);
/* getcwd's question of directory d: its absolute text now into out
 * (PATH_BYTES; a channel's asked of its service, fs.resolve of ""). 0, or
 * -ENOENT (removed, or cut off from its view: Linux's getcwd says so),
 * -ENAMETOOLONG (deeper than PATH_BYTES from "/"), -errno. deleted: a
 * removed one's last known text with " (deleted)" instead of ENOENT, as
 * Linux's /proc links name one. */
long dir_where(struct fd_desc *d, char *out, bool deleted);
/* One getdents64 record (Linux's linux_dirent64, padded to 8 bytes) for
 * name at the caller's buf, d_off next: its length, 0 if it doesn't fit
 * in room bytes (nothing written then), or -EFAULT. */
long dir_put(void *buf, size_t room, const char *name, uint64_t ino, uint8_t type,
             int64_t next);
