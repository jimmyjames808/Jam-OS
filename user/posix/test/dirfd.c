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
 *     on descriptors only (FAT: 15 levels, then ENAMETOOLONG); on ext the
 *     current directory goes to the bottom too (fchdir; DL2): relative
 *     paths work, getcwd and the /proc links are ENAMETOOLONG, a spawned
 *     child starts there, chdir("..") climbs;
 *   - renamed: a directory renamed while open is followed (FAT: not), as
 *     the current directory, by getcwd, /proc/self/cwd and the
 *     descriptor's /proc/self/fd link;
 *   - removed: a directory removed and made again: the old descriptor
 *     names the removed one, an orphan (fstat says no links, fstatfs,
 *     fsync and ".." to the parent it had answer; ENOENT to a create and
 *     to getdents; its /proc link "(deleted)"), never the new one; a whole
 *     path from it goes through the mount (FAT: the new one, by its
 *     name); the current directory removed: getcwd ENOENT,
 *     /proc/self/cwd "(deleted)", nothing made there, chdir("..") out;
 *   - across: renameat, renameat2's exchange and linkat between two
 *     descriptors and between one and a whole path; EXDEV to the other
 *     mount;
 *   - paths: O_PATH (a file's and a directory's, the latter as an *at
 *     base and fchdir's) and AT_EMPTY_PATH;
 *   - up: ".." from a mount's root goes on from the mount point in our
 *     namespace ("/"; on FAT it stops at the root, M13's rule);
 *   - forked: a forked child makes a file through a directory descriptor,
 *     and writes one opened from it (its open made again there), and one
 *     in the current directory it shares, then execs: the new image's
 *     directory and descriptor 3 are the same (the hand-over's channels);
 *   - many: 100 directories open at once, more than the 64 channels a
 *     process has of a volume: the rest are text descriptions (the log
 *     says so once), so a rename is followed by the first, not by the
 *     last; each is written in and listed;
 *   - stand (DR, ext): a directory removed while 4 processes stand in it,
 *     each its current directory and a descriptor: each answers as a
 *     removed one, and ".." gets it out.
 * And once, on a fake `fs` service with directory channels: replaced
 * (DR): a descriptor whose channel closed (a fresh start) opened again by
 * its text when that is the same directory, id and volume, else ENOENT. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"
/* jl-names.h renames the layer's file_map (not used here) and fs_idl.h
 * names the `file` stub so: the stub's name wins. */
#undef file_map
#include <fs_idl.h>

#define EXT4_MAGIC 0xef53   /* statfs f_type of ext2, 3 and 4 */
#define LEVELS     26       /* DeleteDeep's */
#define MANY       100      /* directories open at once: past the 64 channels */
#define TOP_MAX    64       /* "/data/ptdirfd" and the names under it */

extern char **environ;

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

/* posixtest --child-dirfd mode [path] spawned in our directory, fd its 3:
 * its exit status (pt_child_dirfd's 0 when all held), or -1. */
static int spawn_kid(const char *mode, const char *path, int fd)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 3);
    char *argv[] = { "posixtest", "--child-dirfd", (char *)mode, (char *)path, NULL };
    pid_t pid = -1;
    int status = -1;
    int e = posix_spawn(&pid, "/bin/posixtest", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (e || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* readlink of link is want, or want and " (deleted)". */
static bool link_is(const char *link, const char *want, bool deleted)
{
    char buf[PATH_MAX];
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n < 0)
        return false;
    buf[n] = '\0';
    size_t w = strlen(want);
    return !strncmp(buf, want, w) && !strcmp(buf + w, deleted ? " (deleted)" : "");
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
    if (!ext) {
        CHECK(fchdir(fd) == 0 && chdir("/") == 0);
        return;
    }
    /* The current directory there too (DL2: a channel): getcwd and its
     * /proc links can't name it (past PATH_MAX), every relative path works,
     * and a spawned child starts there with the descriptor as its 3. */
    char cwd[PATH_MAX], link[32];
    errno = 0;
    CHECK(fchdir(fd) == 0 && getcwd(cwd, sizeof(cwd)) == NULL && errno == ENAMETOOLONG);
    CHECK(access("file", R_OK) == 0 && put_file(AT_FDCWD, "here", "h") &&
          faccessat(fd, "here", F_OK, 0) == 0);
    errno = 0;
    CHECK(readlink("/proc/self/cwd", cwd, sizeof(cwd)) == -1 && errno == ENAMETOOLONG);
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    errno = 0;
    CHECK(readlink(link, cwd, sizeof(cwd)) == -1 && errno == ENAMETOOLONG);
    CHECK(spawn_kid("deep", NULL, fd) == 0);
    CHECK(chdir("..") == 0 && faccessat(AT_FDCWD, long_name, F_OK, 0) == 0 && chdir("/") == 0);
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
        /* The current directory follows a rename too, and so do its /proc
         * links and the descriptor's. */
        char link[32];
        struct stat s1, s2;
        snprintf(link, sizeof(link), "/proc/self/fd/%d", dfd);
        CHECK(rename(b, a) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, a));
        CHECK(link_is("/proc/self/cwd", a, false) && link_is(link, a, false));
        CHECK(stat("/proc/self/cwd", &s1) == 0 && stat(".", &s2) == 0 && s1.st_ino == s2.st_ino &&
              S_ISDIR(s1.st_mode));
        int in = inotify_init1(IN_CLOEXEC);   /* a watch by a path from it: on the mount */
        CHECK(in >= 0 && inotify_add_watch(in, ".", IN_CREATE) >= 0);
        if (in >= 0)
            close(in);
        CHECK(rename(a, b) == 0);
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
        struct stat now;
        CHECK(fstat(dfd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 0 &&
              stat(c, &now) == 0 && now.st_ino != st.st_ino);   /* an orphan, as Linux's */
        struct statfs sf;
        CHECK(fstatfs(dfd, &sf) == 0 && sf.f_type == EXT4_MAGIC && fsync(dfd) == 0);
        int up = openat(dfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);   /* the parent it had */
        CHECK(up >= 0 && fstatat(up, "rm", &now, 0) == 0 && S_ISDIR(now.st_mode));
        if (up >= 0)
            close(up);
        /* a whole path ignores the descriptor (it goes through the mount, not the orphan) */
        CHECK(put_file(dfd, g, "whole") && there(g) && unlink(g) == 0);
        char link[32];   /* its /proc link: the removed one's name, "(deleted)" */
        snprintf(link, sizeof(link), "/proc/self/fd/%d", dfd);
        CHECK(link_is(link, c, true));
    } else {
        CHECK(made && there(g));   /* a text description: the new one, by its name */
        CHECK(unlink(g) == 0);
    }
    if (dfd >= 0)
        close(dfd);
    CHECK(rmdir(c) == 0);

    /* The current directory removed under us: getcwd ENOENT, its /proc
     * link "(deleted)", nothing made there, and ".." gets out (on ext
     * through the channel: DE2's remembered parent). */
    char cwd[PATH_MAX];
    CHECK(mkdir(c, 0755) == 0 && chdir(c) == 0 && rmdir(c) == 0);
    errno = 0;
    CHECK(getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT);
    CHECK(link_is("/proc/self/cwd", c, true));
    errno = 0;
    CHECK(open("x", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) == -1 && errno == ENOENT);
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, top));
    CHECK(chdir("/") == 0);
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
    /* In it as the current directory too: the child's is the same (its
     * fork key), and so is the new image's after the child's execve (the
     * hand-over's), with the descriptor as its 3. */
    CHECK(chdir(d) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        char cwd[PATH_MAX];
        int why = !put_file(dfd, "kid", "kid")               ? 2
                  : ext && write(w, "+kid", 4) != 4          ? 3
                  : !put_file(AT_FDCWD, "kidcwd", "c")       ? 4
                  : !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, d) ? 5
                  : dup2(dfd, 3) != 3 || fcntl(3, F_SETFD, 0) ? 6 : 0;   /* dfd may be 3 */
        char *argv[] = { "posixtest", "--child-dirfd", "here", d, NULL };
        if (!why)
            execv("/bin/posixtest", argv);
        _exit(why ? why : 7);
    }
    int status = -1;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    if (WEXITSTATUS(status))   /* the child's (2 to 7) or the new image's (10 on) */
        printf("posixtest: dirfd: forked: the child ended with %d\n", WEXITSTATUS(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(chdir("/") == 0);
    CHECK(there(c) && (!ext || (pread(w, buf, sizeof(buf), 0) == 10 &&
                                !memcmp(buf, "parent+kid", 10))));
    CHECK(faccessat(dfd, "kidcwd", F_OK, 0) == 0 && faccessat(dfd, "exec", F_OK, 0) == 0);
    if (w >= 0)
        close(w);
    if (dfd >= 0)
        close(dfd);
    int tfd = open(top, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(tfd >= 0 && remove_at(tfd, "fk") == 0);
    if (tfd >= 0)
        close(tfd);
}

/* 100 directories open (300 at first: the init run neared its 300 s
 * cap): a process's 64 channels of a volume, then text descriptions. */
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

/* ---- standing in a removed directory (DR) -------------------------------------------- */

#define STANDERS 4

/* A child standing in d (its current directory and descriptor fd) after
 * the parent removed it: each answers as a removed one. 0, or the number
 * of the check that failed. */
static int stand_after(const char *top, const char *d, int fd)
{
    char cwd[PATH_MAX];
    struct stat st;
    errno = 0;
    if (getcwd(cwd, sizeof(cwd)) || errno != ENOENT)
        return 2;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_nlink != 0)
        return 3;
    if (stat(".", &st) || st.st_nlink != 0)
        return 4;
    errno = 0;
    if (open("x", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) != -1 || errno != ENOENT)
        return 5;
    errno = 0;
    if (put_file(fd, "y", "y") || errno != ENOENT)
        return 6;
    if (!link_is("/proc/self/cwd", d, true))
        return 7;
    /* ".." the parent it had, whose "st" is another directory now */
    struct stat again;
    int up = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (up < 0 || fstatat(up, "st", &again, 0) || again.st_ino == st.st_ino)
        return 8;
    close(up);
    if (chdir("..") || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, top))
        return 9;
    return 0;
}

