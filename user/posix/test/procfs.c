/* posixtest's procfs section (docs/CR-PLAN.md section 3): libc's /proc.
 *
 *   - /proc/self/exe: readlink gives the boot image's path (and the
 *     buffer's edges: cut short, no NUL, never past size), open reads the
 *     file (ETXTBSY to write it), stat follows it, lstat doesn't;
 *   - /proc, /proc/self and /proc/<pid>; another pid and Linux's other
 *     files are ENOENT for every call, and nothing can be made there;
 *   - /proc/self/fd: its listing against the table, descriptors opened
 *     and closed between two getdents calls, a seek back; readlink of each
 *     kind of descriptor (an inotify one is `anon_inode:inotify`);
 *   - /proc/sys/fs/inotify (INOTIFY-PLAN 3.5): the directories and the
 *     three files (max_user_watches, max_user_instances, max_queued_events:
 *     read only, the numbers the services allow), the Nth inotify
 *     descriptor past max_user_instances EMFILE (a dup is not another), the
 *     Nth watch past max_user_watches ENOSPC (a removed one makes room);
 *   - reopening /proc/self/fd/N: a file (a description of its own), a
 *     memfd read-only (no write, no writable shared map, no seal: and in
 *     a child, neither a map nor a reopen gets write back), a pipe end, a
 *     socket's and an eventfd's ENXIO;
 *   - children started with other argv[0]s (a name that isn't a program,
 *     another program's path, /bin's): their exe is still this program's
 *     file (the path spawn gives in the hand-over), never taken from the
 *     name;
 *   - the self-image (docs/CR-PLAN.md section 6): posix_spawn of
 *     /proc/self/exe starts this program again, the child's exe our path,
 *     its SR_SELF_IMAGE our very VMO (none for a boot-image program),
 *     never writable, and every page of its code touched adds no page to
 *     the job. Asked for by name, `self-image <path>` checks the same from
 *     a copy on /data (the shell's run, and init's /svc/exec: posixtest
 *     spawn-data), which must have its image. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <fswatch.h>
#include <jamos/libjam.h>
#include <spawn.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "pt.h"

#define EXE    "/boot/bin/posixtest"   /* this program's file, as /proc/self/exe names it */
#define FD_TOP 64                      /* descriptors looked at by fcntl */
#define SMALL  48                      /* getdents bytes: two records of a short name */
#define PG     4096ull

extern char **environ;
/* Our own ELF header, mapped (user/posix/posix.ld): the file's first bytes. */
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

/* /proc/self/fd/<fd> into path (32 bytes). */
static void fd_path(char *path, int fd)
{
    snprintf(path, 32, "/proc/self/fd/%d", fd);
}

/* Is fd's link want (or does it start with want)? */
static bool link_is(int fd, const char *want, bool prefix)
{
    char path[32], buf[256];
    fd_path(path, fd);
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0)
        return false;
    buf[n] = '\0';
    return prefix ? !strncmp(buf, want, strlen(want)) : !strcmp(buf, want);
}

/* ---- /proc/self/exe ------------------------------------------------------------------- */

static void check_exe_link(void)
{
    char buf[256];
    ssize_t len = (ssize_t)strlen(EXE);
    memset(buf, '#', sizeof(buf));
    CHECK(readlink("/proc/self/exe", buf, sizeof(buf)) == len && !memcmp(buf, EXE, (size_t)len) &&
          buf[len] == '#');
    /* the buffer's edges: cut short, no NUL written, never an error */
    memset(buf, '#', sizeof(buf));
    CHECK(readlink("/proc/self/exe", buf, 1) == 1 && buf[0] == '/' && buf[1] == '#');
    memset(buf, '#', sizeof(buf));
    CHECK(readlink("/proc/self/exe", buf, (size_t)len - 1) == len - 1 &&
          !memcmp(buf, EXE, (size_t)len - 1) && buf[len - 1] == '#');
    memset(buf, '#', sizeof(buf));
    CHECK(readlink("/proc/self/exe", buf, (size_t)len) == len && !memcmp(buf, EXE, (size_t)len) &&
          buf[len] == '#');
    /* musl's readlink turns a size of 0 into a 1-byte read and answers 0;
     * the call itself (Linux: EINVAL) is the raw one. */
    errno = 0;
    CHECK(readlink("/proc/self/exe", buf, 0) == 0);
    errno = 0;
    CHECK(syscall(SYS_readlink, "/proc/self/exe", buf, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(readlink("/proc/self/exe/", buf, sizeof(buf)) == -1 && errno == ENOTDIR);
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)getpid());
    CHECK(readlink(path, buf, sizeof(buf)) == len && !memcmp(buf, EXE, (size_t)len));
    /* relative to the current directory there */
    char cwd[256];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL && chdir("/proc/self") == 0);
    CHECK(readlink("exe", buf, sizeof(buf)) == len && !memcmp(buf, EXE, (size_t)len));
    CHECK(chdir(cwd) == 0);
}

