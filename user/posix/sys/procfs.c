/* /proc (docs/CR-PLAN.md section 3): libc's own, like /dev and /etc, with
 * nothing in the namespace behind it; the few names a Linux program reads
 * about itself, about the machine and about its children, and ENOENT for
 * every other one, never a half-filled file.
 *
 *   /proc                 a directory: self, our pid, the pid of each child
 *                         of ours not yet reaped, and cpuinfo, meminfo and
 *                         stat
 *   /proc/self            a link to <pid>; /proc/<pid> is the same
 *                         directory for our own pid and for a child's (any
 *                         other pid is ENOENT: a process sees only itself
 *                         and its children here); a child's has maps, stat,
 *                         statm, status, smaps_rollup and fd, no exe
 *   /proc/cpuinfo, meminfo, stat, /proc/<pid>/maps, stat, statm, status,
 *                         smaps_rollup
 *                         regular files of Linux's text (procinfo.c and
 *                         cpuinfo.c make it: "PRC notes" in CR-PLAN.md says
 *                         from what), read only, size 0 as Linux's. A read
 *                         from the start makes the text afresh, and the
 *                         later reads of that pass take it from the one
 *                         copy, so reading a few bytes at a time sees one
 *                         file. /proc/self/fd/N of one reads
 *                         /proc/<pid>/<name>
 *   /proc/<pid>/cmdline   our argument strings, NUL-ended (ours and our
 *                         threads' only: a child's is not kept)
 *   /proc/<pid>/mem       ours only, read only: pread at an address reads
 *                         the program's memory there (EIO where it can't
 *                         be read). Never written: that would bypass the
 *                         pages' permissions
 *   /proc/self/task       a directory of our threads by id (the first,
 *                         then those clone made), each a directory with
 *                         maps, stat, statm, status, smaps_rollup, cmdline
 *                         and mem; the
 *                         thread's id is also a directory of /proc, not
 *                         listed, as Linux's (a process's id wins when
 *                         one number is both)
 *   /proc/version, /proc/sys/kernel/osrelease, ostype, version, hostname
 *                         what uname says (misc.c's fields)
 *   /proc/sys/fs/inotify/max_user_watches, max_user_instances,
 *                         max_queued_events: inotify.c's limits (kinds.h)
 *   /proc/self/exe        a link to the program's file: readlink gives its
 *                         path, open opens that file for reading (ETXTBSY
 *                         for writing, as Linux), stat stats it;
 *                         posix_spawn and execve of it start this program
 *                         again from its own pages (spawn.c's self-image)
 *   /proc/self/fd         a directory of our open descriptors, listed by
 *                         number as the table is when each getdents runs
 *   /proc/<pid>/fd        a child's: the numbers of its open descriptors as
 *                         links (DT_LNK), asked of the child (proc.c's
 *                         lx_child_fds) on each getdents; a name in it is
 *                         ENOENT (what each one is isn't asked), a child
 *                         we can't ask EACCES, a reaped one ESRCH
 *   /proc/self/fd/N       a link to what descriptor N is: a file's or a
 *                         directory's path, /dev/... for the devices,
 *                         /etc/... for libc's files, /memfd:NAME (deleted),
 *                         pipe:[I], socket:[I], anon_inode:[KIND] or
 *                         anon_inode:inotify (I: the
 *                         description's serial). open makes a new
 *                         description of the same thing (a path opened again;
 *                         a memfd's object with the access asked, so
 *                         O_RDONLY is a twin that can't be written or mapped
 *                         writable, here or in a child; a pipe end shared
 *                         when the access fits), ENXIO for a socket or an
 *                         anonymous kind
 *
 * Which file the program is. First the path the POSIX hand-over names
 * (<posixfds.h>): libjam's spawn names it for every program it gives a
 * hand-over (a boot-image one as /boot/<its name>), and posix_spawn for
 * one init's /svc/exec starts. It is the starter's word, as argv is: the
 * starter chose the program. A program started with no hand-over (the
 * shell's background runs, a native starter's) is looked for instead.
 * Every POSIX program maps its ELF and program
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
#include "calls.h"
#include "files.h"
#include "kinds.h"
#include "memfd.h"
#include "proc.h"
#include "procinfo.h"
#include "sig.h"
#include "threads.h"

#define PROC_DIR  "/proc"
#define INO_BASE  0x7000000000ull       /* our nodes' st_ino: clear of every mount's ids */
#define INO_FD    (INO_BASE + 0x40000000ull)   /* + N: /proc/self/fd/N (a pid's nodes: node + pid << 8) */
#define INO_ROOT  1u                    /* "/"'s (path.c's): /proc's ".." */
#define PH_MAX    16u                   /* program headers we look at */
#define CHUNK     16384u                /* a file compared this many bytes at a time */
#define NAME_LEN  24u                   /* a listing's names: numbers, "self", "exe", "fd" */
#define ONE_USER  1000u
#define MEM_STEP  4096u                 /* /proc/self/mem: bytes copied at a time (one page) */
#define MEM_MAX   (1u << 20)            /* ... and most one read gives: a short read is allowed */