/* (DR's hostile run) A directory removed while 4 processes stand in it,
 * each its current directory and a descriptor on it: every one answers as
 * a removed directory (getcwd ENOENT, no links, nothing made there, its
 * /proc link "(deleted)", ".." the parent it had and chdir("..") out), and
 * one made again at its name is another directory. ext only: on FAT a
 * removed directory is a name, and nothing holds it. */
static void stand(const char *top, bool ext)
{
    if (!ext)
        return;
    char d[TOP_MAX + 8];
    snprintf(d, sizeof(d), "%s/st", top);
    CHECK(mkdir(d, 0755) == 0);
    struct stat was;
    CHECK(stat(d, &was) == 0);
    int ready[2], go[2];
    CHECK(pipe(ready) == 0 && pipe(go) == 0);
    pid_t kids[STANDERS];
    for (int i = 0; i < STANDERS; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            close(ready[0]);
            close(go[1]);
            char b = 0;
            int fd = chdir(d) == 0 ? open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
            bool told = fd >= 0 && write(ready[1], "r", 1) == 1;
            close(ready[1]);   /* a child that fails: the parent's read ends, not waits */
            if (!told || read(go[0], &b, 1) != 1)
                _exit(1);
            _exit(stand_after(top, d, fd));
        }
        CHECK(kids[i] > 0);
    }
    close(ready[1]);
    close(go[0]);
    char b;
    int in = 0;
    while (in < STANDERS && read(ready[0], &b, 1) == 1)
        in++;
    CHECK(in == STANDERS);
    CHECK(rmdir(d) == 0);
    CHECK(mkdir(d, 0755) == 0);   /* made again: another directory, which nobody stands in */
    struct stat now;
    CHECK(stat(d, &now) == 0 && now.st_ino != was.st_ino);
    for (int i = 0; i < STANDERS; i++)
        CHECK(write(go[1], "g", 1) == 1);
    close(go[1]);
    close(ready[0]);
    int bad = 0;
    for (int i = 0; i < STANDERS; i++) {
        int status = -1;
        if (kids[i] <= 0 || waitpid(kids[i], &status, 0) != kids[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status)) {
            printf("posixtest: dirfd: stand: child %d ended with %d\n", i,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            bad++;
        }
    }
    CHECK(bad == 0);
    CHECK(rmdir(d) == 0);
}