static void check_exe_file(void)
{
    struct stat st, boot, fst;
    CHECK(lstat("/proc/self/exe", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(stat("/proc/self/exe", &st) == 0 && S_ISREG(st.st_mode));
    CHECK(stat(EXE, &boot) == 0 && boot.st_ino == st.st_ino && boot.st_size == st.st_size);
    CHECK(access("/proc/self/exe", X_OK) == 0);
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    char head[64];
    CHECK(fd >= 0 && read(fd, head, sizeof(head)) == (ssize_t)sizeof(head) &&
          !memcmp(head, __ehdr_start, sizeof(head)));
    CHECK(fd >= 0 && fstat(fd, &fst) == 0 && fst.st_size == boot.st_size);
    CHECK(fd >= 0 && link_is(fd, EXE, false));   /* what was opened is the file itself */
    if (fd >= 0)
        close(fd);
    errno = 0;
    CHECK(open("/proc/self/exe", O_WRONLY) == -1 && errno == ETXTBSY);
    errno = 0;
    CHECK(open("/proc/self/exe", O_RDWR) == -1 && errno == ETXTBSY);
    errno = 0;
    CHECK(open("/proc/self/exe", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    errno = 0;
    CHECK(open("/proc/self/exe", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
}

/* ---- /proc and /proc/self -------------------------------------------------------------- */

/* Is name in dir's listing (readdir)? */
static bool listed(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    bool seen = false;
    struct dirent *e;
    while (d && (e = readdir(d)))
        seen |= !strcmp(e->d_name, name);
    if (d)
        closedir(d);
    return seen;
}

/* Linux's files we don't have: ENOENT whatever is asked. */
static void check_missing(void)
{
    static const char *const gone[] = {
        "/proc/uptime", "/proc/self/smaps", "/proc/self/task/0", "/proc/self/environ",
        "/proc/self/ns/user", "/proc/sys/kernel/nosuch", "/proc/sys/vm", "/proc/self/fd/01",
        "/proc/self/fd/x", "/proc/self/exe2",
    };
    char buf[64];
    struct stat st;
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        errno = 0;
        CHECK(open(gone[i], O_RDONLY) == -1 && errno == ENOENT);
        errno = 0;
        CHECK(stat(gone[i], &st) == -1 && errno == ENOENT);
        errno = 0;
        CHECK(readlink(gone[i], buf, sizeof(buf)) == -1 && errno == ENOENT);
        errno = 0;
        CHECK(access(gone[i], F_OK) == -1 && errno == ENOENT);
    }
    errno = 0;
    CHECK(fopen("/proc/self/smaps", "r") == NULL && errno == ENOENT);
    char other[32];
    int n = (int)getpid() + 1;   /* only we are here: a number that is nobody's of ours (a thread's counts) */
    for (;; n++) {
        snprintf(other, sizeof(other), "/proc/self/task/%d", n);
        if (stat(other, &st) == -1)
            break;
    }
    snprintf(other, sizeof(other), "/proc/%d", n);
    errno = 0;
    CHECK(stat(other, &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(open("/proc/self/new", O_CREAT | O_WRONLY, 0600) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(mkdir("/proc/x", 0700) == -1 && errno == EACCES);
    errno = 0;
    CHECK(unlink("/proc/self/exe") == -1 && errno == EACCES);
}

static void check_self(void)
{
    char want[16], buf[32], path[48];
    snprintf(want, sizeof(want), "%d", (int)getpid());
    ssize_t n = readlink("/proc/self", buf, sizeof(buf));
    CHECK(n == (ssize_t)strlen(want) && !memcmp(buf, want, strlen(want)));
    struct stat st;
    CHECK(lstat("/proc/self", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(stat("/proc/self", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("/proc", &st) == 0 && S_ISDIR(st.st_mode));
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)getpid());
    CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    errno = 0;
    CHECK(readlink("/proc/self/fd", buf, sizeof(buf)) == -1 && errno == EINVAL);   /* a directory */
    errno = 0;
    CHECK(readlink("/proc", buf, sizeof(buf)) == -1 && errno == EINVAL);
    CHECK(listed("/proc", "self") && listed("/proc", want) && listed("/proc", ".."));
    CHECK(listed("/proc/self", "exe") && listed("/proc/self", "fd") && listed("/proc/self", "cwd") &&
          !listed("/proc/self", "x"));
    CHECK(listed("/proc/self", "task") && listed("/proc/self", "cmdline") && listed("/proc/self", "mem"));
    CHECK(!listed("/proc/self", "ns"));   /* no user namespaces (base's check of ns/user says so) */
    errno = 0;
    CHECK(open("/proc/self", O_WRONLY) == -1 && errno == EISDIR);
}

/* ---- /proc/self/fd ---------------------------------------------------------------------- */

/* The listing (readdir) is exactly the open descriptors, its own too. */
static void check_fd_listing(void)
{
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != NULL);
    if (!d)
        return;
    bool seen[FD_TOP] = { false };
    int other = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        int n = atoi(e->d_name);
        CHECK(e->d_type == DT_LNK);
        if (n >= 0 && n < FD_TOP)
            seen[n] = true;
        else
            other++;
    }
    bool agree = true;   /* the listing and the table */
    for (int fd = 0; fd < FD_TOP; fd++)
        agree &= seen[fd] == (fcntl(fd, F_GETFD) != -1);
    CHECK(agree && seen[dirfd(d)] && other == 0);
    closedir(d);
}

/* getdents into a small buffer, the names gathered: true for each fd
 * below FD_TOP listed; the d_off after the entry named at, into *at_off. */
static void gather(int dfd, bool seen[FD_TOP], int at, long *at_off)
{
    struct dirent store[1];   /* asked for SMALL bytes of it at a time */
    const char *buf = (const char *)store;
    int n;
    while ((n = getdents(dfd, store, SMALL)) > 0)
        for (int k = 0; k < n;) {
            const struct dirent *e = (const struct dirent *)(buf + k);
            int fd = e->d_name[0] == '.' ? -1 : atoi(e->d_name);
            if (fd >= 0 && fd < FD_TOP)
                seen[fd] = true;
            if (fd == at)
                *at_off = (long)e->d_off;
            k += e->d_reclen;
        }
    CHECK(n == 0);
}

/* Descriptors opened and closed between two getdents calls show as the
 * table is at each call; a seek back lists again. */
static void check_fd_between(void)
{
    int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int a = dup(1), b = dup(1), c = dup(1);
    struct dirent e[1];
    CHECK(dfd >= 0 && a > 0 && b > a && c > b && c < 60);
    CHECK(getdents(dfd, e, SMALL) == SMALL && !strcmp(e->d_name, "."));   /* "." and ".." */
    close(b);
    CHECK(dup2(1, 60) == 60);
    bool seen[FD_TOP] = { false };
    long off_a = -1;
    gather(dfd, seen, a, &off_a);
    CHECK(seen[a] && !seen[b] && seen[c] && seen[60] && seen[dfd] && off_a > 0);
    errno = 0;
    CHECK(getdents(dfd, e, SMALL) == 0);   /* at the end */
    /* b's number again, behind the position: seen only from the start */
    CHECK(dup2(1, b) == b && lseek(dfd, 0, SEEK_SET) == 0);
    errno = 0;
    CHECK(getdents(dfd, e, 10) == -1 && errno == EINVAL);   /* not even one record fits */
    memset(seen, 0, sizeof(seen));
    gather(dfd, seen, -2, &off_a);
    CHECK(seen[a] && seen[b] && seen[c] && seen[60]);
    /* back to just after a: a isn't listed again, what follows is */
    memset(seen, 0, sizeof(seen));
    CHECK(lseek(dfd, off_a, SEEK_SET) == off_a);
    gather(dfd, seen, -2, &off_a);
    CHECK(!seen[a] && seen[b] && seen[c] && seen[60]);
    close(a);
    close(b);
    close(c);
    close(60);
    close(dfd);
}

/* readlink of each kind of descriptor. */
static void check_fd_links(void)
{
    int f = open(EXE, O_RDONLY | O_CLOEXEC), dir = open("/boot", O_RDONLY | O_DIRECTORY);
    int p[2] = { -1, -1 }, s[2] = { -1, -1 };
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    int ev = eventfd(0, 0), ep = epoll_create1(0), m = memfd_create("ptproc", 0);
    int null = open("/dev/null", O_RDONLY), pw = open("/etc/passwd", O_RDONLY);
    CHECK(link_is(f, EXE, false) && link_is(dir, "/boot", false));
    CHECK(link_is(p[0], "pipe:[", true) && link_is(p[1], "pipe:[", true));
    CHECK(link_is(s[0], "socket:[", true) && link_is(s[1], "socket:[", true));
    CHECK(link_is(ev, "anon_inode:[eventfd]", false));
    CHECK(link_is(ep, "anon_inode:[eventpoll]", false));
    int in = inotify_init1(0);
    CHECK(in >= 0 && link_is(in, "anon_inode:inotify", false));   /* Linux's, no brackets */
    CHECK(link_is(m, "/memfd:ptproc (deleted)", false));
    CHECK(link_is(null, "/dev/null", false) && link_is(pw, "/etc/passwd", false));
    char path[32], buf[8];
    struct stat ls, st, fst;
    fd_path(path, f);
    CHECK(lstat(path, &ls) == 0 && S_ISLNK(ls.st_mode));
    CHECK(stat(path, &st) == 0 && fstat(f, &fst) == 0 && st.st_ino == fst.st_ino);
    close(pw);
    fd_path(path, pw);
    errno = 0;
    CHECK(readlink(path, buf, sizeof(buf)) == -1 && errno == ENOENT);   /* closed: gone */
    int fds[] = { f, dir, p[0], p[1], s[0], s[1], ev, ep, in, m, null };
    for (unsigned i = 0; i < sizeof(fds) / sizeof(fds[0]); i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* ---- /proc/sys/fs/inotify --------------------------------------------------------------- */

/* The number in /proc/sys/fs/inotify/<name> ("N\n"), or -1. */
static long ino_limit(const char *name)
{
    char path[64], buf[32];
    snprintf(path, sizeof(path), "/proc/sys/fs/inotify/%s", name);
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n < 2)
        return -1;
    buf[n] = '\0';
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf + n - 1 && *end == '\n' && v > 0 ? v : -1;
}

/* Does directory dir list exactly want (a NULL-ended list, any order, "."
 * and ".." aside)? */
static bool lists_only(const char *dir, const char *const *want)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    unsigned n = 0, seen = 0, total = 0;
    while (want[n])
        n++;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        total++;
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(e->d_name, want[i]))
                seen |= 1u << i;
    }
    closedir(d);
    return total == n && seen == (1u << n) - 1;
}

/* The inode directory dir lists for its "..", or 0. */
static ino_t dotdot_ino(const char *dir)
{
    DIR *d = opendir(dir);
    ino_t ino = 0;
    for (struct dirent *e; d && (e = readdir(d));)
        if (!strcmp(e->d_name, ".."))
            ino = e->d_ino;
    if (d)
        closedir(d);
    return ino;
}

/* The three files and their directories as /proc's others are. */
static void check_inotify_files(void)
{
    static const char *const names[] = { "max_user_watches", "max_user_instances",
                                         "max_queued_events", NULL };
    static const char *const sys_list[] = { "kernel", "fs", NULL };
    static const char *const fs_list[] = { "inotify", NULL };
    CHECK(lists_only("/proc/sys", sys_list) && lists_only("/proc/sys/fs", fs_list));
    CHECK(lists_only("/proc/sys/fs/inotify", names));
    struct stat st, up;
    const char *const dirs[] = { "/proc/sys/fs", "/proc/sys/fs/inotify" };
    const char *const parents[] = { "/proc/sys", "/proc/sys/fs" };
    for (unsigned i = 0; i < 2; i++) {
        CHECK(stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
              (st.st_mode & 0777) == 0555);
        CHECK(stat(parents[i], &up) == 0 && dotdot_ino(dirs[i]) == up.st_ino);
    }
    for (unsigned i = 0; names[i]; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/sys/fs/inotify/%s", names[i]);
        CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
              (st.st_mode & 0777) == 0444 && st.st_size == 0);
        CHECK(ino_limit(names[i]) > 0);
        errno = 0;
        CHECK(open(path, O_WRONLY) == -1 && errno == EACCES);
        strcat(path, "/x");
        errno = 0;
        CHECK(stat(path, &st) == -1 && errno == ENOTDIR);
    }
    errno = 0;
    CHECK(stat("/proc/sys/fs/nosuch", &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(stat("/proc/sys/fs/inotify/nosuch", &st) == -1 && errno == ENOENT);
    /* What the services allow (user/include/fswatch.h, fswatch.idl): a
     * process's watches on a volume, its channels there, and a queue of
     * 16 KiB of records of an 18-byte name. */
    CHECK(ino_limit("max_user_watches") == FSW_PROCESS_WATCHES);
    CHECK(ino_limit("max_user_instances") == FSW_PER_PROCESS);
    CHECK(ino_limit("max_queued_events") == FSWATCH_QUEUE_BYTES / (FSWATCH_RECORD_FIXED + 18));
}

/* Descriptors past max_user_instances are EMFILE, and a dup is not another. */
static void check_inotify_instances(long limit)
{
    enum { SLOTS = 64 };
    int fds[SLOTS], dups[SLOTS], n = 0, err = 0;
    CHECK(limit > 0 && limit < SLOTS);
    for (; n < SLOTS; n++) {
        fds[n] = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fds[n] < 0) {
            err = errno;
            break;
        }
    }
    CHECK(n == limit && err == EMFILE);
    errno = 0;
    CHECK(inotify_init() == -1 && errno == EMFILE);
    while (n > 0 && n > limit - 1)
        close(fds[--n]);   /* one place free */
    int nd = 0;
    for (int i = 0; i < n; i++)
        dups[nd++] = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
    int last = inotify_init();   /* the dups are the same descriptions */
    CHECK(last >= 0);
    errno = 0;
    CHECK(inotify_init() == -1 && errno == EMFILE);
    if (last >= 0)
        close(last);
    for (int i = 0; i < nd; i++)
        if (dups[i] >= 0)
            close(dups[i]);
    for (int i = 0; i < n; i++)
        close(fds[i]);
    int again = inotify_init();   /* all closed: room again */
    CHECK(again >= 0);
    if (again >= 0)
        close(again);
}

#define LIM_PATHS  1500
#define LIM_LEN    64
static char lim_path[LIM_PATHS][LIM_LEN];
static unsigned lim_n;

static void lim_add(const char *dir, const char *name)
{
    if (lim_n < LIM_PATHS && snprintf(lim_path[lim_n], LIM_LEN, "%s%s", dir, name) < LIM_LEN)
        lim_n++;
}

/* A process's watches past max_user_watches are ENOSPC, all its
 * descriptors together (Linux's is a user's: IR's 5), and a removed one
 * makes room. Quiet watches (nothing changes under them) of distinct
 * paths: libc's trees, the boot image's programs and one /proc/self/fd/N
 * for each of as many descriptors as the table has room for; the first
 * half through one descriptor, the rest through a second. */
static void check_inotify_watches(long limit)
{
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int in2 = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(in >= 0 && in2 >= 0 && null >= 0);
    static const char *const fixed[] = { "/", "/etc", "/dev", "/boot", "/boot/bin", "/proc",
                                         "/proc/self", "/proc/self/fd", "/proc/self/task",
                                         "/proc/sys", "/proc/sys/kernel", "/proc/sys/fs",
                                         "/proc/sys/fs/inotify" };
    lim_n = 0;
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
        lim_add("", fixed[i]);
    DIR *d = opendir("/boot/bin");
    for (struct dirent *e; d && (e = readdir(d));)
        if (e->d_name[0] != '.')
            lim_add("/boot/bin/", e->d_name);
    if (d)
        closedir(d);
    int kept[LIM_PATHS], nk = 0;
    for (int fd; lim_n < LIM_PATHS && (fd = fcntl(null, F_DUPFD_CLOEXEC, 3)) >= 0;) {
        char num[16];
        snprintf(num, sizeof(num), "%d", fd);
        lim_add("/proc/self/fd/", num);
        kept[nk++] = fd;
    }
    unsigned ok = 0;
    int err = 0, first = -1;
    for (; ok < lim_n && in >= 0 && in2 >= 0; ok++) {
        int wd = inotify_add_watch(ok < (unsigned)limit / 2 ? in : in2, lim_path[ok],
                                   IN_ATTRIB | IN_DONT_FOLLOW);
        if (wd < 0) {
            err = errno;
            break;
        }
        if (first < 0)
            first = wd;
    }
    if (lim_n <= (unsigned)limit) {
        printf("posixtest: procfs: only %u paths to watch here, max_user_watches is %ld: "
               "its ENOSPC is left unchecked\n", lim_n, limit);
        CHECK(ok == lim_n);
    } else {
        if (ok != (unsigned)limit || err != ENOSPC)
            printf("posixtest: procfs: %u watches, then errno %d (max_user_watches %ld)\n", ok, err,
                   limit);
        CHECK(ok == (unsigned)limit && err == ENOSPC);
        char buf[512];
        CHECK(inotify_rm_watch(in, first) == 0);
        while (read(in, buf, sizeof(buf)) > 0)
            ;
        CHECK(inotify_add_watch(in2, lim_path[ok], IN_ATTRIB | IN_DONT_FOLLOW) > 0);
        errno = 0;
        CHECK(ok + 1 >= lim_n ||
              (inotify_add_watch(in, lim_path[ok + 1], IN_ATTRIB | IN_DONT_FOLLOW) == -1 &&
               errno == ENOSPC));
    }
    for (int i = 0; i < nk; i++)
        close(kept[i]);
    close(null);
    if (in >= 0)
        close(in);
    if (in2 >= 0)
        close(in2);
}

static void check_inotify_limits(void)
{
    check_inotify_files();
    long watches = ino_limit("max_user_watches"), instances = ino_limit("max_user_instances");
    if (instances > 0)
        check_inotify_instances(instances);
    if (watches > 0)
        check_inotify_watches(watches);
}

/* ---- reopening -------------------------------------------------------------------------- */

/* posixtest --child-procfs <what>: run with argv[0] as given. */
static int spawn_child(const char *argv0, const char *what, int give)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (give >= 0)
        posix_spawn_file_actions_adddup2(&fa, give, 3);
    char *argv[] = { (char *)argv0, "--child-procfs", (char *)what, NULL };
    pid_t pid = -1;
    int status = -1;
    int e = posix_spawn(&pid, "/bin/posixtest", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    CHECK(e == 0 && waitpid(pid, &status, 0) == pid);
    return status;
}

/* A memfd reopened read-only: Chromium's twin of a shared memory region. */
static void check_twin(void)
{
    char path[32], buf[4];
    int m = memfd_create("pttwin", MFD_ALLOW_SEALING);
    CHECK(m >= 0 && ftruncate(m, 4096) == 0 && write(m, "twin", 4) == 4);
    fd_path(path, m);
    int r = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(r >= 0 && r != m && read(r, buf, 4) == 4 && !memcmp(buf, "twin", 4));
    errno = 0;
    CHECK(write(r, "x", 1) == -1 && errno == EBADF);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, r, 0) == MAP_FAILED &&
          errno == EACCES);
    errno = 0;
    CHECK(fcntl(r, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);
    char *ro = mmap(NULL, 4096, PROT_READ, MAP_SHARED, r, 0);
    CHECK(ro != MAP_FAILED && !memcmp(ro, "twin", 4));
    CHECK(pwrite(m, "TWIN", 4, 0) == 4 && ro != MAP_FAILED && !memcmp(ro, "TWIN", 4));
    struct stat sm, sr;
    CHECK(fstat(m, &sm) == 0 && fstat(r, &sr) == 0 && sm.st_ino == sr.st_ino);
    int w = open(path, O_RDWR | O_CLOEXEC);   /* the writable one opens writable */
    CHECK(w >= 0 && pwrite(w, "T", 1, 0) == 1);
    CHECK(spawn_child("posixtest", "twin", r) == 0);   /* nothing gets write back in a child */
    if (ro != MAP_FAILED)
        munmap(ro, 4096);
    close(w);
    close(r);
    close(m);
}

static void check_reopen(void)
{
    char path[32], buf[4];
    int f = open(EXE, O_RDONLY | O_CLOEXEC);
    CHECK(f >= 0 && lseek(f, 100, SEEK_SET) == 100);
    fd_path(path, f);
    int g = open(path, O_RDONLY | O_CLOEXEC);   /* a description of its own */
    CHECK(g >= 0 && g != f && lseek(g, 0, SEEK_CUR) == 0 && lseek(f, 0, SEEK_CUR) == 100);
    CHECK(g >= 0 && read(g, buf, 4) == 4 && !memcmp(buf, "\177ELF", 4));
    close(g);
    close(f);
    check_twin();
    int p[2] = { -1, -1 }, s[2] = { -1, -1 };
    CHECK(pipe(p) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    fd_path(path, p[0]);
    int pr = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(pr >= 0 && write(p[1], "p", 1) == 1 && read(pr, buf, 1) == 1 && buf[0] == 'p');
    errno = 0;
    CHECK(open(path, O_WRONLY) == -1 && errno == EACCES);   /* not a read end's access */
    fd_path(path, s[0]);
    errno = 0;
    CHECK(open(path, O_RDWR) == -1 && errno == ENXIO);
    int ev = eventfd(0, 0);
    fd_path(path, ev);
    errno = 0;
    CHECK(open(path, O_RDWR) == -1 && errno == ENXIO);
    int in = inotify_init();
    fd_path(path, in);
    errno = 0;
    CHECK(in >= 0 && open(path, O_RDONLY) == -1 && errno == ENXIO);
    if (in >= 0)
        close(in);
    int null = open("/dev/null", O_WRONLY);
    fd_path(path, null);
    int n2 = open(path, O_RDWR | O_CLOEXEC);
    CHECK(n2 >= 0 && write(n2, "x", 1) == 1 && read(n2, buf, 1) == 0);
    int fds[] = { pr, p[0], p[1], s[0], s[1], ev, null, n2 };
    for (unsigned i = 0; i < sizeof(fds) / sizeof(fds[0]); i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* ---- the self-image ------------------------------------------------------------------ */

/* Our SR_SELF_IMAGE's object id (0: none) and, if rights isn't NULL, its
 * rights. */
static uint64_t image_koid(rights_t *rights)
{
    handle_t h = startup_handle(SR_SELF_IMAGE);
    struct handle_info info;
    if (!h || jam_handle_info(h, &info) != OK)
        return 0;
    if (rights)
        *rights = info.rights;
    return info.koid;
}

/* A byte of each page of every segment that isn't writable (the code and
 * the constants, mapped from the program's image) read, so each is
 * there; how many pages. */
static uint64_t touch_text(void)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)__ehdr_start;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(const void *)(__ehdr_start + eh->e_phoff);
    uint64_t pages = 0;
    uint8_t sum = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || (ph[i].p_flags & PF_W))
            continue;
        uint64_t lo = ph[i].p_vaddr & ~(PG - 1), hi = (ph[i].p_vaddr + ph[i].p_memsz + PG - 1) &
                                                      ~(PG - 1);
        for (uint64_t a = lo; a < hi; a += PG, pages++)
            sum += *(const volatile uint8_t *)(uintptr_t)a;
    }
    __asm__ volatile("" : : "r"(sum));
    return pages;
}

/* posix_spawn of prog (/proc/self/exe; koid: our image's, 0 for none),
 * the child holding on once it is ready (touch: with every page of its
 * code read): how many pages our job grew by meanwhile. */
static uint64_t self_spawn(const char *prog, const char *want, uint64_t koid, bool touch)
{
    int up[2] = { -1, -1 }, hold[2] = { -1, -1 };
    CHECK(pipe2(up, O_CLOEXEC) == 0 && pipe2(hold, O_CLOEXEC) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, up[1], 3);     /* it says it is ready */
    posix_spawn_file_actions_adddup2(&fa, hold[0], 4);   /* it waits for its end */
    char k[24];
    snprintf(k, sizeof(k), "%llu", (unsigned long long)koid);
    char *argv[] = { "self", "--child-procfs", "image", (char *)want, k, touch ? "touch" : "still",
                     NULL };
    uint64_t before = pt_job_pages();
    pid_t pid = -1;
    int e = posix_spawn(&pid, prog, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(up[1]);
    close(hold[0]);
    char c = 0;
    CHECK(e == 0 && read(up[0], &c, 1) == 1 && c == 'r');
    uint64_t grew = pt_job_pages() - before;
    close(hold[1]);
    int status = -1;
    CHECK(e == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    close(up[0]);
    return grew;
}

/* posix_spawn of /proc/self/exe: this program again (want: our path),
 * from our image. Twice: a child that reads every page of its code grows
 * our job no more than one that doesn't (its own pages, data, stack and
 * heap, are the same in both; the code's are the image's, ours already).
 * Both against the same program started from the boot image, which never
 * copies its code: the child's own pages are most of what any start
 * costs (about 95 against posixtest's 162 of code), so only a measure
 * beside the same child's tells a copy from them. */
static void check_self_spawn(const char *want)
{
    uint64_t text = touch_text(), koid = image_koid(NULL);
    uint64_t base = self_spawn("/bin/posixtest", EXE, 0, false);
    uint64_t still = self_spawn("/proc/self/exe", want, koid, false);
    uint64_t touched = self_spawn("/proc/self/exe", want, koid, true);
    printf("posixtest: procfs: /proc/self/exe started %s again from %s: the job grew %llu "
           "pages, %llu with its %llu pages of code touched (%llu from the boot image)\n", want,
           koid ? "its image" : "the boot image", (unsigned long long)still,
           (unsigned long long)touched, (unsigned long long)text, (unsigned long long)base);
    CHECK(still < base + text / 8);      /* no copy of the code made for it */
    CHECK(touched < base + text / 8);    /* none of the code's pages as it runs */
    CHECK(touched < still + text / 8);
}

/* A child started from our image by libjam's spawn with no POSIX
 * hand-over (the shell's way for a program in the background): spawn
 * names its path beside argv (SR_ARGS), so its /proc/self/exe is want
 * without a look through the file. */
static void check_no_handover(const char *want)
{
    handle_t img = startup_handle(SR_SELF_IMAGE), vmo = HANDLE_INVALID, proc;
    uint64_t size = 0;
    const char *argv[] = { "pt-prog", "--child-procfs", "startup-exe", want };
    static const char *const env[] = { NULL };
    CHECK(img && jam_vmo_get_size(img, &size) == OK &&
          jam_handle_duplicate(img, RIGHT_SAME, &vmo) == OK);
    struct spawn_args a = {
        .path = want, .vmo = vmo, .size = size, .argc = 4, .argv = argv,
        .job = startup_handle(SR_JOB), .envp = env, .stack_size = 8ull << 20,
    };
    struct process_info info = { 0 };
    status_t st = vmo ? spawn(&a, &proc) : ERR_BAD_HANDLE;
    if (vmo)
        jam_handle_close(vmo);
    if (st == OK) {
        st = spawn_wait(proc, 10 * NS_PER_S, &info);
        jam_handle_close(proc);
    }
    printf("posixtest: procfs: a child from our image with no hand-over: %s, code %lld\n",
           status_str(st), (long long)info.exit_code);
    CHECK(st == OK && info.exit_code == 0);
}

/* posixtest self-image <path>, run from path (a copy on /data). */
void pt_self_image(char **argv)
{
    const char *want = NULL;
    for (int i = 1; argv[i]; i++)
        if (!strcmp(argv[i], "self-image") && argv[i + 1])
            want = argv[i + 1];
    CHECK(want != NULL);
    if (!want)
        return;
    char buf[256];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    CHECK(n == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)n));
    rights_t r = 0;
    CHECK(image_koid(&r) && !(r & RIGHT_WRITE));   /* a program from a VMO holds it */
    check_self_spawn(want);
    check_no_handover(want);
}

/* ---- children ----------------------------------------------------------------------- */

/* Started from /proc/self/exe (self_spawn): our path is want, our image
 * the starter's (koid, decimal; 0: none), executable and never writable;
 * then our code touched ("touch"), 'r' on descriptor 3, and we hold on
 * until descriptor 4 ends. */
static int child_image(const char *want, const char *koid, bool touch)
{
    char buf[256], c;
    int bad = 0;
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    bad += !(n == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)n));
    rights_t r = 0;
    uint64_t k = image_koid(&r);
    bad += k != strtoull(koid, NULL, 10);
    bad += k && ((r & RIGHT_WRITE) || !(r & RIGHT_EXEC));
    if (touch)
        touch_text();
    c = bad ? 'b' : 'r';
    bad += write(3, &c, 1) != 1;
    while (read(4, &c, 1) > 0)
        ;
    return bad ? 1 : 0;
}

static int child_twin(void)
{
    char buf[4];
    int bad = 0;
    errno = 0;
    bad += !(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0) == MAP_FAILED &&
             errno == EACCES);
    errno = 0;
    bad += !(open("/proc/self/fd/3", O_RDWR) == -1 && errno == EACCES);
    errno = 0;
    bad += !(write(3, "x", 1) == -1 && errno == EBADF);
    int r = open("/proc/self/fd/3", O_RDONLY);
    bad += !(r >= 0 && read(r, buf, 4) == 4 && !memcmp(buf, "TWIN", 4));
    return bad ? 1 : 0;
}