enum node {
    N_ROOT = 1,   /* /proc */
    N_SELF,       /* /proc/self, /proc/<pid>: the directory */
    N_SELFLINK,   /* /proc/self as a link (only lstat and readlink see it) */
    N_EXE,        /* /proc/self/exe */
    N_FDDIR,      /* /proc/self/fd */
    N_FD,         /* /proc/self/fd/N */
    N_TASKDIR,    /* /proc/self/task */
    N_TASK,       /* /proc/self/task/<tid>, /proc/<tid>: a thread's directory */
    N_SYSDIR,     /* /proc/sys */
    N_SYSKERNEL,  /* /proc/sys/kernel */
    N_SYSFS,      /* /proc/sys/fs */
    N_SYSINOTIFY, /* /proc/sys/fs/inotify */
    N_MAPS,       /* /proc/<pid>/maps (and the files below it) */
    N_STAT,       /* /proc/<pid>/stat */
    N_STATM,      /* /proc/<pid>/statm */
    N_STATUS,     /* /proc/<pid>/status */
    N_CMDLINE,    /* /proc/<pid>/cmdline */
    N_MEM,        /* /proc/<pid>/mem */
    N_CPUINFO,    /* /proc/cpuinfo */
    N_MEMINFO,    /* /proc/meminfo */
    N_SYSSTAT,    /* /proc/stat */
    N_VERSION,    /* /proc/version */
    N_OSRELEASE,  /* /proc/sys/kernel/osrelease */
    N_OSTYPE,     /* /proc/sys/kernel/ostype */
    N_KVERSION,   /* /proc/sys/kernel/version */
    N_HOSTNAME,   /* /proc/sys/kernel/hostname */
    N_SMAPS_ROLLUP,   /* /proc/<pid>/smaps_rollup */
    N_INO_WATCHES,    /* /proc/sys/fs/inotify/max_user_watches */
    N_INO_INSTANCES,  /* /proc/sys/fs/inotify/max_user_instances */
    N_INO_QUEUED,     /* /proc/sys/fs/inotify/max_queued_events */
};

/* Where a generated file lives. */
enum where { IN_PID, IN_ROOT, IN_KERNEL, IN_INOTIFY };

/* The generated files: which text each is, its name and where it lives. */
static const struct { enum node node; enum pfile file; const char *name; enum where where; }
pfiles[] = {
    { N_MAPS, PF_MAPS, "maps", IN_PID }, { N_STAT, PF_STAT, "stat", IN_PID },
    { N_STATM, PF_STATM, "statm", IN_PID }, { N_STATUS, PF_STATUS, "status", IN_PID },
    { N_CMDLINE, PF_CMDLINE, "cmdline", IN_PID }, { N_MEM, PF_MEM, "mem", IN_PID },
    { N_CPUINFO, PF_CPUINFO, "cpuinfo", IN_ROOT }, { N_MEMINFO, PF_MEMINFO, "meminfo", IN_ROOT },
    { N_SYSSTAT, PF_SYSSTAT, "stat", IN_ROOT }, { N_VERSION, PF_VERSION, "version", IN_ROOT },
    { N_OSRELEASE, PF_OSRELEASE, "osrelease", IN_KERNEL },
    { N_OSTYPE, PF_OSTYPE, "ostype", IN_KERNEL }, { N_KVERSION, PF_KVERSION, "version", IN_KERNEL },
    { N_HOSTNAME, PF_HOSTNAME, "hostname", IN_KERNEL },
    { N_SMAPS_ROLLUP, PF_SMAPS_ROLLUP, "smaps_rollup", IN_PID },
    { N_INO_WATCHES, PF_INO_WATCHES, "max_user_watches", IN_INOTIFY },
    { N_INO_INSTANCES, PF_INO_INSTANCES, "max_user_instances", IN_INOTIFY },
    { N_INO_QUEUED, PF_INO_QUEUED, "max_queued_events", IN_INOTIFY },
};
#define PFILES (sizeof(pfiles) / sizeof(pfiles[0]))

/* Whose directory: ours (/proc/self, /proc/<our pid>), a child's, or one
 * of our threads'. */
#define WHO_SELF 1u
#define WHO_KID  2u
#define WHO_TASK 4u
#define WHO_ALL  (WHO_SELF | WHO_KID | WHO_TASK)

/* A process directory's entries, after "." and "..", and whose have each. */
static const struct { enum node node; uint8_t type; const char *name; unsigned who; } pid_names[] = {
    { N_EXE, DT_LNK, "exe", WHO_SELF }, { N_FDDIR, DT_DIR, "fd", WHO_SELF | WHO_KID },
    { N_TASKDIR, DT_DIR, "task", WHO_SELF }, { N_MAPS, DT_REG, "maps", WHO_ALL },
    { N_STAT, DT_REG, "stat", WHO_ALL }, { N_STATM, DT_REG, "statm", WHO_ALL },
    { N_STATUS, DT_REG, "status", WHO_ALL }, { N_CMDLINE, DT_REG, "cmdline", WHO_SELF | WHO_TASK },
    { N_MEM, DT_REG, "mem", WHO_SELF | WHO_TASK },
    { N_SMAPS_ROLLUP, DT_REG, "smaps_rollup", WHO_ALL },
};
#define PID_NAMES  (sizeof(pid_names) / sizeof(pid_names[0]))
#define KIDS_LISTED 256      /* children /proc lists (their files open all the same) */