/* ---- a channel replaced after its service ended (DL1's reopen) ----------------------- */

/* A fake `fs` service with directory channels, at RF_MOUNT: "/" and "/d"
 * are directories; fs.opendir("/d") answers a channel served by a thread
 * of its own with the id and volume rf says now. rf.gen moved: every
 * channel made before closes, as when a service starts fresh. */
#define RF_MOUNT "/ptdfake"

static struct {
    pthread_mutex_t mu;
    handle_t        srv;        /* the mount's channel, our end */
    uint64_t        id, volume; /* what the next fs.opendir answers */
    unsigned        gen;        /* channels made before it moved close (atomic) */
    int             dirs;       /* channels' threads running (atomic) */
    int             threads;    /* every thread (atomic) */
} rf = { .mu = PTHREAD_MUTEX_INITIALIZER };

struct rf_dir {
    handle_t ch;
    uint64_t id;
    unsigned gen;
};

static status_t rf_stat(void *ctx, const char *path, uint32_t len, uint64_t *size,
                        uint8_t *is_dir, uint64_t *mtime, uint64_t *id)
{
    const struct rf_dir *x = ctx;
    bool d = x ? !len : (len == 1 && path[0] == '/') || (len == 2 && !memcmp(path, "/d", 2));
    if (!d)
        return ERR_NOT_FOUND;
    *size = 0;
    *is_dir = 1;
    *mtime = 0;
    *id = x ? x->id : len == 1 ? 2 : rf.id;
    return OK;
}

