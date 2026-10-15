/* posixtest's links section (`posixtest links`, asked for by name):
 * what POSIX gains on a filesystem with links and modes (docs/M13.6-PLAN.md,
 * "What POSIX gains" and "Paths and links"). It needs an extfs volume
 * mounted twice in its namespace, at LX and at LV (two mounts of one
 * volume): utest's extfs_posix starts extfs over a RAM disk and runs it so.
 *
 *   - statfs says ext4 (0xef53) and a name length of 255;
 *   - symbolic links: made, read (whole and cut short), lstat sees the
 *     link, stat and open follow it, getdents' d_type says DT_LNK; a
 *     relative one with "..", one into another mount (absolute: followed
 *     in our own namespace), a loop (ELOOP, inside one walk and across
 *     mounts), O_NOFOLLOW (ELOOP), O_PATH | O_NOFOLLOW (the link itself),
 *     O_CREAT | O_EXCL and mkdir on a dangling one (EEXIST, nothing made),
 *     O_CREAT through one (its target made);
 *   - ".." above a mount's root is "/"; chdir through a link stores where
 *     it led (getcwd, then "..");
 *   - hard links: st_nlink, one st_ino, EXDEV between the two mounts of
 *     the one volume, EPERM for a directory, EEXIST;
 *   - modes, owners and times stored: chmod (all twelve bits), fchmod,
 *     access by them, chown to the one user (and -1) only, utimensat and
 *     futimens with nanoseconds, UTIME_OMIT; the umask on every create;
 *   - renameat2's RENAME_EXCHANGE and RENAME_NOREPLACE;
 *   - unlink of an open file (still read through its descriptor);
 *   - a writable O_APPEND descriptor given to a child while we write it
 *     too (many writers): nothing lost, nothing overwritten;
 *   - a file past 4 GiB (sparse); mknod still EPERM. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define LX "/u6"     /* the volume */
#define LV "/u6v"    /* the same volume, a second mount (a view) */
#define EXT4_MAGIC 0xef53

extern char **environ;

static bool put(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    return !close(fd) && ok;
}

/* Does the file at path hold exactly s? */
static bool holds(const char *path, const char *s)
{
    char buf[64] = { 0 };
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n == (ssize_t)strlen(s) && !memcmp(buf, s, (size_t)n);
}

static mode_t mode_of(const char *path)
{
    struct stat st;
    return lstat(path, &st) ? (mode_t)-1 : st.st_mode;
}

/* d_type of name in directory dir, or 255 if it isn't listed. */
static int dtype(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    int t = 255;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (!strcmp(e->d_name, name))
            t = e->d_type;
    if (d)
        closedir(d);
    return t;
}

