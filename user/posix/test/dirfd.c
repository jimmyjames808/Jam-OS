/* posixtest's dirfd section (docs/DIRHANDLE-PLAN.md, DL1): directory
 * descriptors as their directories' own `fs` channels, on /data and on
 * /tmp (ext: a view of /data/tmp, or a volume in memory). Where the mount
 * is FAT (a stick made with DATA_FS=fat) a descriptor is its path as text
 * and the section checks today's answers there. In each mount's
 * <mount>/ptdirfd:
 *   - deep: base's FileUtilTest.DeleteDeep: 26 levels of 255-byte names
 *     made through mkdirat and openat on each level's descriptor (about
 *     6.6 KB of path), a file and a link `up` to ".." at the bottom, the
 *     *at calls there, then the tree removed as DeletePathRecursively
 *     removes one: fstatat, openat with O_NOFOLLOW, fdopendir and unlinkat
 *     on descriptors only (FAT: 15 levels, then ENAMETOOLONG);
 *   - renamed: a directory renamed while open is followed (FAT: not);
 *   - removed: a directory removed and made again: the old descriptor
 *     names the removed one (ENOENT to a create and to getdents), never
 *     the new one (FAT: the new one, by its name);
 *   - across: renameat, renameat2's exchange and linkat between two
 *     descriptors and between one and a whole path; EXDEV to the other
 *     mount;
 *   - paths: O_PATH (a file's and a directory's, the latter as an *at
 *     base and fchdir's) and AT_EMPTY_PATH;
 *   - up: ".." from a mount's root goes on from the mount point in our
 *     namespace ("/"; on FAT it stops at the root, M13's rule);
 *   - forked: a forked child makes a file through a directory descriptor,
 *     and writes one opened from it (its open made again there);
 *   - many: 300 directories open at once, more than the 64 channels a
 *     process has of a volume: the rest are text descriptions (the log
 *     says so once), so a rename is followed by the first, not by the
 *     last; each is written in and listed.
 * A removed directory's channel answers ERR_NOT_FOUND to everything
 * until extfs keeps it as an orphan (DE2), which makes fstat and ".."
 * answer: those two checks take either. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define EXT4_MAGIC 0xef53   /* statfs f_type of ext2, 3 and 4 */
#define LEVELS     26       /* DeleteDeep's */
#define MANY       300      /* directories open at once: past the 64 channels */
#define TOP_MAX    64       /* "/data/ptdirfd" and the names under it */

static char long_name[256];   /* NAME_MAX bytes of 'a' */

/* Is m's mount ext (directory channels) rather than FAT? */
static bool is_ext(const char *m)
{
    struct statfs s;
    return statfs(m, &s) == 0 && s.f_type == EXT4_MAGIC;
}

/* name under dfd and everything under it removed as base's
 * DeletePathRecursively removes a tree: by descriptors only (fstatat
 * without following, openat with O_NOFOLLOW, fdopendir, unlinkat); 0 or
 * -1. */
static int remove_at(int dfd, const char *name)
{
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlinkat(dfd, name, 0);
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    int r = 0;
    struct dirent *e;
    while (!r && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        r = remove_at(dirfd(d), e->d_name);
        if (!r)
            rewinddir(d);   /* the listing changed under us: from the start again */
    }
    closedir(d);
    return r ? r : unlinkat(dfd, name, AT_REMOVEDIR);
}

/* A file at dfd/name holding text. */
static bool put_file(int dfd, const char *name, const char *text)
{
    int f = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok = f >= 0 && write(f, text, strlen(text)) == (ssize_t)strlen(text);
    if (f >= 0)
        close(f);
    return ok;
}

/* Is name (absolute) there? */
static bool there(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* A listing of dfd (a duplicate: the caller keeps dfd) holding name, and
 * "..", whose d_ino is *up (0: not asked). */
static bool lists(int dfd, const char *name, ino_t up)
{
    int fd = dup(dfd);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    bool found = false, dots = false;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        found |= !strcmp(e->d_name, name);
        if (!strcmp(e->d_name, ".."))
            dots = !up || e->d_ino == up;
    }
    if (d)
        closedir(d);
    else if (fd >= 0)
        close(fd);
    return found && dots;
}

/* The bottom of the deep tree: the *at calls there, then a file and a
 * link (ext) left for the removal. */