/* What a /proc path is. */
struct at {
    enum node node;
    bool      self_link;   /* named through "self" (a link), not "<pid>" */
    int       fd;          /* N_FD: N */
    int32_t   pid;         /* a process's node: its pid (ours, or a child's) */
    uint32_t  tid;         /* a thread's node, or a file below one: its id; else 0 */
};

/* One entry of a listing. */
struct pentry {
    char     name[NAME_LEN];
    uint64_t ino;
    uint8_t  type;   /* DT_* */
    int64_t  next;   /* the position after it */
};

/* An open generated file: the text of the pass being read (d->lock). */
struct pfile_state {
    enum pfile file;
    int32_t    pid;        /* the process of a /proc/<pid> file */
    uint32_t   tid;        /* the thread of a /proc/<pid>/task/<tid> file, else 0 */
    char      *text;       /* NULL until the first read; replaced by a read from the start */
    size_t     len;
    char       path[64];   /* /proc/<pid>/<name>, /proc/self/fd/N's link */
};

/* The program's ELF header: posix.ld maps it, the linker names it. */
extern const Elf64_Ehdr __ehdr_start __attribute__((visibility("hidden")));

static lock_t exe_lock;
static int    exe_state;   /* exe_lock: 0 not looked for, 1 found (exe_path), -1 not found */
static char  *exe_path;    /* exe_lock: absolute, malloc'd; never changed once set */
static char  *argv0;       /* argv[0] as the program started: the constructor's copy */
static uintptr_t stack_addr;   /* an address on the main thread's stack (the constructor's) */

/* A forked child: the lock fresh (the answer, if found, is the same
 * program's). */
void procfs_forked(void)
{
    exe_lock = (lock_t){ 0 };
}

/* Boot-image programs by path, as posix_spawn runs them: /boot itself,
 * and /bin and /usr/bin, its bin and usr/bin. */
static const struct { const char *path; const char *name; } boot_dirs[] = {
    { "/boot/", "" }, { "/bin/", "bin/" }, { "/usr/bin/", "usr/bin/" },
};

static const struct fd_ops procdir_ops;
static const struct fd_ops pfile_ops;

/* Before main: argv[0] may be rewritten by the program later (a process
 * title), and exe is asked for only when a program reads it. Constructors
 * run on the main thread, so a local's address is on the main stack: the
 * mapping that holds it is /proc/self/maps' [stack]. */
__attribute__((constructor)) static void keep_argv0(void)
{
    stack_addr = (uintptr_t)__builtin_frame_address(0);
    if (program_invocation_name)
        argv0 = jam_strdup(program_invocation_name);
}

uintptr_t proc_stack_addr(void)
{
    return stack_addr;
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
    exe_path = a + b < PATH_BYTES ? jam_malloc(a + b + 1) : NULL;
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
    struct image im = { .buf = jam_malloc(CHUNK) };
    im.fd = im.buf ? lx_openat(AT_FDCWD, (long)(uintptr_t)p->abs, O_RDONLY | O_CLOEXEC, 0, 0, 0)
                   : -ENOMEM;
    bool us = im.fd >= 0 && is_us(&im);
    if (im.fd >= 0)
        (void)lx_close(im.fd, 0, 0, 0, 0, 0);   /* a read-only open: nothing to lose */
    jam_free(im.buf);
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
    char *name = jam_malloc(PATH_BYTES);
    struct path *p = jam_malloc(sizeof(*p));
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
    jam_free(p);
    jam_free(name);
    return ok;
}

/* The path the POSIX hand-over named (libjam's spawn names it for every
 * program started with one): taken as the starter gave it, cleaned, if it
 * is absolute and not under libc's own trees. exe_lock. */
