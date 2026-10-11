/* /proc (docs/CR-PLAN.md section 3): libc's own, like /dev and /etc, with
 * nothing in the namespace behind it; the few names a Linux program reads
 * about itself, and ENOENT for every other one, never a half-filled file.
 *
 *   /proc                 a directory: self, and our pid
 *   /proc/self            a link to <pid>; /proc/<pid> is the same
 *                         directory for our own pid only (any other pid is
 *                         ENOENT: a process sees only itself here)
 *   /proc/self/exe        a link to the program's file: readlink gives its
 *                         path, open opens that file for reading (ETXTBSY
 *                         for writing, as Linux), stat stats it
 *   /proc/self/fd         a directory of our open descriptors, listed by
 *                         number as the table is when each getdents runs
 *   /proc/self/fd/N       a link to what descriptor N is: a file's or a
 *                         directory's path, /dev/... for the devices,
 *                         /etc/... for libc's files, /memfd:NAME (deleted),
 *                         pipe:[I], socket:[I] or anon_inode:[KIND] (I: the
 *                         description's serial). open makes a new
 *                         description of the same thing (a path opened again;
 *                         a memfd's object with the access asked, so
 *                         O_RDONLY is a twin that can't be written or mapped
 *                         writable, here or in a child; a pipe end shared
 *                         when the access fits), ENXIO for a socket or an
 *                         anonymous kind
 *
 * Which file the program is. Every POSIX program maps its ELF and program
 * headers (user/posix/posix.ld), so the file can be checked against the
 * process itself: the headers and every segment that isn't writable, byte
 * for byte. The first look tries the file argv[0] names (the constructor's
 * copy: the shell gives "bin/x", posix_spawn the path it was given, and a
 * program from /data its absolute path), then every file of the boot image;
 * the first that is this program, its path is the answer (a boot program's
 * as /boot/<its name>), kept for the process's life. A file that only
 * claims the name is never taken. A program whose file can't be named this
 * way (one from /data started without its path in argv[0]) has no exe:
 * ENOENT.
 *
 * Not built: names under a directory's link (/proc/self/fd/N/x: ENOTDIR),
 * O_PATH | O_NOFOLLOW of a link (the link is followed), statfs's
 * PROC_SUPER_MAGIC, and any change under /proc (EACCES, files.h's
 * path_libc_refuse). Locks: exe_lock for the answer above; a /proc/self/fd
 * listing takes the descriptor table's lock (fd_next) with no description's
 * lock held, fd.h's order. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <jam/bootfs.h>
#include <string.h>
#include <sys/sysmacros.h>
#include "files.h"
#include "memfd.h"
#include "proc.h"

#define PROC_DIR  "/proc"
#define INO_BASE  0x7000000000ull       /* our nodes' st_ino: clear of every mount's ids */
#define INO_FD    (INO_BASE + 0x100)    /* + N: /proc/self/fd/N */
#define INO_ROOT  1u                    /* "/"'s (path.c's): /proc's ".." */
#define PH_MAX    16u                   /* program headers we look at */
#define CHUNK     16384u                /* a file compared this many bytes at a time */
#define NAME_LEN  24u                   /* a listing's names: numbers, "self", "exe", "fd" */
#define ONE_USER  1000u

enum node {
    N_ROOT = 1,   /* /proc */
    N_SELF,       /* /proc/self, /proc/<pid>: the directory */
    N_SELFLINK,   /* /proc/self as a link (only lstat and readlink see it) */
    N_EXE,        /* /proc/self/exe */
    N_FDDIR,      /* /proc/self/fd */
    N_FD,         /* /proc/self/fd/N */
};

/* What a /proc path is. */
struct at {
    enum node node;
    bool      self_link;   /* named through "self" (a link), not "<pid>" */
    int       fd;          /* N_FD: N */
};

/* One entry of a listing. */
struct pentry {
    char     name[NAME_LEN];
    uint64_t ino;
    uint8_t  type;   /* DT_* */
    int64_t  next;   /* the position after it */
};

/* The program's ELF header: posix.ld maps it, the linker names it. */
extern const Elf64_Ehdr __ehdr_start __attribute__((visibility("hidden")));

static lock_t exe_lock;
static int    exe_state;   /* exe_lock: 0 not looked for, 1 found (exe_path), -1 not found */
static char  *exe_path;    /* exe_lock: absolute, malloc'd; never changed once set */
static char  *argv0;       /* argv[0] as the program started: the constructor's copy */