static void deep_bottom(int fd, bool ext)
{
    struct stat st, up;
    char buf[8];
    CHECK(put_file(fd, "file", "deep"));
    CHECK(fstatat(fd, "file", &st, 0) == 0 && S_ISREG(st.st_mode) && st.st_size == 4);
    CHECK(faccessat(fd, "file", R_OK | W_OK, 0) == 0);
    CHECK(fchmodat(fd, "file", 0600, 0) == 0);
    struct timespec t[2] = { { 1000, 0 }, { 2000, 0 } };
    CHECK(utimensat(fd, "file", t, 0) == 0);
    if (ext) {
        CHECK(fstatat(fd, "file", &st, 0) == 0 && (st.st_mode & 0777) == 0600 &&
              st.st_mtime == 2000);
        CHECK(symlinkat("..", fd, "up") == 0);
        CHECK(readlinkat(fd, "up", buf, sizeof(buf)) == 2 && !memcmp(buf, "..", 2));
        CHECK(fstatat(fd, "up", &st, 0) == 0 && fstatat(fd, "..", &up, 0) == 0 &&
              S_ISDIR(st.st_mode) && st.st_ino == up.st_ino);   /* the link leads up a level */
        CHECK(fstatat(fd, "up", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode));
    }
    CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
    struct statfs sf;
    CHECK(fstatfs(fd, &sf) == 0 && (sf.f_type == EXT4_MAGIC) == ext);
    CHECK(fsync(fd) == 0);
    CHECK(fstatat(fd, "..", &up, 0) == 0 && lists(fd, "file", ext ? up.st_ino : 0));
    int p = openat(fd, "file", O_PATH | O_CLOEXEC);
    CHECK(p >= 0 && fstat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 4);
    if (p >= 0)
        close(p);
    errno = 0;
    if (ext)   /* the current directory is text until DL2: past PATH_BYTES */
        CHECK(fchdir(fd) == -1 && errno == ENAMETOOLONG);
    else
        CHECK(fchdir(fd) == 0 && chdir("/") == 0);
}

/* base's FileUtilTest.DeleteDeep. */
static void deep(const char *top, bool ext)
{
    char path[TOP_MAX + 8];
    snprintf(path, sizeof(path), "%s/deep", top);
    CHECK(mkdir(path, 0755) == 0);
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    int levels = 0, why = 0;
    while (fd >= 0 && levels < LEVELS) {
        if (mkdirat(fd, long_name, 0700)) {
            why = errno;
            break;
        }
        int next = openat(fd, long_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(next >= 0);
        close(fd);   /* each level's descriptor replaces the one above, as the test's */
        fd = next;
        levels++;
    }
    if (ext)
        CHECK(levels == LEVELS);
    else   /* text: 13 + 256 * 15 bytes is the last that fits PATH_BYTES */
        CHECK(levels >= 14 && levels < LEVELS && why == ENAMETOOLONG);
    if (fd >= 0) {
        deep_bottom(fd, ext);
        close(fd);
    }
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(tfd >= 0 && remove_at(tfd, "deep") == 0 && !there(path));
    if (tfd >= 0)
        close(tfd);
}

/* A directory renamed while open: its descriptor follows it (a channel). */
static void renamed(const char *top, bool ext)
{
    char a[TOP_MAX + 8], b[TOP_MAX + 8], f[TOP_MAX + 16], cwd[PATH_MAX];
    snprintf(a, sizeof(a), "%s/ra", top);
    snprintf(b, sizeof(b), "%s/rb", top);
    snprintf(f, sizeof(f), "%s/rb/f", top);
    CHECK(mkdir(a, 0755) == 0);
    int dfd = open(a, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dfd >= 0 && rename(a, b) == 0);
    errno = 0;
    bool made = put_file(dfd, "f", "moved");
    if (ext) {
        CHECK(made && there(f));
        CHECK(fchdir(dfd) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, b));   /* asked now */
        CHECK(chdir("/") == 0);
        CHECK(renameat(dfd, "f", dfd, "g") == 0 && faccessat(dfd, "g", F_OK, 0) == 0);
        CHECK(unlinkat(dfd, "g", 0) == 0);
    } else {
        CHECK(!made && errno == ENOENT);   /* a text description: the old name */
    }
    if (dfd >= 0)
        close(dfd);
    CHECK(rmdir(b) == 0);
}