static bool from_handover(void)
{
    const char *given = lx_handover_exe();
    struct path *p = given ? jam_malloc(sizeof(*p)) : NULL;
    bool ok = p && !path_set(given, p) && path_libc(p) == LIBC_NONE && found("", p->abs);
    jam_free(p);
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

const char *proc_exe(void)
{
    lock_take(&exe_lock);
    if (!exe_state) {
        const struct bootfs_view *fs;
        bool boot = bootfs_default(&fs) == OK;
        exe_state = from_handover() || from_argv0(fs, boot) || (boot && from_scan(fs)) ? 1 : -1;
    }
    const char *path = exe_state > 0 ? exe_path : NULL;
    lock_give(&exe_lock);
    return path;
}

static void exe_on(void *path)
{
    *(const char **)path = proc_exe();
}

/* dladdr's override (user/posix/musl/src/ldso/dladdr.c): a static program's
 * one module is its image. */
int __jam_dladdr(const void *addr, const char **file, void **base)
{
    const Elf64_Ehdr *eh = &__ehdr_start;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    uintptr_t bias = 0;   /* where the image is, against its headers' addresses */
    for (unsigned i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && !ph[i].p_offset)
            bias = (uintptr_t)eh - ph[i].p_vaddr;
    uintptr_t a = (uintptr_t)addr - bias;
    bool in = false;
    for (unsigned i = 0; i < eh->e_phnum && !in; i++)
        in = ph[i].p_type == PT_LOAD && a >= ph[i].p_vaddr && a - ph[i].p_vaddr < ph[i].p_memsz;
    if (!in)
        return 0;
    /* The layer's own, under exe_lock: no handler runs inside (one that
     * calls dladdr would wait for the lock for ever); where calls run (the
     * first looks up files: a small stack's runs on the stack for calls). */
    const char *path = NULL;
    struct sig_thread *q = sig_quiet_begin();
    lx_call_stack_run(exe_on, &path);
    sig_quiet_end(q);
    *file = path ? path : argv0 ? argv0 : "";
    *base = (void *)(uintptr_t)eh;
    return 1;
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

/* The generated file named n that lives in where, or 0. */
static enum node find_file(enum where where, const char *n, size_t l)
{
    for (unsigned i = 0; i < PFILES; i++)
        if (pfiles[i].where == where && strlen(pfiles[i].name) == l && !memcmp(pfiles[i].name, n, l))
            return pfiles[i].node;
    return 0;
}

/* The entry named n of a process's or thread's directory (who: WHO_*), or 0. */
static enum node pid_name(unsigned who, const char *n, size_t l)
{
    for (unsigned i = 0; i < PID_NAMES; i++)
        if ((pid_names[i].who & who) && strlen(pid_names[i].name) == l &&
            !memcmp(pid_names[i].name, n, l))
            return pid_names[i].node;
    return 0;
}

/* The name n (length l) is one of /proc's own, not a process's: /proc/sys
 * and its directories (kernel, fs/inotify), or a file. s, *at: the rest of
 * the path. */
static long lookup_system(const char *s, size_t *at, const char *n, size_t l, struct at *out)
{
    if (l == 3 && !memcmp(n, "sys", 3)) {
        out->node = N_SYSDIR;
        if (!(l = next_name(s, at, &n)))
            return 0;
        if (l == 6 && !memcmp(n, "kernel", 6)) {
            out->node = N_SYSKERNEL;
            if (!(l = next_name(s, at, &n)))
                return 0;
            out->node = find_file(IN_KERNEL, n, l);
        } else if (l == 2 && !memcmp(n, "fs", 2)) {
            out->node = N_SYSFS;
            if (!(l = next_name(s, at, &n)))
                return 0;
            if (l != 7 || memcmp(n, "inotify", 7))
                return -ENOENT;
            out->node = N_SYSINOTIFY;
            if (!(l = next_name(s, at, &n)))
                return 0;
            out->node = find_file(IN_INOTIFY, n, l);
        } else {
            return -ENOENT;
        }
    } else {
        out->node = find_file(IN_ROOT, n, l);
    }
    if (!out->node)
        return -ENOENT;
    return next_name(s, at, &n) ? -ENOTDIR : 0;
}

/* What abs (clean, proc_path's) is: 0, or -ENOENT (no such name: every
 * ENOENT of a name /proc doesn't have comes from here), -ENOTDIR (a name
 * under a link to something that isn't a directory, or under a file). */
static long lookup_name(const char *abs, struct at *out)
{
    const char *s = abs + sizeof(PROC_DIR) - 1, *n;
    size_t at = 0, l;
    *out = (struct at){ .node = N_ROOT };
    if (!(l = next_name(s, &at, &n)))
        return 0;
    out->self_link = l == 4 && !memcmp(n, "self", 4);
    long pid = out->self_link ? lx_pid() : number(n, l);
    if (pid < 0)   /* not a process: /proc's own */
        return lookup_system(s, &at, n, l, out);
    bool kid = false;
    uint32_t tid = 0;
    if (pid != lx_pid()) {
        if (lx_child_exists((int32_t)pid))
            kid = true;
        else if (thread_tid_exists((uint32_t)pid))   /* /proc/<tid>: one of our threads */
            tid = (uint32_t)pid, pid = lx_pid();
        else
            return -ENOENT;   /* another process, or a file of Linux's we don't have */
    }
    out->pid = (int32_t)pid;
    out->tid = tid;
    out->node = tid ? N_TASK : N_SELF;
    if (!(l = next_name(s, &at, &n)))
        return 0;
    out->node = pid_name(tid ? WHO_TASK : kid ? WHO_KID : WHO_SELF, n, l);
    if (!out->node)
        return -ENOENT;
    if (!(l = next_name(s, &at, &n)))
        return 0;
    if (out->node == N_TASKDIR) {   /* /proc/<pid>/task/<tid>[/<name>] */
        long t = number(n, l);
        if (t < 0 || !thread_tid_exists((uint32_t)t))
            return -ENOENT;
        out->tid = (uint32_t)t;
        out->node = N_TASK;
        if (!(l = next_name(s, &at, &n)))
            return 0;
        out->node = pid_name(WHO_TASK, n, l);
        if (!out->node)
            return -ENOENT;
        return next_name(s, &at, &n) ? -ENOTDIR : 0;
    }
    if (out->node != N_FDDIR)
        return -ENOTDIR;
    if (kid)   /* a child's descriptors are listed, not looked into: what each is isn't asked */
        return -ENOENT;
    long fd = number(n, l);
    if (fd < 0 || fd >= FD_MAX || !fd_open(fd))
        return -ENOENT;
    out->node = N_FD;
    out->fd = (int)fd;
    return next_name(s, &at, &n) ? -ENOTDIR : 0;   /* through a directory's link: not built */
}

/* lookup_name, with every name /proc doesn't have noted in the ENOSYS log
 * (CL3's: written only when the program's environment asks for it). */
static long lookup(const char *abs, struct at *out)
{
    long r = lookup_name(abs, out);
    if (r == -ENOENT)
        lx_enosys_path(abs);
    return r;
}

bool proc_is_exe(const char *abs)
{
    struct at a;
    return proc_path(abs) && !lookup_name(abs, &a) && a.node == N_EXE;
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
    else if (d->ops == &pfile_ops)
        snprintf(out, PATH_BYTES, "%s", ((struct pfile_state *)d->priv)->path);
    else if (d->ops == &etc_ops && etc_link(d, out))
        return;
    else if (d->ops->stat && !d->ops->stat(d, &st) && S_ISCHR(st.st_mode) &&
             chr_link(st.st_rdev, out))
        return;
    else if (d->ops == &pipe_ops)
        snprintf(out, PATH_BYTES, "pipe:[%llu]", (unsigned long long)d->serial);
    else if (d->ops == &sockp_ops || !strcmp(kind, "socket"))
        snprintf(out, PATH_BYTES, "socket:[%llu]", (unsigned long long)d->serial);
    else if (d->ops == &inotify_ops)
        snprintf(out, PATH_BYTES, "anon_inode:inotify");   /* Linux's one without brackets */
    else
        snprintf(out, PATH_BYTES, "anon_inode:[%s]", !strcmp(kind, "epoll") ? "eventpoll" : kind);
}

/* ---- stat and readlink ------------------------------------------------------------- */

static bool is_file(enum node node)
{
    return node >= N_MAPS;
}

/* id: the descriptor of an N_FD, else the pid of a process's node. */
static uint64_t ino_of(enum node node, int id)
{
    return node == N_FD ? INO_FD + (uint64_t)id : INO_BASE + node + ((uint64_t)id << 8);
}

static bool is_dir(enum node node)
{
    return node == N_ROOT || node == N_SELF || node == N_FDDIR || node == N_TASKDIR ||
           node == N_TASK || node == N_SYSDIR || node == N_SYSKERNEL || node == N_SYSFS ||
           node == N_SYSINOTIFY;
}

static void node_stat(enum node node, int id, struct stat *st)
{
    bool dir = is_dir(node);
    st->st_mode = dir ? S_IFDIR | (node == N_FDDIR ? 0500 : 0555)
                      : is_file(node) ? S_IFREG | (node == N_MEM ? 0400 : 0444) : S_IFLNK | 0777;
    st->st_nlink = dir ? 2 : 1;
    st->st_uid = st->st_gid = node == N_ROOT || node == N_SYSDIR || node == N_SYSKERNEL ||
                              node == N_SYSFS || node == N_SYSINOTIFY ||
                              node >= N_CPUINFO ? 0 : ONE_USER;
    st->st_ino = ino_of(node, id);
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
        node_stat(a.node == N_SELF ? N_SELFLINK : a.node, a.node == N_FD ? a.fd : a.pid, st);
        return 0;
    }
    const char *e = a.node == N_EXE ? proc_exe() : NULL;
    if (a.node == N_EXE)
        r = e ? lx_newfstatat(AT_FDCWD, (long)(uintptr_t)e, (long)(uintptr_t)st, 0, 0, 0) : -ENOENT;
    else if (a.node == N_FD)
        r = lx_fstat(a.fd, (long)(uintptr_t)st, 0, 0, 0, 0);
    else
        node_stat(a.node, a.tid ? (int)a.tid : a.pid, st);
    if (!r && p->must_dir && !S_ISDIR(st->st_mode))
        r = -ENOTDIR;
    return r;
}

static long copy_link(const char *text, char *buf, size_t size)
{
    size_t n = strlen(text);
    if (n > size)
        n = size;   /* readlink(2): cut short, no NUL, never an error */
    return uc_put(buf, text, n) ? -EFAULT : (long)n;   /* the caller's buf */
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
        const char *e = proc_exe();
        return e ? copy_link(e, buf, size) : -ENOENT;
    }
    if (a.node != N_FD)
        return -EINVAL;   /* a directory */
    struct fd_desc *d = fd_get(a.fd);
    char *text = d ? jam_malloc(PATH_BYTES) : NULL;
    r = !d ? -ENOENT : !text ? -ENOMEM : 0;
    if (!r) {
        link_of(d, text);
        r = copy_link(text, buf, size);
    }
    jam_free(text);
    if (d)
        fd_put(d);
    return r;
}