/* Boot-image programs by path, as posix_spawn runs them: /boot itself,
 * and /bin and /usr/bin, its bin and usr/bin. */
static const struct { const char *path; const char *name; } boot_dirs[] = {
    { "/boot/", "" }, { "/bin/", "bin/" }, { "/usr/bin/", "usr/bin/" },
};

static const struct fd_ops procdir_ops;

/* Before main: argv[0] may be rewritten by the program later (a process
 * title), and exe is asked for only when a program reads it. */
__attribute__((constructor)) static void keep_argv0(void)
{
    if (program_invocation_name)
        argv0 = strdup(program_invocation_name);
}

/* ---- which file the program is ------------------------------------------------- */

/* A program image to compare: a boot file's bytes, or a file open at fd. */
struct image {
    const uint8_t *mem;    /* the bytes, or NULL for fd */
    uint64_t       size;   /* mem's */
    long           fd;
    uint8_t       *buf;    /* CHUNK bytes, for fd's */
};

/* Are the image's bytes [off, off + n) those at want? */
static bool same(const struct image *im, uint64_t off, const void *want, uint64_t n)
{
    if (im->mem)
        return off <= im->size && n <= im->size - off && !memcmp(im->mem + off, want, n);
    for (uint64_t done = 0; done < n;) {
        size_t k = n - done < CHUNK ? (size_t)(n - done) : CHUNK;
        if (lx_pread64(im->fd, (long)(uintptr_t)im->buf, (long)k, (long)(off + done), 0, 0) !=
                (long)k ||
            memcmp(im->buf, (const uint8_t *)want + done, k))
            return false;
        done += k;
    }
    return true;
}

/* Is the image this program: its ELF header, program headers and every
 * segment that can't have changed since it was loaded (the writable one
 * has) as they are in our memory? The headers are compared first, so the
 * segments' places come from headers known to be the file's. */
static bool is_us(const struct image *im)
{
    const Elf64_Ehdr *eh = &__ehdr_start;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    if (eh->e_phentsize != sizeof(*ph) || eh->e_phnum > PH_MAX || !same(im, 0, eh, sizeof(*eh)) ||
        !same(im, eh->e_phoff, ph, (uint64_t)eh->e_phnum * sizeof(*ph)))
        return false;
    for (unsigned i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && !(ph[i].p_flags & PF_W) &&
            !same(im, ph[i].p_offset, (const void *)(uintptr_t)ph[i].p_vaddr, ph[i].p_filesz))
            return false;
    return true;
}

/* The answer: "/boot/" + a bootfs name, or a path as it is. exe_lock. */
static bool found(const char *prefix, const char *rest)
{
    size_t a = strlen(prefix), b = strlen(rest);
    exe_path = a + b < PATH_BYTES ? malloc(a + b + 1) : NULL;
    if (!exe_path)
        return false;
    memcpy(exe_path, prefix, a);
    memcpy(exe_path + a, rest, b + 1);
    return true;
}

static bool boot_is_us(const struct bootfs_view *fs, const char *name)
{
    struct image im = { 0 };
    const void *data;
    if (bootfs_lookup(fs, name, &data, &im.size) != OK)
        return false;
    im.mem = data;
    return is_us(&im);
}

/* A file in a mount (never libc's own trees: no /proc loop). */
static bool file_is_us(const struct path *p)
{
    if (p->where != PATH_MOUNT || path_libc(p) != LIBC_NONE)
        return false;
    struct image im = { .buf = malloc(CHUNK) };
    im.fd = im.buf ? lx_openat(AT_FDCWD, (long)(uintptr_t)p->abs, O_RDONLY | O_CLOEXEC, 0, 0, 0)
                   : -ENOMEM;
    bool us = im.fd >= 0 && is_us(&im);
    if (im.fd >= 0)
        (void)lx_close(im.fd, 0, 0, 0, 0, 0);   /* a read-only open: nothing to lose */
    free(im.buf);
    return us;
}

/* abs (clean) under a boot directory: its bootfs name into name. */
static bool boot_name(const char *abs, char name[PATH_BYTES])
{
    for (unsigned i = 0; i < sizeof(boot_dirs) / sizeof(boot_dirs[0]); i++) {
        size_t l = strlen(boot_dirs[i].path);
        if (!strncmp(abs, boot_dirs[i].path, l)) {
            snprintf(name, PATH_BYTES, "%s%s", boot_dirs[i].name, abs + l);
            return true;
        }
    }
    return false;
}