static void symlinks(void)
{
    char buf[PATH_MAX];
    struct stat st, lst;
    CHECK(mkdir(LX "/t", 0755) == 0 && mkdir(LX "/t/sub", 0755) == 0);
    CHECK(put(LX "/t/f", "x"));
    CHECK(symlink("f", LX "/t/l1") == 0);
    CHECK(symlink("g", LX "/t/l1") == -1 && errno == EEXIST);
    CHECK(readlink(LX "/t/l1", buf, sizeof(buf)) == 1 && buf[0] == 'f');
    CHECK(symlink("abcdef", LX "/t/l6") == 0);
    CHECK(readlink(LX "/t/l6", buf, 3) == 3 && !memcmp(buf, "abc", 3));   /* cut, no NUL */
    CHECK(readlink(LX "/t/f", buf, sizeof(buf)) == -1 && errno == EINVAL);
    CHECK(readlink(LX "/t/none", buf, sizeof(buf)) == -1 && errno == ENOENT);
    CHECK(lstat(LX "/t/l1", &lst) == 0 && S_ISLNK(lst.st_mode) && lst.st_size == 1);
    CHECK(stat(LX "/t/l1", &st) == 0 && S_ISREG(st.st_mode));
    CHECK(fstatat(AT_FDCWD, LX "/t/l1", &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode));
    CHECK(holds(LX "/t/l1", "x"));
    CHECK(dtype(LX "/t", "l1") == DT_LNK && dtype(LX "/t", "f") == DT_REG &&
          dtype(LX "/t", "sub") == DT_DIR);
    CHECK(symlink("", LX "/t/empty") == -1 && errno == ENOENT);

    /* Relative, with "..": sub/l2 -> ../f. */
    CHECK(symlink("../f", LX "/t/sub/l2") == 0 && holds(LX "/t/sub/l2", "x"));
    CHECK(symlink("sub", LX "/t/ls") == 0 && holds(LX "/t/ls/../f", "x"));   /* .. physical */

    /* O_NOFOLLOW, O_PATH. */
    CHECK(open(LX "/t/l1", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    int fd = open(LX "/t/l1", O_PATH | O_NOFOLLOW);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode));
    if (fd >= 0)
        close(fd);
    CHECK(open(LX "/t/ls", O_RDONLY | O_DIRECTORY | O_NOFOLLOW) == -1 && errno == ELOOP);
    fd = open(LX "/t/ls", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0);
    if (fd >= 0)
        close(fd);

    /* Dangling links: never made by O_EXCL or mkdir; O_CREAT makes the target. */
    CHECK(symlink("nothere", LX "/t/dang") == 0);
    CHECK(stat(LX "/t/dang", &st) == -1 && errno == ENOENT);
    CHECK(open(LX "/t/dang", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    CHECK(mkdir(LX "/t/dang", 0755) == -1 && errno == EEXIST);
    CHECK(access(LX "/t/nothere", F_OK) == -1 && errno == ENOENT);
    CHECK(put(LX "/t/dang", "made") && holds(LX "/t/nothere", "made"));

    /* Loops: inside one walk (the service's count), and across two mounts (ours). */
    CHECK(symlink("lp", LX "/t/lp") == 0);
    CHECK(stat(LX "/t/lp", &st) == -1 && errno == ELOOP);
    CHECK(open(LX "/t/lp", O_RDONLY) == -1 && errno == ELOOP);
    CHECK(symlink(LV "/t/xb", LX "/t/xa") == 0 && symlink(LX "/t/xa", LX "/t/xb") == 0);
    CHECK(stat(LX "/t/xa", &st) == -1 && errno == ELOOP);
    CHECK(lstat(LX "/t/xa", &st) == 0 && S_ISLNK(st.st_mode));

    /* Unlink, rename move the link, never its target. */
    CHECK(symlink("f", LX "/t/lm") == 0 && rename(LX "/t/lm", LX "/t/lm2") == 0);
    CHECK(unlink(LX "/t/lm2") == 0 && holds(LX "/t/f", "x"));
}

/* Absolute links and "..": followed in our own namespace. */
static void outward(void)
{
    struct stat a, b;
    char cwd[PATH_MAX];
    CHECK(symlink(LV "/t/f", LX "/t/abs") == 0 && holds(LX "/t/abs", "x"));
    CHECK(symlink("/", LX "/t/root") == 0 && stat(LX "/t/root/u6v/t/f", &a) == 0);
    CHECK(symlink("../../u6v/t", LX "/t/up") == 0 && holds(LX "/t/up/f", "x"));
    CHECK(stat(LX "/..", &a) == 0 && stat("/", &b) == 0 && a.st_ino == b.st_ino);
    CHECK(holds(LX "/../u6v/t/f", "x"));
    CHECK(holds(LX "/t/../../u6/t/f", "x"));
    CHECK(chdir(LX "/..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));

    /* chdir through a link stores where it led. */
    CHECK(symlink("t/sub", LX "/ld") == 0);
    CHECK(chdir(LX "/ld") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, LX "/t/sub"));
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, LX "/t"));
    CHECK(holds("f", "x") && holds("sub/l2", "x"));
    int dfd = open(LX "/ld", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0 && fchdir(dfd) == 0 && getcwd(cwd, sizeof(cwd)) &&
          !strcmp(cwd, LX "/t/sub"));
    CHECK(dfd >= 0 && faccessat(dfd, "l2", R_OK, 0) == 0);
    if (dfd >= 0)
        close(dfd);
    CHECK(chdir("/") == 0);
}

static void hard_links(void)
{
    struct stat a, b;
    CHECK(put(LX "/t/h1", "one"));
    CHECK(link(LX "/t/h1", LX "/t/h2") == 0);
    CHECK(stat(LX "/t/h1", &a) == 0 && stat(LX "/t/h2", &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_nlink == 2 && b.st_nlink == 2);
    CHECK(link(LX "/t/h1", LX "/t/h2") == -1 && errno == EEXIST);
    CHECK(link(LX "/t/sub", LX "/t/hd") == -1 && errno == EPERM);
    CHECK(link(LX "/t/h1", LV "/t/h3") == -1 && errno == EXDEV);   /* one volume, two mounts */
    CHECK(link(LX "/t/none", LX "/t/h4") == -1 && errno == ENOENT);
    CHECK(put(LX "/t/h2", "two") && holds(LX "/t/h1", "two"));
    CHECK(unlink(LX "/t/h1") == 0 && stat(LX "/t/h2", &b) == 0 && b.st_nlink == 1);
    /* linkat follows no link unless asked. */
    CHECK(linkat(AT_FDCWD, LX "/t/l1", AT_FDCWD, LX "/t/hl", 0) == 0);
    CHECK(lstat(LX "/t/hl", &a) == 0 && S_ISLNK(a.st_mode));
    CHECK(linkat(AT_FDCWD, LX "/t/l1", AT_FDCWD, LX "/t/hf", AT_SYMLINK_FOLLOW) == 0);
    CHECK(lstat(LX "/t/hf", &a) == 0 && S_ISREG(a.st_mode) && a.st_nlink == 2);
}

static void attrs(void)
{
    struct stat st;
    CHECK(put(LX "/t/m", "m"));
    CHECK(chmod(LX "/t/m", 07751) == 0 && (mode_of(LX "/t/m") & 07777) == 07751);
    CHECK(chmod(LX "/t/m", 0600) == 0 && (mode_of(LX "/t/m") & 07777) == 0600);
    CHECK(access(LX "/t/m", R_OK | W_OK) == 0);
    CHECK(access(LX "/t/m", X_OK) == -1 && errno == EACCES);
    int fd = open(LX "/t/m", O_RDONLY);
    CHECK(fd >= 0 && fchmod(fd, 0755) == 0 && (mode_of(LX "/t/m") & 07777) == 0755);
    CHECK(access(LX "/t/m", X_OK) == 0);
    CHECK(fd >= 0 && fchown(fd, 1000, (gid_t)-1) == 0);
    CHECK(fd >= 0 && fchown(fd, 0, 0) == -1 && errno == EPERM);
    CHECK(chown(LX "/t/m", (uid_t)-1, 1000) == 0);
    CHECK(chown(LX "/t/m", 0, (gid_t)-1) == -1 && errno == EPERM);
    CHECK(stat(LX "/t/m", &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000);
    CHECK(chmod(LX "/t/l1", 0700) == 0 && (mode_of(LX "/t/f") & 0777) == 0700);   /* followed */

    struct timespec ts[2] = { { 1000000000, 123456789 }, { 1500000000, 987654321 } };
    CHECK(utimensat(AT_FDCWD, LX "/t/m", ts, 0) == 0);
    CHECK(stat(LX "/t/m", &st) == 0 && st.st_atim.tv_sec == 1000000000 &&
          st.st_atim.tv_nsec == 123456789 && st.st_mtim.tv_sec == 1500000000 &&
          st.st_mtim.tv_nsec == 987654321);
    struct timespec omit[2] = { { 0, UTIME_OMIT }, { 7, 5 } };
    CHECK(fd >= 0 && futimens(fd, omit) == 0);
    CHECK(stat(LX "/t/m", &st) == 0 && st.st_atim.tv_sec == 1000000000 &&
          st.st_mtim.tv_sec == 7 && st.st_mtim.tv_nsec == 5);
    struct timespec bad[2] = { { 0, 1000000000 }, { 0, 0 } };
    CHECK(utimensat(AT_FDCWD, LX "/t/m", bad, 0) == -1 && errno == EINVAL);
    time_t before = time(NULL);
    CHECK(utimensat(AT_FDCWD, LX "/t/m", NULL, 0) == 0);
    CHECK(stat(LX "/t/m", &st) == 0 && st.st_mtim.tv_sec >= before - 1 &&
          st.st_mtim.tv_sec <= time(NULL) + 1);
    if (fd >= 0)
        close(fd);

    /* The umask on every create. */
    mode_t old = umask(027);
    fd = open(LX "/t/um", O_WRONLY | O_CREAT, 0666);
    CHECK(fd >= 0 && (mode_of(LX "/t/um") & 0777) == 0640);
    if (fd >= 0)
        close(fd);
    CHECK(mkdir(LX "/t/umd", 0777) == 0 && (mode_of(LX "/t/umd") & 0777) == 0750);
    umask(old);
    CHECK(mknod(LX "/t/node", S_IFIFO | 0644, 0) == -1 && errno == EPERM);
}

static void renames(void)
{
    CHECK(put(LX "/t/e1", "one") && put(LX "/t/sub/e2", "two"));
    CHECK(renameat2(AT_FDCWD, LX "/t/e1", AT_FDCWD, LX "/t/sub/e2", RENAME_EXCHANGE) == 0);
    CHECK(holds(LX "/t/e1", "two") && holds(LX "/t/sub/e2", "one"));
    CHECK(renameat2(AT_FDCWD, LX "/t/e1", AT_FDCWD, LX "/t/sub/e2", RENAME_NOREPLACE) == -1 &&
          errno == EEXIST);
    CHECK(renameat2(AT_FDCWD, LX "/t/e1", AT_FDCWD, LX "/t/none", RENAME_EXCHANGE) == -1 &&
          errno == ENOENT);
    CHECK(rename(LX "/t/e1", LV "/t/e3") == -1 && errno == EXDEV);
    CHECK(rename(LX "/t/e1", LX "/t/e3") == 0 && holds(LX "/t/e3", "two"));
}

static void open_files(void)
{
    char buf[8] = { 0 };
    struct stat st;

    /* Unlink of an open file: it lives until its close. */
    CHECK(put(LX "/t/gone", "still"));
    int fd = open(LX "/t/gone", O_RDONLY);
    CHECK(fd >= 0 && unlink(LX "/t/gone") == 0);
    CHECK(fd >= 0 && read(fd, buf, 5) == 5 && !memcmp(buf, "still", 5));
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_nlink == 0);
    if (fd >= 0)
        close(fd);

    /* A writable O_APPEND descriptor to a child, written by us meanwhile. */
    CHECK(put(LX "/t/ap", "A"));
    fd = open(LX "/t/ap", O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int status = -1;
    char *av[] = { "posixtest", "--child-links", NULL };
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 1);
    CHECK(posix_spawn(&pid, "/bin/posixtest", &fa, NULL, av, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    CHECK(write(fd, "P", 1) == 1);   /* no EBUSY: the child's open is its own */
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(write(fd, "C", 1) == 1);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 4);
    if (fd >= 0)
        close(fd);
    CHECK(holds(LX "/t/ap", "APBC") || holds(LX "/t/ap", "ABPC"));

    /* Past 4 GiB (sparse). */
    fd = open(LX "/t/big", O_RDWR | O_CREAT, 0644);
    off_t at = (off_t)5 << 30;
    CHECK(fd >= 0 && pwrite(fd, "z", 1, at) == 1);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == at + 1);
    CHECK(fd >= 0 && pread(fd, buf, 1, at) == 1 && buf[0] == 'z');
    CHECK(fd >= 0 && ftruncate(fd, 0) == 0 && fsync(fd) == 0);
    if (fd >= 0)
        close(fd);
}

/* posixtest --child-links: a byte appended to descriptor 1. */
int pt_child_links(void)
{
    return write(1, "B", 1) == 1 ? 0 : 1;
}

void pt_links(char **argv)
{
    (void)argv;
    struct statfs sf;
    if (statfs(LX, &sf) || sf.f_type != EXT4_MAGIC) {
        printf("posixtest: links: no ext volume at %s (utest extfs_posix mounts one)\n", LX);
        CHECK(!"an ext volume at " LX);
        return;
    }
    CHECK(sf.f_namelen == 255);
    symlinks();
    outward();
    hard_links();
    attrs();
    renames();
    open_files();
}