/* ---- open ------------------------------------------------------------------------- */

/* A directory's description: its node and, for a process's, its pid and,
 * for a thread's, its id in priv (node in 8 bits, pid in 24, tid above). */
static long open_dir(enum node node, int32_t pid, uint32_t tid, long flags)
{
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return -EISDIR;
    uint32_t status = O_RDONLY | ((uint32_t)flags & (O_NONBLOCK | O_PATH | O_DIRECTORY));
    return kind_install(&procdir_ops,
                        (void *)((uintptr_t)node | (uintptr_t)(pid & 0xffffff) << 8 |
                                 (uintptr_t)tid << 32),
                        status, flags & O_CLOEXEC);
}

/* A generated file: no writing (Linux's EACCES), no directory. */
static long open_file(const struct at *a, const struct path *p, long flags)
{
    if (p->must_dir || (flags & O_DIRECTORY))
        return -ENOTDIR;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return -EACCES;
    struct pfile_state *st = jam_calloc(1, sizeof(*st));
    if (!st)
        return -ENOMEM;
    for (unsigned i = 0; i < PFILES; i++)
        if (pfiles[i].node == a->node) {
            st->file = pfiles[i].file;
            if (pfiles[i].where == IN_ROOT)
                snprintf(st->path, sizeof(st->path), PROC_DIR "/%s", pfiles[i].name);
            else if (pfiles[i].where == IN_KERNEL)
                snprintf(st->path, sizeof(st->path), PROC_DIR "/sys/kernel/%s", pfiles[i].name);
            else if (pfiles[i].where == IN_INOTIFY)
                snprintf(st->path, sizeof(st->path), PROC_DIR "/sys/fs/inotify/%s",
                         pfiles[i].name);
            else if (a->tid)
                snprintf(st->path, sizeof(st->path), PROC_DIR "/%d/task/%u/%s", (int)a->pid,
                         (unsigned)a->tid, pfiles[i].name);
            else
                snprintf(st->path, sizeof(st->path), PROC_DIR "/%d/%s", (int)a->pid,
                         pfiles[i].name);
        }
    st->pid = a->pid;
    st->tid = a->tid;
    uint32_t status = O_RDONLY | ((uint32_t)flags & (O_NONBLOCK | O_PATH));
    return kind_install(&pfile_ops, st, status, flags & O_CLOEXEC);   /* closes st if it fails */
}