static status_t rf_statfs(void *ctx, uint64_t *total, uint64_t *free_bytes, uint8_t *ro,
                          uint8_t label[16], uint32_t *magic, uint32_t *name_max,
                          uint32_t *features)
{
    (void)ctx;
    *total = 1u << 20;
    *free_bytes = 1u << 19;
    *ro = 0;
    memset(label, 0, 16);
    *magic = 0x01021994;
    *name_max = 255;
    *features = FS_FEAT_DIRS;
    return OK;
}

static const struct fs_ops rf_dir_ops = { .stat = rf_stat, .statfs = rf_statfs };

static void *rf_dir_main(void *arg)
{
    struct rf_dir *x = arg;
    for (;;) {
        signals_t seen = 0;
        status_t st = jam_object_wait_one(x->ch, SIG_READABLE | SIG_PEER_CLOSED,
                                          now() + 20 * NS_PER_MS, &seen);
        if (__atomic_load_n(&rf.gen, __ATOMIC_SEQ_CST) != x->gen)
            break;   /* the service "ended": unanswered */
        if (st == ERR_TIMED_OUT)
            continue;
        if (st != OK || !(seen & SIG_READABLE))
            break;
        (void)fs_serve_one(x->ch, &rf_dir_ops, x);
    }
    jam_handle_close(x->ch);
    free(x);
    __atomic_sub_fetch(&rf.dirs, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&rf.threads, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static status_t rf_opendir(void *ctx, const char *path, uint32_t len, uint32_t flags,
                           handle_t *out, uint64_t *id, uint64_t *volume)
{
    (void)ctx;
    (void)flags;
    if (len != 2 || memcmp(path, "/d", 2))
        return ERR_NOT_FOUND;
    struct rf_dir *x = malloc(sizeof(*x));
    handle_t client;
    if (!x || jam_channel_create(&client, &x->ch) != OK) {
        free(x);
        return ERR_NO_RESOURCES;
    }
    x->id = rf.id;
    x->gen = __atomic_load_n(&rf.gen, __ATOMIC_SEQ_CST);
    pthread_t t;
    __atomic_add_fetch(&rf.dirs, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&rf.threads, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&t, NULL, rf_dir_main, x)) {
        __atomic_sub_fetch(&rf.dirs, 1, __ATOMIC_SEQ_CST);
        __atomic_sub_fetch(&rf.threads, 1, __ATOMIC_SEQ_CST);
        jam_handle_close(x->ch);
        jam_handle_close(client);
        free(x);
        return ERR_NO_RESOURCES;
    }
    pthread_detach(t);
    *out = client;
    *id = rf.id;
    *volume = rf.volume;
    return OK;
}

static status_t rf_mount_stat(void *ctx, const char *path, uint32_t len, uint64_t *size,
                              uint8_t *is_dir, uint64_t *mtime, uint64_t *id)
{
    (void)ctx;
    return rf_stat(NULL, path, len, size, is_dir, mtime, id);
}

static const struct fs_ops rf_mount_ops = { .stat = rf_mount_stat, .statfs = rf_statfs,
                                            .opendir = rf_opendir };

static void *rf_mount_main(void *arg)
{
    (void)arg;
    for (;;) {
        signals_t seen = 0;
        status_t st = jam_object_wait_one(rf.srv, SIG_READABLE | SIG_PEER_CLOSED,
                                          now() + 20 * NS_PER_MS, &seen);
        if (st == ERR_TIMED_OUT)
            continue;
        if (st != OK || !(seen & SIG_READABLE))
            break;
        pthread_mutex_lock(&rf.mu);
        (void)fs_serve_one(rf.srv, &rf_mount_ops, NULL);
        pthread_mutex_unlock(&rf.mu);
    }
    jam_handle_close(rf.srv);
    __atomic_sub_fetch(&rf.threads, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* The fake's directory channels end (a fresh start): true once every
 * one's thread has closed it. The next opendir answers id and volume. */
static bool rf_restart(uint64_t id, uint64_t volume)
{
    pthread_mutex_lock(&rf.mu);
    rf.id = id;
    rf.volume = volume;
    __atomic_add_fetch(&rf.gen, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&rf.mu);
    for (int i = 0; i < 200 && __atomic_load_n(&rf.dirs, __ATOMIC_SEQ_CST); i++)
        usleep(10000);
    return __atomic_load_n(&rf.dirs, __ATOMIC_SEQ_CST) == 0;
}

/* st_ino of fd's directory, or 0 with errno. */
static ino_t ino_of(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_ino : 0;
}

/* (DR) DL1's reopen after ERR_PEER_CLOSED, which extfs can't be made to
 * do from here (a fresh start gives its state up): on the fake, a
 * directory descriptor whose channel closed is opened again by its last
 * known text and carries on when that is the same directory (its id and
 * volume); another directory at the name (another inode, or the same
 * inode number on another volume, DR's L1) is ENOENT from then on, even
 * once the name is the old directory again. */
static void replaced(void)
{
    handle_t client;
    rf.id = 7;
    rf.volume = 1;
    rf.gen = 0;
    CHECK(jam_channel_create(&client, &rf.srv) == OK);
    pthread_t t;
    __atomic_add_fetch(&rf.threads, 1, __ATOMIC_SEQ_CST);
    CHECK(pthread_create(&t, NULL, rf_mount_main, NULL) == 0);
    pthread_detach(t);
    CHECK(ns_mount(RF_MOUNT, client) == OK);
    int a = open(RF_MOUNT "/d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int b = open(RF_MOUNT "/d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int c = open(RF_MOUNT "/d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(a >= 0 && b >= 0 && c >= 0 && ino_of(a) == 7 && ino_of(b) == 7 && ino_of(c) == 7);
    CHECK(__atomic_load_n(&rf.dirs, __ATOMIC_SEQ_CST) == 3);   /* each its channel */

    /* The same directory there again: each carries on, on a channel of its own. */
    CHECK(rf_restart(7, 1));
    CHECK(ino_of(a) == 7 && ino_of(b) == 7 && ino_of(c) == 7);
    CHECK(__atomic_load_n(&rf.dirs, __ATOMIC_SEQ_CST) == 3);

    /* The same inode number on another volume: not the same directory. */
    CHECK(rf_restart(7, 2));
    errno = 0;
    CHECK(ino_of(a) == 0 && errno == ENOENT);
    /* Another inode at the name. */
    CHECK(rf_restart(8, 1));
    errno = 0;
    CHECK(ino_of(b) == 0 && errno == ENOENT);
    /* Gone stays gone: the old directory back at its name changes nothing. */
    CHECK(rf_restart(7, 1));
    errno = 0;
    CHECK(ino_of(a) == 0 && errno == ENOENT && ino_of(b) == 0 && ino_of(c) == 7);
    close(a);
    close(b);
    close(c);
    CHECK(ns_unmount(RF_MOUNT) == OK);
    for (int i = 0; i < 200 && __atomic_load_n(&rf.threads, __ATOMIC_SEQ_CST); i++)
        usleep(10000);
    CHECK(__atomic_load_n(&rf.threads, __ATOMIC_SEQ_CST) == 0);
    printf("posixtest: dirfd: a closed channel replaced by its text, or ENOENT\n");
}

/* posixtest --child-dirfd <mode> [path], its 3 a directory: "deep" (the
 * bottom of the deep tree: getcwd ENAMETOOLONG, "file" there from both)
 * or "here" (getcwd says path, "kid" under 3, "exec" made in the current
 * directory). 0, or the number of the check that failed. */
int pt_child_dirfd(int argc, char **argv)
{
    char cwd[PATH_MAX];
    struct stat st;
    if (fstat(3, &st) || !S_ISDIR(st.st_mode))
        return 10;
    if (argc >= 3 && !strcmp(argv[2], "deep")) {
        errno = 0;
        if (getcwd(cwd, sizeof(cwd)) || errno != ENAMETOOLONG)
            return 11;
        if (access("file", R_OK) || faccessat(3, "file", R_OK, 0))
            return 12;
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[2], "here")) {
        if (!getcwd(cwd, sizeof(cwd)) || strcmp(cwd, argv[3]))
            return 13;
        if (faccessat(3, "kid", F_OK, 0) || !put_file(AT_FDCWD, "exec", "e"))
            return 14;
        return 0;
    }
    return 15;
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
        stand(tops[m], ext);
    }
    replaced();
    for (int m = 0; m < 2; m++) {
        int r = have[m] ? open(mounts[m], O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (r >= 0) {
            CHECK(remove_at(r, "ptdirfd") == 0 && !there(tops[m]));
            close(r);
        }
    }
}