/* The file argv[0] names, if it is this program. exe_lock. */
static bool from_argv0(const struct bootfs_view *fs, bool boot)
{
    char *name = malloc(PATH_BYTES);
    struct path *p = malloc(sizeof(*p));
    bool ok = false;
    if (name && p && argv0 && argv0[0] != '/' && argv0[0]) {
        /* the shell's "bin/x", or a bare name as its run finds it */
        snprintf(name, PATH_BYTES, "%s%s", strchr(argv0, '/') ? "" : "bin/", argv0);
        ok = boot && boot_is_us(fs, name) && found("/boot/", name);
    } else if (name && p && argv0 && !path_set(argv0, p)) {
        if (boot_name(p->abs, name))
            ok = boot && boot_is_us(fs, name) && found("/boot/", name);
        else
            ok = file_is_us(p) && found("", p->abs);
    }
    free(p);
    free(name);
    return ok;
}

/* The first file of the boot image that is this program. exe_lock. */
static bool from_scan(const struct bootfs_view *fs)
{
    const struct bootfs_entry *e =
        (const struct bootfs_entry *)(fs->base + sizeof(struct bootfs_header));
    for (uint32_t i = 0; i < fs->count; i++, e++) {
        if (strnlen(e->name, BOOTFS_NAME_MAX) == BOOTFS_NAME_MAX || e->offset > fs->size ||
            e->size > fs->size - e->offset)
            continue;
        struct image im = { .mem = fs->base + e->offset, .size = e->size };
        if (is_us(&im))
            return found("/boot/", e->name);
    }
    return false;
}

/* The program's file, or NULL (not found, kept so). */
static const char *exe(void)
{
    lock_take(&exe_lock);
    if (!exe_state) {
        const struct bootfs_view *fs;
        bool boot = bootfs_default(&fs) == OK;
        exe_state = from_argv0(fs, boot) || (boot && from_scan(fs)) ? 1 : -1;
    }
    const char *path = exe_state > 0 ? exe_path : NULL;
    lock_give(&exe_lock);
    return path;
}

/* ---- names ------------------------------------------------------------------------ */

bool proc_path(const char *abs)
{
    return (!strcmp(abs, PROC_DIR) || !strncmp(abs, PROC_DIR "/", sizeof(PROC_DIR))) &&
           !path_is_mount(PROC_DIR);
}