static long open_exe(const struct path *p, long flags)
{
    if (p->must_dir || (flags & O_DIRECTORY))
        return -ENOTDIR;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return -ETXTBSY;   /* the running program's file: Linux's answer */
    const char *e = proc_exe();
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
    char *t = jam_malloc(PATH_BYTES);
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
    jam_free(t);
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
    if (is_file(a.node))
        return open_file(&a, p, flags);
    if (a.node == N_FDDIR && a.pid != lx_pid() && (r = lx_child_fds_check(a.pid)))
        return r == -ESRCH ? -ENOENT : r;   /* a child we can't ask: Linux's EACCES for another's */
    return open_dir(a.node, a.pid, a.tid, flags);
}

/* ---- the directories' kind ------------------------------------------------------- */

static enum node node_of(struct fd_desc *d)
{
    return (enum node)((uintptr_t)d->priv & 0xff);
}

static int32_t pid_of(struct fd_desc *d)
{
    return (int32_t)(((uintptr_t)d->priv >> 8) & 0xffffff);
}

static uint32_t tid_of(struct fd_desc *d)
{
    return (uint32_t)((uintptr_t)d->priv >> 32);
}

/* /proc/self/fd's entry at pos (2 or more): 1 and *e, or 0 at the end. A
 * child's (snap: the bitmap it answered with) is the first descriptor in
 * it at or past pos - 2. */
static int fd_entry(int64_t pos, const uint8_t *snap, struct pentry *e)
{
    long fd = -1;
    if (snap) {
        for (long n = pos - 2; n < FD_MAX && fd < 0; n++)
            if (snap[n / 8] & (1u << (n % 8)))
                fd = n;
    } else {
        struct fd_desc *d;
        bool cloexec;
        fd = pos - 2 < FD_MAX ? fd_next(pos - 2, &d, &cloexec) : -1;
        if (fd >= 0)
            fd_put(d);
    }
    if (fd < 0)
        return 0;
    snprintf(e->name, NAME_LEN, "%ld", fd);
    e->ino = INO_FD + (uint64_t)fd;
    e->type = DT_LNK;
    e->next = 2 + fd + 1;
    return 1;
}

/* One entry of a listing: name, node and type. */
static void entry(struct pentry *e, const char *name, uint64_t ino, uint8_t type)
{
    snprintf(e->name, NAME_LEN, "%s", name);
    e->ino = ino;
    e->type = type;
}

/* /proc's own entries: "self", our pid, our children (KIDS_LISTED at most,
 * in the order they were made), then the files. k counts from 0. */
static int root_entry(int64_t k, struct pentry *e)
{
    int32_t kids[KIDS_LISTED];
    unsigned nk = lx_child_pids(kids, KIDS_LISTED);
    if (nk > KIDS_LISTED)
        nk = KIDS_LISTED;
    char num[NAME_LEN];
    if (k == 0) {
        entry(e, "self", ino_of(N_SELFLINK, 0), DT_LNK);
    } else if (k < 2 + (int64_t)nk) {
        int32_t pid = k == 1 ? lx_pid() : kids[k - 2];
        snprintf(num, sizeof(num), "%d", (int)pid);
        entry(e, num, ino_of(N_SELF, pid), DT_DIR);
    } else {   /* the files, then "sys" */
        int64_t j = k - 2 - (int64_t)nk;
        for (unsigned i = 0; i < PFILES; i++)
            if (pfiles[i].where == IN_ROOT && !j--) {
                entry(e, pfiles[i].name, ino_of(pfiles[i].node, 0), DT_REG);
                return 1;
            }
        if (j)
            return 0;
        entry(e, "sys", ino_of(N_SYSDIR, 0), DT_DIR);
    }
    return 1;
}