static int child_exe(void)
{
    char buf[64];
    ssize_t len = (ssize_t)strlen(EXE);
    return readlink("/proc/self/exe", buf, sizeof(buf)) == len && !memcmp(buf, EXE, (size_t)len)
               ? 0 : 1;
}

/* Started with no hand-over (check_no_handover): the path SR_ARGS named
 * is want, and so is /proc/self/exe. */
static int child_startup_exe(const char *want)
{
    char buf[256];
    const char *given = startup_exe();
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return given && !strcmp(given, want) && n == (ssize_t)strlen(want) &&
                   !memcmp(buf, want, (size_t)n)
               ? 0 : 1;
}

int pt_child_procfs(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[2], "startup-exe"))
        return child_startup_exe(argv[3]);
    if (argc >= 3 && !strcmp(argv[2], "block"))
        return pt_child_block();   /* procinfo.c's */
    if (argc >= 3 && !strcmp(argv[2], "fds"))
        return pt_child_fds();     /* procinfo.c's */
    if (argc >= 3 && !strcmp(argv[2], "twin"))
        return child_twin();
    if (argc >= 3 && !strcmp(argv[2], "exe"))
        return child_exe();
    if (argc >= 6 && !strcmp(argv[2], "image"))
        return child_image(argv[3], argv[4], !strcmp(argv[5], "touch"));
    return 2;
}

/* argv[0] never names the file: a lie, a missing name, /bin's path. */
static void check_children(void)
{
    static const char *const argv0s[] = {
        "zz-not-a-program", "/boot/bin/sh", "bin/sh", "/bin/posixtest", "/data/no-such-program",
        "/proc/self/exe",
    };
    for (unsigned i = 0; i < sizeof(argv0s) / sizeof(argv0s[0]); i++) {
        bool right = spawn_child(argv0s[i], "exe", -1) == 0;
        if (!right)
            printf("posixtest: procfs: a child with argv[0] %s: wrong exe\n", argv0s[i]);
        CHECK(right);
    }
}

void pt_procfs(char **argv __attribute__((unused)))
{
    check_exe_link();
    check_exe_file();
    check_self();
    check_missing();
    check_fd_listing();
    check_fd_between();
    check_fd_links();
    check_inotify_limits();
    check_reopen();
    check_children();
    check_self_spawn(EXE);
}