/* A directory removed while open and made again at its name. */
static void removed(const char *top, bool ext)
{
    char c[TOP_MAX + 8], g[TOP_MAX + 16];
    char buf[512];
    struct stat st;
    snprintf(c, sizeof(c), "%s/rm", top);
    snprintf(g, sizeof(g), "%s/rm/g", top);
    CHECK(mkdir(c, 0755) == 0);
    int dfd = open(c, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dfd >= 0 && rmdir(c) == 0 && mkdir(c, 0755) == 0);
    errno = 0;
    bool made = put_file(dfd, "g", "x");
    if (ext) {
        CHECK(!made && errno == ENOENT);   /* the removed one: nothing is made there */
        CHECK(!there(g));                   /* ... and the new one is another directory */
        errno = 0;
        CHECK(syscall(SYS_getdents64, dfd, buf, sizeof(buf)) == -1 && errno == ENOENT);
        CHECK(fstat(dfd, &st) == 0 || errno == ENOENT);   /* DE2: answered (an orphan) */
        int up = openat(dfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(up >= 0 || errno == ENOENT);                  /* DE2: the parent it had */
        if (up >= 0) {
            CHECK(fstatat(up, "rm", &st, 0) == 0 && S_ISDIR(st.st_mode));
            close(up);
        }
    } else {
        CHECK(made && there(g));   /* a text description: the new one, by its name */
        CHECK(unlink(g) == 0);
    }
    if (dfd >= 0)
        close(dfd);
    CHECK(rmdir(c) == 0);
}

/* renameat, renameat2 and linkat between two descriptors, and between
 * one and a whole path; other: the other mount's top (EXDEV). */
static void across(const char *top, bool ext, const char *other)
{
    char a[TOP_MAX + 8], b[TOP_MAX + 8], p[TOP_MAX + 16];
    struct stat s1, s2;
    snprintf(a, sizeof(a), "%s/xa", top);
    snprintf(b, sizeof(b), "%s/xb", top);
    CHECK(mkdir(a, 0755) == 0 && mkdir(b, 0755) == 0);
    int da = open(a, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int db = open(b, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(da >= 0 && db >= 0 && put_file(da, "x", "x") && put_file(da, "z", "z"));
    CHECK(renameat(da, "x", db, "y") == 0 && faccessat(da, "x", F_OK, 0) == -1 &&
          faccessat(db, "y", F_OK, 0) == 0);
    snprintf(p, sizeof(p), "%s/x2", a);   /* a whole path on the other side */
    CHECK(renameat(db, "y", AT_FDCWD, p) == 0 && there(p));
    CHECK(renameat(AT_FDCWD, p, db, "y2") == 0 && !there(p) && faccessat(db, "y2", F_OK, 0) == 0);
    errno = 0;
    CHECK(renameat(da, "missing", db, "m") == -1 && errno == ENOENT);
    CHECK(mkdirat(da, "sub", 0755) == 0 && put_file(da, "sub/in", "in") &&
          renameat(da, "sub", db, "sub") == 0 && faccessat(db, "sub/in", F_OK, 0) == 0);
    errno = 0;
    if (ext) {
        CHECK(linkat(da, "z", db, "z2", 0) == 0);
        CHECK(fstatat(da, "z", &s1, 0) == 0 && fstatat(db, "z2", &s2, 0) == 0 &&
              s1.st_ino == s2.st_ino && s1.st_nlink == 2);
        errno = 0;
        CHECK(linkat(da, "z", db, "z2", 0) == -1 && errno == EEXIST);
        CHECK(fstatat(db, "y2", &s2, 0) == 0);
        CHECK(syscall(SYS_renameat2, da, "z", db, "y2", RENAME_EXCHANGE) == 0);
        CHECK(fstatat(da, "z", &s1, 0) == 0 && fstatat(db, "y2", &s1, 0) == 0 &&
              s1.st_ino != s2.st_ino);   /* y2 is what z was: exchanged */
    } else {
        CHECK(linkat(da, "z", db, "z2", 0) == -1 && errno == EPERM);   /* vfat: no links */
    }
    int dother = other ? open(other, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    if (dother >= 0) {
        errno = 0;
        CHECK(renameat(da, "z", dother, "z") == -1 && errno == EXDEV);
        if (ext && is_ext(other)) {
            errno = 0;
            CHECK(linkat(da, "z", dother, "z", 0) == -1 && errno == EXDEV);
        }
        close(dother);
    }
    if (da >= 0)
        close(da);
    if (db >= 0)
        close(db);
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(tfd >= 0 && remove_at(tfd, "xa") == 0 && remove_at(tfd, "xb") == 0);
    if (tfd >= 0)
        close(tfd);
}

/* O_PATH descriptors (a file's, a directory's as an *at base and
 * fchdir's) and AT_EMPTY_PATH. */
static void paths(const char *top, bool ext)
{
    char d[TOP_MAX + 8], cwd[PATH_MAX], buf[512];
    struct stat st, st2;
    snprintf(d, sizeof(d), "%s/op", top);
    CHECK(mkdir(d, 0755) == 0);
    int pd = open(d, O_PATH | O_DIRECTORY | O_CLOEXEC);
    CHECK(pd >= 0 && fstat(pd, &st) == 0 && S_ISDIR(st.st_mode));
    errno = 0;
    CHECK(syscall(SYS_getdents64, pd, buf, sizeof(buf)) == -1 && errno == EBADF);
    CHECK(mkdirat(pd, "sub", 0755) == 0 && put_file(pd, "f", "path"));
    int f = openat(pd, "f", O_RDWR | O_CLOEXEC);   /* an O_PATH directory as the base */
    CHECK(f >= 0 && read(f, buf, 4) == 4 && !memcmp(buf, "path", 4));
    CHECK(fstatat(f, "", &st, AT_EMPTY_PATH) == 0 && S_ISREG(st.st_mode) && st.st_size == 4);
    struct timespec t[2] = { { 3000, 0 }, { 4000, 0 } };
    CHECK(futimens(f, t) == 0);
    if (f >= 0)
        close(f);
    int pf = openat(pd, "f", O_PATH | O_CLOEXEC);
    errno = 0;
    CHECK(pf >= 0 && read(pf, buf, 1) == -1 && errno == EBADF);
    CHECK(fstat(pf, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 4 &&
          (!ext || st.st_mtime == 4000));
    CHECK(fstatat(pf, "", &st2, AT_EMPTY_PATH) == 0 && st2.st_ino == st.st_ino);
    CHECK(fchownat(pf, "", -1, -1, AT_EMPTY_PATH) == 0);
    if (pf >= 0)
        close(pf);
    int ps = openat(pd, "sub", O_PATH | O_DIRECTORY | O_CLOEXEC);
    int back = ps >= 0 ? openat(ps, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    CHECK(back >= 0 && fstat(back, &st) == 0 && fstat(pd, &st2) == 0 && st.st_ino == st2.st_ino);
    CHECK(fstatat(back, "", &st, AT_EMPTY_PATH) == 0 && S_ISDIR(st.st_mode));
    CHECK(faccessat(back, "", R_OK, AT_EMPTY_PATH) == 0);
    CHECK(fchmod(back, 0750) == 0 && fstat(back, &st) == 0 && (!ext || (st.st_mode & 0777) == 0750));
    if (ext) {
        CHECK(symlinkat("f", pd, "lnk") == 0);
        int pl = openat(pd, "lnk", O_PATH | O_NOFOLLOW | O_CLOEXEC);   /* the link itself */
        CHECK(pl >= 0 && fstat(pl, &st) == 0 && S_ISLNK(st.st_mode));
        if (pl >= 0)
            close(pl);
    }
    CHECK(fchdir(ps) == 0 && getcwd(cwd, sizeof(cwd)) && !strncmp(cwd, d, strlen(d)) &&
          !strcmp(cwd + strlen(d), "/sub"));
    CHECK(chdir("/") == 0);
    if (ps >= 0)
        close(ps);
    if (back >= 0)
        close(back);
    if (pd >= 0)
        close(pd);
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(tfd >= 0 && remove_at(tfd, "op") == 0);
    if (tfd >= 0)
        close(tfd);
}

/* ".." from a mount's root: the mount point's directory in our namespace
 * ("/", which lists the mounts); on FAT it stops at the root. */
static void up(const char *mount, bool ext)
{
    struct stat st, root;
    char again[16];
    snprintf(again, sizeof(again), "..%s/.", mount);   /* out of the mount and back in */
    int r = open(mount, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int u = r >= 0 ? openat(r, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    CHECK(u >= 0 && fstat(r, &root) == 0);
    if (ext)
        CHECK(fstatat(u, mount + 1, &st, 0) == 0 && S_ISDIR(st.st_mode) &&
              fstatat(r, again, &st, 0) == 0 && st.st_ino == root.st_ino);
    else
        CHECK(fstat(u, &st) == 0 && st.st_ino == root.st_ino);   /* M13: ".." stops there */
    if (u >= 0)
        close(u);
    if (r >= 0)
        close(r);
}

/* A forked child's descriptors: a directory channel duplicated, and (ext:
 * FAT's one writer is ours) a file opened from one, whose open the child
 * makes again by its name. */
static void forked(const char *top, bool ext)
{
    char d[TOP_MAX + 8], c[TOP_MAX + 16], buf[16];
    snprintf(d, sizeof(d), "%s/fk", top);
    snprintf(c, sizeof(c), "%s/fk/kid", top);
    CHECK(mkdir(d, 0755) == 0);
    int dfd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int w = dfd >= 0 ? openat(dfd, "w", O_RDWR | O_CREAT | O_CLOEXEC, 0644) : -1;
    CHECK(w >= 0 && write(w, "parent", 6) == 6);
    pid_t pid = fork();
    if (pid == 0) {
        bool ok = put_file(dfd, "kid", "kid") && (!ext || write(w, "+kid", 4) == 4);
        _exit(ok ? 0 : 1);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(there(c) && (!ext || (pread(w, buf, sizeof(buf), 0) == 10 &&
                                !memcmp(buf, "parent+kid", 10))));
    if (w >= 0)
        close(w);
    if (dfd >= 0)
        close(dfd);
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(tfd >= 0 && remove_at(tfd, "fk") == 0);
    if (tfd >= 0)
        close(tfd);
}

/* 300 directories open: a process's 64 channels of a volume, then text
 * descriptions. */
static void many(const char *top, bool ext)
{
    static int fds[MANY];
    char p[TOP_MAX + 16], q[TOP_MAX + 16];
    int opened = 0, worked = 0;
    for (int i = 0; i < MANY; i++) {
        snprintf(p, sizeof(p), "%s/m%03d", top, i);
        fds[i] = mkdir(p, 0755) == 0 ? open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        opened += fds[i] >= 0;
    }
    CHECK(opened == MANY);
    for (int i = 0; i < MANY; i++) {
        struct stat st;
        worked += fds[i] >= 0 && put_file(fds[i], "f", "m") && fstat(fds[i], &st) == 0 &&
                  S_ISDIR(st.st_mode) && faccessat(fds[i], "f", F_OK, 0) == 0;
    }
    CHECK(worked == MANY);
    CHECK(fds[MANY / 2] >= 0 && lists(fds[MANY / 2], "f", 0));
    int ends[2] = { 0, MANY - 1 };
    for (int k = 0; k < 2; k++) {
        snprintf(p, sizeof(p), "%s/m%03d", top, ends[k]);
        snprintf(q, sizeof(q), "%s/n%03d", top, ends[k]);
        CHECK(rename(p, q) == 0);
    }
    errno = 0;
    if (ext)   /* the first is a channel: followed */
        CHECK(fds[0] >= 0 && faccessat(fds[0], "f", F_OK, 0) == 0);
    else
        CHECK(fds[0] >= 0 && faccessat(fds[0], "f", F_OK, 0) == -1 && errno == ENOENT);
    errno = 0;   /* the last is text (the service's share is used): not followed */
    CHECK(fds[MANY - 1] >= 0 && faccessat(fds[MANY - 1], "f", F_OK, 0) == -1 && errno == ENOENT);
    for (int i = 0; i < MANY; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int gone = 0;
    for (int i = 0; tfd >= 0 && i < MANY; i++) {
        snprintf(p, sizeof(p), "%c%03d", i == 0 || i == MANY - 1 ? 'n' : 'm', i);
        gone += remove_at(tfd, p) == 0;
    }
    CHECK(gone == MANY);
    if (tfd >= 0)
        close(tfd);
}

void pt_dirfd(char **argv)
{
    (void)argv;
    static const char *const mounts[] = { "/data", "/tmp" };
    char tops[2][TOP_MAX];
    struct stat st;
    for (int i = 0; i < 300 && stat("/data", &st); i++) {   /* volumes mounts it: 30 s */
        struct timespec t = { 0, 100 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    memset(long_name, 'a', 255);
    bool have[2];
    for (int m = 0; m < 2; m++) {
        snprintf(tops[m], sizeof(tops[m]), "%s/ptdirfd", mounts[m]);
        int r = open(mounts[m], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        have[m] = r >= 0 && remove_at(r, "ptdirfd") == 0 && mkdir(tops[m], 0755) == 0;
        if (r >= 0)
            close(r);
        if (!have[m])
            printf("posixtest: dirfd: %s: not there (%s): skipped\n", mounts[m], strerror(errno));
    }
    for (int m = 0; m < 2; m++) {
        if (!have[m])
            continue;
        bool ext = is_ext(mounts[m]);
        printf("posixtest: dirfd: %s (%s)\n", mounts[m], ext ? "ext: channels" : "fat: text");
        deep(tops[m], ext);
        renamed(tops[m], ext);
        removed(tops[m], ext);
        across(tops[m], ext, have[1 - m] ? tops[1 - m] : NULL);
        paths(tops[m], ext);
        up(mounts[m], ext);
        forked(tops[m], ext);
        many(tops[m], ext);
    }
    for (int m = 0; m < 2; m++) {
        int r = have[m] ? open(mounts[m], O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (r >= 0) {
            CHECK(remove_at(r, "ptdirfd") == 0 && !there(tops[m]));
            close(r);
        }
    }
}