/* The entry k of a /proc/sys directory (where: its files): its file. */
static int sys_entry(enum where where, int64_t k, struct pentry *e)
{
    for (unsigned i = 0; i < PFILES; i++)
        if (pfiles[i].where == where && !k--) {
            entry(e, pfiles[i].name, ino_of(pfiles[i].node, 0), DT_REG);
            return 1;
        }
    return 0;
}

/* /proc/<pid>/task's entry k: our k'th thread, by id. */
static int task_entry(int64_t k, struct pentry *e)
{
    uint32_t t;
    char num[NAME_LEN];
    if (k >= UINT32_MAX || thread_tids(&t, (unsigned)k, 1) <= (unsigned)k)
        return 0;
    snprintf(num, sizeof(num), "%u", (unsigned)t);
    entry(e, num, ino_of(N_TASK, (int)t), DT_DIR);
    return 1;
}

/* A process's or thread's directory's entry k (those of pid_names that
 * whose has). */
static int pid_entry(unsigned who, int32_t pid, uint32_t tid, int64_t k, struct pentry *e)
{
    for (unsigned i = 0; i < PID_NAMES; i++)
        if ((pid_names[i].who & who) && !k--) {
            entry(e, pid_names[i].name, ino_of(pid_names[i].node, tid ? (int)tid : pid),
                  pid_names[i].type);
            return 1;
        }
    return 0;
}

/* The inode of a directory's "..". */
static uint64_t dir_up(enum node node, int32_t pid)
{
    switch (node) {
    case N_ROOT:      return INO_ROOT;
    case N_SELF:      return ino_of(N_ROOT, 0);
    case N_TASKDIR:   return ino_of(N_SELF, pid);
    case N_TASK:      return ino_of(N_TASKDIR, pid);
    case N_SYSDIR:    return ino_of(N_ROOT, 0);
    case N_SYSKERNEL: return ino_of(N_SYSDIR, 0);
    case N_SYSFS:     return ino_of(N_SYSDIR, 0);
    case N_SYSINOTIFY: return ino_of(N_SYSFS, 0);
    default:         return ino_of(N_SELF, pid);   /* fd */
    }
}

/* The entry at pos of directory node (of process pid and thread tid, for
 * the process directories, N_FDDIR and N_TASKDIR): 1 and *e, or 0 at the
 * end. Listing positions: 0 ".", 1 "..", then 2 + k: entry k of the
 * directory, and for /proc/self/fd the first open descriptor >= k (so one
 * opened or closed between two calls shows or not as the table is then).
 * snap: for a child's N_FDDIR, the bitmap of its descriptors, else NULL. */