/* A decimal of Linux's form (no sign, no leading zero), else -1. */
static long number(const char *s, size_t n)
{
    if (!n || n > 9 || (s[0] == '0' && n > 1))
        return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* The next name of s at *at (past one slash): its length, 0 at the end. */
static size_t next_name(const char *s, size_t *at, const char **name)
{
    if (s[*at] == '/')
        (*at)++;
    size_t l = 0;
    while (s[*at + l] && s[*at + l] != '/')
        l++;
    *name = s + *at;
    *at += l;
    return l;
}

static bool fd_open(long fd)
{
    struct fd_desc *d = fd_get(fd);
    if (d)
        fd_put(d);
    return d != NULL;
}

/* What abs (clean, proc_path's) is: 0, or -ENOENT (no such name: every
 * ENOENT of a name /proc doesn't have comes from here), -ENOTDIR (a name
 * under a link to something that isn't a directory). */
static long lookup(const char *abs, struct at *out)
{
    const char *s = abs + sizeof(PROC_DIR) - 1, *n;
    size_t at = 0, l;
    *out = (struct at){ .node = N_ROOT };
    if (!(l = next_name(s, &at, &n)))
        return 0;
    out->self_link = l == 4 && !memcmp(n, "self", 4);
    if (!out->self_link && number(n, l) != lx_pid())
        return -ENOENT;   /* another process, or a file of Linux's we don't have */
    out->node = N_SELF;
    if (!(l = next_name(s, &at, &n)))
        return 0;
    if (l == 3 && !memcmp(n, "exe", 3))
        out->node = N_EXE;
    else if (l == 2 && !memcmp(n, "fd", 2))
        out->node = N_FDDIR;
    else
        return -ENOENT;
    if (!(l = next_name(s, &at, &n)))
        return 0;
    if (out->node == N_EXE)
        return -ENOTDIR;
    long fd = number(n, l);
    if (fd < 0 || fd >= FD_MAX || !fd_open(fd))
        return -ENOENT;
    out->node = N_FD;
    out->fd = (int)fd;
    return next_name(s, &at, &n) ? -ENOTDIR : 0;   /* through a directory's link: not built */
}

/* ---- what a descriptor is, as a link ---------------------------------------------- */

/* A character device's path by its number (dev.c's, tty.c's, pty.c's). */
static bool chr_link(dev_t rdev, char out[PATH_BYTES])
{
    static const char *const mem[] = { [3] = "null", [5] = "zero", [7] = "full",
                                       [8] = "random", [9] = "urandom" };
    unsigned ma = major(rdev), mi = minor(rdev);
    if (ma == 1 && mi < sizeof(mem) / sizeof(mem[0]) && mem[mi])
        snprintf(out, PATH_BYTES, "/dev/%s", mem[mi]);
    else if (ma == 5 && (mi == 0 || mi == 2))
        snprintf(out, PATH_BYTES, "/dev/%s", mi ? "ptmx" : "tty");
    else if (ma == 136)
        snprintf(out, PATH_BYTES, "/dev/pts/%u", mi);
    else
        return false;
    return true;
}

/* An /etc file's path: what a child is given for it (no handles). */
static bool etc_link(struct fd_desc *d, char out[PATH_BYTES])
{
    struct fd_given g = { 0 };
    bool ok = !d->ops->give(d, &g) && g.data;
    if (ok)
        snprintf(out, PATH_BYTES, "%s", (const char *)g.data);
    fd_given_drop(&g);
    return ok;
}

/* The link /proc/self/fd/N reads for description d, into out. */
static void link_of(struct fd_desc *d, char out[PATH_BYTES])
{
    const char *path = d->ops->path ? d->ops->path(d) : NULL;
    struct stat st = { 0 };
    const char *kind = d->ops->name;
    if (path)
        snprintf(out, PATH_BYTES, "%s", path);
    else if (d->ops == &memfd_ops)
        snprintf(out, PATH_BYTES, "/memfd:%s (deleted)", memfd_name(d));
    else if (d->ops == &etc_ops && etc_link(d, out))
        return;
    else if (d->ops->stat && !d->ops->stat(d, &st) && S_ISCHR(st.st_mode) &&
             chr_link(st.st_rdev, out))
        return;
    else if (d->ops == &pipe_ops)
        snprintf(out, PATH_BYTES, "pipe:[%llu]", (unsigned long long)d->serial);
    else if (d->ops == &sockp_ops || !strcmp(kind, "socket"))
        snprintf(out, PATH_BYTES, "socket:[%llu]", (unsigned long long)d->serial);
    else
        snprintf(out, PATH_BYTES, "anon_inode:[%s]", !strcmp(kind, "epoll") ? "eventpoll" : kind);
}

/* ---- stat and readlink ------------------------------------------------------------- */

static uint64_t ino_of(enum node node, int fd)
{
    return node == N_FD ? INO_FD + (uint64_t)fd : INO_BASE + node;
}

static void node_stat(enum node node, int fd, struct stat *st)
{
    bool dir = node == N_ROOT || node == N_SELF || node == N_FDDIR;
    st->st_mode = dir ? S_IFDIR | (node == N_FDDIR ? 0500 : 0555) : S_IFLNK | 0777;
    st->st_nlink = dir ? 2 : 1;
    st->st_uid = st->st_gid = node == N_ROOT ? 0 : ONE_USER;
    st->st_ino = ino_of(node, fd);
    st->st_blksize = 4096;
}

long proc_stat(const struct path *p, bool follow, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    struct at a;
    long r = lookup(p->abs, &a);
    if (r)
        return r;
    bool link = a.node == N_EXE || a.node == N_FD || (a.node == N_SELF && a.self_link);
    if (link && !follow && !p->must_dir) {   /* "link/" is followed, as Linux */
        node_stat(a.node == N_SELF ? N_SELFLINK : a.node, a.fd, st);
        return 0;
    }
    const char *e = a.node == N_EXE ? exe() : NULL;
    if (a.node == N_EXE)
        r = e ? lx_newfstatat(AT_FDCWD, (long)(uintptr_t)e, (long)(uintptr_t)st, 0, 0, 0) : -ENOENT;
    else if (a.node == N_FD)
        r = lx_fstat(a.fd, (long)(uintptr_t)st, 0, 0, 0, 0);
    else
        node_stat(a.node, 0, st);
    if (!r && p->must_dir && !S_ISDIR(st->st_mode))
        r = -ENOTDIR;
    return r;
}

static long copy_link(const char *text, char *buf, size_t size)
{
    size_t n = strlen(text);
    if (n > size)
        n = size;   /* readlink(2): cut short, no NUL, never an error */
    memcpy(buf, text, n);
    return (long)n;
}

long proc_readlink(const struct path *p, char *buf, size_t size)
{
    struct at a;
    long r = lookup(p->abs, &a);
    if (r)
        return r;
    if (p->must_dir) {   /* "link/" names the target, which is no link */
        struct stat st;
        r = proc_stat(p, true, &st);
        return r ? r : -EINVAL;
    }
    if (a.node == N_SELF && a.self_link) {
        char pid[NAME_LEN];
        snprintf(pid, sizeof(pid), "%d", (int)lx_pid());
        return copy_link(pid, buf, size);
    }
    if (a.node == N_EXE) {
        const char *e = exe();
        return e ? copy_link(e, buf, size) : -ENOENT;
    }
    if (a.node != N_FD)
        return -EINVAL;   /* a directory */
    struct fd_desc *d = fd_get(a.fd);
    char *text = d ? malloc(PATH_BYTES) : NULL;
    r = !d ? -ENOENT : !text ? -ENOMEM : 0;
    if (!r) {
        link_of(d, text);
        r = copy_link(text, buf, size);
    }
    free(text);
    if (d)
        fd_put(d);
    return r;
}

/* ---- open ------------------------------------------------------------------------- */

static long open_dir(enum node node, long flags)
{
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return -EISDIR;
    uint32_t status = O_RDONLY | ((uint32_t)flags & (O_NONBLOCK | O_PATH | O_DIRECTORY));
    return kind_install(&procdir_ops, (void *)(uintptr_t)node, status, flags & O_CLOEXEC);
}

static long open_exe(const struct path *p, long flags)
{
    if (p->must_dir || (flags & O_DIRECTORY))
        return -ENOTDIR;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return -ETXTBSY;   /* the running program's file: Linux's answer */
    const char *e = exe();
    if (!e)
        return -ENOENT;
    return lx_openat(AT_FDCWD, (long)(uintptr_t)e, flags & ~(long)(O_CREAT | O_NOFOLLOW), 0, 0, 0);
}

/* d shared by a new descriptor, when the access asked is d's. */
static long share(struct fd_desc *d, long flags)
{
    long acc = flags & O_ACCMODE;
    if ((acc != O_WRONLY && !fd_can_read(d)) || (acc != O_RDONLY && !fd_can_write(d)))
        return -EACCES;
    fd_ref(d);
    return fd_install(d, 0, flags & O_CLOEXEC);
}

/* Anything but a memfd, by what its link says. */
static long reopen_other(struct fd_desc *d, long flags, bool dir)
{
    char *t = malloc(PATH_BYTES);
    if (!t)
        return -ENOMEM;
    link_of(d, t);
    long r;
    if (t[0] == '/' && proc_path(t))
        r = -ELOOP;   /* a directory handed to us as one of /proc's: never followed */
    else if (t[0] == '/')
        r = lx_openat(AT_FDCWD, (long)(uintptr_t)t,
                      (flags & ~(long)(O_CREAT | O_EXCL | O_NOFOLLOW)) | (dir ? O_DIRECTORY : 0), 0,
                      0, 0);
    else if (d->ops == &pipe_ops)
        r = dir ? -ENOTDIR : share(d, flags);
    else
        r = -ENXIO;   /* a socket or an anonymous kind: Linux's answer */
    free(t);
    return r;
}

static long reopen(int fd, long flags, bool dir)
{
    struct fd_desc *d = fd_get(fd);
    if (!d)
        return -ENOENT;   /* closed since the lookup */
    long r;
    if (d->ops == &memfd_ops)
        r = dir ? -ENOTDIR : memfd_reopen(d, (int)flags);
    else
        r = reopen_other(d, flags, dir);
    fd_put(d);
    return r;
}

long proc_open(const struct path *p, long flags)
{
    struct at a;
    long r = lookup(p->abs, &a);
    if (r)
        return r;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return -EEXIST;
    bool link = a.node == N_EXE || a.node == N_FD || (a.node == N_SELF && a.self_link);
    if (link && (flags & O_NOFOLLOW) && !(flags & O_PATH))
        return -ELOOP;
    if (a.node == N_EXE)
        return open_exe(p, flags);
    if (a.node == N_FD)
        return reopen(a.fd, flags, p->must_dir || (flags & O_DIRECTORY));
    return open_dir(a.node, flags);
}

/* ---- the directories' kind ------------------------------------------------------- */

static enum node node_of(struct fd_desc *d)
{
    return (enum node)(uintptr_t)d->priv;
}

/* /proc/self/fd's entry at pos (2 or more): 1 and *e, or 0 at the end. */
static int fd_entry(int64_t pos, struct pentry *e)
{
    struct fd_desc *d;
    bool cloexec;
    long fd = pos - 2 < FD_MAX ? fd_next(pos - 2, &d, &cloexec) : -1;
    if (fd < 0)
        return 0;
    fd_put(d);
    snprintf(e->name, NAME_LEN, "%ld", fd);
    e->ino = INO_FD + (uint64_t)fd;
    e->type = DT_LNK;
    e->next = 2 + fd + 1;
    return 1;
}

/* The entry at pos of directory node: 1 and *e, or 0 at the end. Listing
 * positions: 0 ".", 1 "..", then 2 + k: entry k of /proc and /proc/self,
 * and for /proc/self/fd the first open descriptor >= k (so one opened or
 * closed between two calls shows or not as the table is then). */
static int entry_at(enum node node, int64_t pos, struct pentry *e)
{
    static const enum node parent[] = { [N_ROOT] = 0, [N_SELF] = N_ROOT, [N_FDDIR] = N_SELF };
    e->next = pos + 1;
    if (pos < 2) {
        snprintf(e->name, NAME_LEN, "%s", pos ? ".." : ".");
        e->ino = pos && node == N_ROOT ? INO_ROOT : ino_of(pos ? parent[node] : node, 0);
        e->type = DT_DIR;
        return 1;
    }
    if (node == N_FDDIR)
        return fd_entry(pos, e);
    if (pos > 3)
        return 0;
    bool first = pos == 2;   /* the link ("self", "exe"), then the directory */
    if (node == N_ROOT)
        snprintf(e->name, NAME_LEN, first ? "self" : "%d", (int)lx_pid());
    else
        snprintf(e->name, NAME_LEN, "%s", first ? "exe" : "fd");
    e->type = first ? DT_LNK : DT_DIR;
    if (node == N_ROOT)
        e->ino = ino_of(first ? N_SELFLINK : N_SELF, 0);
    else
        e->ino = ino_of(first ? N_EXE : N_FDDIR, 0);
    return 1;
}

static long pd_getdents(struct fd_desc *d, void *buf, size_t len)
{
    lock_take(&d->lock);
    int64_t pos = d->offset;
    lock_give(&d->lock);
    /* fd_next takes the table's lock: not under the description's (fd.h).
     * Two threads listing one description at once may see an entry twice. */
    struct pentry e;
    size_t used = 0;
    long r;
    while ((r = entry_at(node_of(d), pos, &e)) > 0) {
        size_t k = dir_put((uint8_t *)buf + used, len - used, e.name, e.ino, e.type, e.next);
        if (!k) {
            r = used ? 0 : -EINVAL;   /* the buffer can't hold the next one */
            break;
        }
        used += k;
        pos = e.next;
    }
    lock_take(&d->lock);
    d->offset = pos;
    lock_give(&d->lock);
    return r < 0 ? r : (long)used;
}

static long pd_read(struct fd_desc *d, const struct iovec *iov, int n, int64_t pos)
{
    return -EISDIR;
}

static long pd_seek(struct fd_desc *d, int64_t off, int whence)
{
    lock_take(&d->lock);
    int64_t to = -1;
    if (whence == SEEK_SET)
        to = off;
    else if (whence == SEEK_CUR && __builtin_add_overflow(d->offset, off, &to))
        to = -1;
    if (to >= 0)
        d->offset = to;
    lock_give(&d->lock);
    return to >= 0 ? to : -EINVAL;
}

static long pd_stat(struct fd_desc *d, struct stat *st)
{
    node_stat(node_of(d), 0, st);
    return 0;
}

/* No .give: a child left out of the hand-over gets none (its own /proc is
 * its own, and a directory record would name a mount). No .path: the *at
 * calls and fchdir take mount directories only. */
static const struct fd_ops procdir_ops = {
    .name = "/proc directory", .read = pd_read, .seek = pd_seek, .stat = pd_stat,
    .getdents = pd_getdents,
};