static int entry_at(enum node node, int32_t pid, uint32_t tid, int64_t pos, const uint8_t *snap,
                    struct pentry *e)
{
    e->next = pos + 1;
    if (pos < 2) {
        entry(e, pos ? ".." : ".", pos ? dir_up(node, pid) : ino_of(node, tid ? (int)tid : pid),
              DT_DIR);
        return 1;
    }
    switch (node) {
    case N_FDDIR:      return fd_entry(pos, snap, e);
    case N_ROOT:       return root_entry(pos - 2, e);
    case N_SYSDIR:
        if (pos > 3)
            return 0;
        entry(e, pos == 2 ? "kernel" : "fs", ino_of(pos == 2 ? N_SYSKERNEL : N_SYSFS, 0), DT_DIR);
        return 1;
    case N_SYSKERNEL:  return sys_entry(IN_KERNEL, pos - 2, e);
    case N_SYSFS:
        if (pos != 2)
            return 0;
        entry(e, "inotify", ino_of(N_SYSINOTIFY, 0), DT_DIR);
        return 1;
    case N_SYSINOTIFY: return sys_entry(IN_INOTIFY, pos - 2, e);
    case N_TASKDIR:    return task_entry(pos - 2, e);
    default:
        return pid_entry(tid ? WHO_TASK : pid == lx_pid() ? WHO_SELF : WHO_KID, pid, tid,
                         pos - 2, e);
    }
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
    /* A child's descriptors: asked once for the whole call. */
    uint8_t kid_fds[FD_MAX / 8];
    const uint8_t *snap = NULL;
    if (node_of(d) == N_FDDIR && pid_of(d) != lx_pid()) {
        r = lx_child_fds(pid_of(d), kid_fds);
        if (r < 0)
            return r;
        snap = kid_fds;
    }
    while ((r = entry_at(node_of(d), pid_of(d), tid_of(d), pos, snap, &e)) > 0) {
        long k = dir_put((uint8_t *)buf + used, len - used, e.name, e.ino, e.type, e.next);
        if (k <= 0) {
            r = used ? 0 : k ? k : -EINVAL;   /* the buffer can't hold the next one */
            break;
        }
        used += (size_t)k;
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
    node_stat(node_of(d), tid_of(d) ? (int)tid_of(d) : pid_of(d), st);
    return 0;
}

/* No .give: a child left out of the hand-over gets none (its own /proc is
 * its own, and a directory record would name a mount). No .path: the *at
 * calls and fchdir take mount directories only. */
static const struct fd_ops procdir_ops = {
    .name = "/proc directory", .read = pd_read, .seek = pd_seek, .stat = pd_stat,
    .getdents = pd_getdents,
};

/* ---- the generated files' kind ----------------------------------------------------- */

static struct pfile_state *pf_of(struct fd_desc *d)
{
    return d->priv;
}

/* Make the text again (or the first time): outside d's lock, since it asks
 * the kernel and the descriptor table (fd.h's lock order). On success the
 * new text is the file's, the old one freed. */
static long pf_fill(struct fd_desc *d)
{
    struct pfile_state *s = pf_of(d);
    char *text;
    size_t len;
    long r = procinfo_text(s->file, s->pid, s->tid, &text, &len);
    if (r)
        return r;
    lock_take(&d->lock);
    jam_free(s->text);
    s->text = text;
    s->len = len;
    lock_give(&d->lock);
    return 0;
}

/* /proc/self/mem: the offset is an address, and a read gives the memory
 * there, a page at a time through the checked copy (ucopy.h). The first
 * page that can't be read ends the read: EIO if nothing was read, as
 * Linux's. Never written (open refuses), since a write through here would
 * pass the pages' permissions. */
static long mem_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    lock_take(&d->lock);
    int64_t off = pos == FD_AT_OFFSET ? d->offset : pos;
    lock_give(&d->lock);
    if (off < 0)
        return -EINVAL;
    size_t want = (size_t)n < MEM_MAX ? (size_t)n : MEM_MAX, done = 0;
    uint8_t chunk[MEM_STEP];
    while (done < want) {
        uint64_t at = (uint64_t)off + done;
        size_t k = MEM_STEP - (size_t)(at & (MEM_STEP - 1));
        if (k > want - done)
            k = want - done;
        if (uc_get(chunk, (const void *)(uintptr_t)at, k))
            break;
        if (fd_iov_scatter(iov, iovcnt, done, chunk, k))
            return -EFAULT;
        done += k;
    }
    if (!done)
        return -EIO;
    if (pos == FD_AT_OFFSET) {
        lock_take(&d->lock);
        d->offset += (int64_t)done;
        lock_give(&d->lock);
    }
    return (long)done;
}

static long pf_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    struct pfile_state *s = pf_of(d);
    if (s->file == PF_MEM)
        return mem_read(d, iov, iovcnt, pos);
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    lock_take(&d->lock);
    uint64_t off = pos == FD_AT_OFFSET ? (uint64_t)d->offset : (uint64_t)pos;
    bool fresh = off == 0 || !s->text;   /* a read from the start starts a pass */
    lock_give(&d->lock);
    long r = fresh ? pf_fill(d) : 0;
    if (r)
        return r;
    lock_take(&d->lock);
    off = pos == FD_AT_OFFSET ? (uint64_t)d->offset : (uint64_t)pos;
    size_t k = off >= s->len ? 0 : s->len - off < (size_t)n ? s->len - (size_t)off : (size_t)n;
    if (fd_iov_scatter(iov, iovcnt, 0, s->text + (k ? off : 0), k)) {
        lock_give(&d->lock);
        return -EFAULT;
    }
    if (pos == FD_AT_OFFSET)
        d->offset += (int64_t)k;
    lock_give(&d->lock);
    return (long)k;
}

static long pf_seek(struct fd_desc *d, int64_t off, int whence)
{
    struct pfile_state *s = pf_of(d);
    if (whence == SEEK_END && s->file == PF_MEM)
        return -EINVAL;   /* the memory has no end */
    if (whence == SEEK_END) {   /* the length of the text now */
        long r = pf_fill(d);
        if (r)
            return r;
    }
    lock_take(&d->lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->offset :
                   whence == SEEK_END ? (int64_t)s->len : -1;
    int64_t to;
    long r = base < 0 || __builtin_add_overflow(base, off, &to) || to < 0 ? -EINVAL : 0;
    if (!r)
        d->offset = to;
    lock_give(&d->lock);
    return r ? r : (long)to;
}

static long pf_stat(struct fd_desc *d, struct stat *st)
{
    enum node node = 0;
    for (unsigned i = 0; i < PFILES; i++)
        if (pfiles[i].file == pf_of(d)->file)
            node = pfiles[i].node;
    node_stat(node, pf_of(d)->tid ? (int)pf_of(d)->tid : pf_of(d)->pid, st);
    return 0;
}

static void pf_close(struct fd_desc *d)
{
    jam_free(pf_of(d)->text);
    jam_free(d->priv);
}

/* No .give: a child left out of the hand-over opens its own. Always ready
 * (no .arm), as a file. */
static const struct fd_ops pfile_ops = {
    .name = "/proc file", .read = pf_read, .seek = pf_seek, .stat = pf_stat, .close = pf_close,
};
