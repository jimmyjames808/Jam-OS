/* posixtest's files section, part two: a directory of 10,000 files
 * listed, telldir and seekdir, the current directory (also removed under
 * us), the *at calls, 255-byte names nested by descriptors, what stat and
 * statvfs say, and MAP_PRIVATE of a file. The answers are
 * Linux's (files.c says how they were written) but where M13's plan
 * chose otherwise: ".." stops at a FAT mount's root (docs/M13-PLAN.md
 * section 3), so chdir("/data/..") stays in /data there; above an ext
 * mount's root it is "/" (M13.6). */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "ptfiles.h"

#define MANY      10000   /* files in one directory: files-many */
#define MANY_EXT  7000    /* ... on ext: the image's 64 MiB volume has 8192 inodes (statfs
                           * doesn't say how many are free) */
#define FEW       300     /* ... in the files section: still several readdir batches */
#define MAP_BYTES 10000   /* the mapped file's size: two pages and part of a third */

/* Remove path and everything under it (opendir and readdir as any
 * program walks a tree); 0 or -1. */
static int rm_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    char *sub = malloc(PATH_MAX);
    int r = sub ? 0 : -1;
    while (!r && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, PATH_MAX, "%s/%s", path, e->d_name);
        r = rm_tree(sub);
        if (!r)
            rewinddir(d);   /* the listing changed under us: from the start again */
    }
    closedir(d);
    free(sub);
    return r ? r : rmdir(path);
}

bool pt_files_start(void)
{
    struct stat st;
    for (int i = 0; i < 300 && stat("/data", &st); i++) {   /* volumes mounts it: 30 s */
        struct timespec t = { 0, 100 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (stat("/data", &st) || !S_ISDIR(st.st_mode))
        return false;
    CHECK(rm_tree(PT_DIR) == 0);
    CHECK(mkdir(PT_DIR, 0755) == 0);
    return true;
}

void pt_files_end(void)
{
    CHECK(rm_tree(PT_DIR) == 0);
    struct stat st;
    CHECK(stat(PT_DIR, &st) == -1 && errno == ENOENT);
}

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* Read the listing to its end, marking each fNNNNN in seen; the count of
 * names, -1 for one twice or unknown. */
static int list_rest(DIR *d, unsigned char *seen, int *dots)
{
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            (*dots)++;
            CHECK(e->d_type == DT_DIR);
            continue;
        }
        int k = -1;
        if (sscanf(e->d_name, "f%d", &k) != 1 || k < 0 || k >= MANY || seen[k] ||
            e->d_type != DT_REG)
            return -1;
        seen[k] = 1;
        n++;
    }
    return n;
}

/* count files in one directory: made, listed (every name once, "." and
 * ".." too), a listing taken up again with seekdir, removed. */
static void many_files(int count)
{
    static unsigned char seen[MANY];
    CHECK(mkdir(PT_DIR "/many", 0755) == 0);
    char name[64];
    uint64_t t0 = ms_now();
    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof(name), PT_DIR "/many/f%05d", i);
        int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            CHECK(!"made every file");
            printf("posixtest: file %d: %s\n", i, strerror(errno));
            return;
        }
        close(fd);
    }
    uint64_t t1 = ms_now();
    DIR *d = opendir(PT_DIR "/many");
    CHECK(d != NULL);
    if (!d)
        return;
    int dots = 0;
    memset(seen, 0, sizeof(seen));
    CHECK(list_rest(d, seen, &dots) == count && dots == 2);
    /* telldir and seekdir halfway, then the same name again */
    rewinddir(d);
    for (int i = 0; i < count / 2; i++)
        CHECK(readdir(d) != NULL);
    long at = telldir(d);
    struct dirent *e = readdir(d);
    char first[NAME_MAX + 1] = "";
    if (e)
        snprintf(first, sizeof(first), "%s", e->d_name);
    seekdir(d, at);
    e = readdir(d);
    CHECK(e && !strcmp(e->d_name, first));
    /* rewinddir after a change lists the directory as it is now (POSIX:
     * rewinddir "shall also cause the directory stream to refer to the
     * current state of the directory") */
    snprintf(name, sizeof(name), PT_DIR "/many/f%05d", 0);
    CHECK(unlink(name) == 0);
    rewinddir(d);
    dots = 0;
    memset(seen, 0, sizeof(seen));
    CHECK(list_rest(d, seen, &dots) == count - 1 && !seen[0]);
    closedir(d);
    uint64_t t2 = ms_now();
    int gone = 0;
    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof(name), PT_DIR "/many/f%05d", i);
        gone += unlink(name) == 0;
    }
    CHECK(gone == count - 1 && rmdir(PT_DIR "/many") == 0);
    printf("posixtest: %d files made in %llu ms, listed in %llu ms, removed in %llu ms\n",
           count, (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
           (unsigned long long)(ms_now() - t2));
}

/* The current directory: chdir, getcwd, relative paths, fchdir. */
static void current_dir(void)
{
    char cwd[PATH_MAX];
    CHECK(chdir(PT_DIR) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR));
    int fd = open("f", O_RDONLY);
    char buf[8] = { 0 };
    CHECK(fd >= 0 && read(fd, buf, 5) == 5 && !strcmp(buf, "hello"));
    close(fd);
    CHECK(chdir("d") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR "/d"));
    CHECK(chdir("../.") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR));
    /* The directory removed while we are in it: getcwd(2) ENOENT (base's
     * MakeAbsoluteFilePathNoResolveSymbolicLinks), and nothing is made in it.
     * (Linux's chdir("..") still leads out of it; ours is a path as text, so
     * only an absolute chdir does.) */
    CHECK(mkdir("gone", 0755) == 0 && chdir("gone") == 0 && getcwd(cwd, sizeof(cwd)));
    CHECK(rmdir(PT_DIR "/gone") == 0);
    errno = 0;
    CHECK(getcwd(cwd, sizeof(cwd)) == NULL && errno == ENOENT);
    errno = 0;
    CHECK(open("x", O_WRONLY | O_CREAT, 0644) == -1 && errno == ENOENT);
    CHECK(chdir(PT_DIR) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR));
    errno = 0;
    CHECK(getcwd(cwd, 3) == NULL && errno == ERANGE);   /* getcwd(3) ERANGE */
    /* ".." above a mount's root: an ext volume's is "/" (links.c); FAT's /data stayed in /data */
    CHECK(chdir("/data/..") == 0 && getcwd(cwd, sizeof(cwd)) &&
          !strcmp(cwd, pt_data_ext() ? "/" : "/data"));
    int dfd = open(PT_DIR "/d", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0 && fchdir(dfd) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR "/d"));
    close(dfd);
    CHECK(chdir("/") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
    DIR *root = opendir(".");
    bool data = false, boot = false;
    struct dirent *e;
    while (root && (e = readdir(root)) != NULL) {
        data |= !strcmp(e->d_name, "data");
        boot |= !strcmp(e->d_name, "boot");
    }
    CHECK(root && data && boot);   /* "/" lists the mounts */
    if (root)
        closedir(root);
}

/* openat, mkdirat, fstatat, renameat, unlinkat, faccessat on a directory
 * descriptor; fdopendir; O_PATH. */
static void at_calls(void)
{
    int dfd = open(PT_DIR, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    int fd = openat(dfd, "f", O_RDONLY);
    char buf[8] = { 0 };
    CHECK(fd >= 0 && read(fd, buf, 5) == 5 && !strcmp(buf, "hello"));
    close(fd);
    struct stat st;
    CHECK(mkdirat(dfd, "at1", 0755) == 0);
    CHECK(fstatat(dfd, "at1", &st, 0) == 0 && S_ISDIR(st.st_mode));
    CHECK(renameat(dfd, "at1", dfd, "at2") == 0);
    CHECK(unlinkat(dfd, "at2", 0) == -1 && errno == EISDIR);
    CHECK(unlinkat(dfd, "at2", AT_REMOVEDIR) == 0);
    CHECK(faccessat(dfd, "f", R_OK, 0) == 0);
    CHECK(fstatat(dfd, "", &st, AT_EMPTY_PATH) == 0 && S_ISDIR(st.st_mode));
    CHECK(fstatat(dfd, "f", &st, 0x8000) == -1 && errno == EINVAL);   /* fstatat(2): flags */
    DIR *d = fdopendir(dup(dfd));
    bool f = false, dot = false;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        f |= !strcmp(e->d_name, "f") && e->d_type == DT_REG;
        dot |= !strcmp(e->d_name, ".");
    }
    CHECK(d && f && dot);
    if (d)
        closedir(d);
    int p = open(PT_DIR "/f", O_PATH);
    CHECK(p >= 0 && fstat(p, &st) == 0 && st.st_size == 5);
    CHECK(read(p, buf, 1) == -1 && errno == EBADF);   /* open(2) O_PATH: no reading */
    close(p);
    /* O_PATH on a directory (musl's O_ACCMODE holds O_PATH, so this once
     * failed with EISDIR; libarchive's extraction opens each directory so):
     * a descriptor for the *at calls and fstat, that lists and reads nothing. */
    int pd = open(PT_DIR, O_PATH | O_DIRECTORY);
    CHECK(pd >= 0);
    if (pd >= 0) {
        CHECK(fstat(pd, &st) == 0 && S_ISDIR(st.st_mode));
        CHECK(fcntl(pd, F_GETFL) & O_PATH);
        fd = openat(pd, "f", O_RDONLY);
        memset(buf, 0, sizeof(buf));
        CHECK(fd >= 0 && read(fd, buf, 5) == 5 && !strcmp(buf, "hello"));
        close(fd);
        CHECK(fstatat(pd, "d", &st, 0) == 0 && S_ISDIR(st.st_mode));
        char ents[256];
        errno = 0;
        CHECK(syscall(SYS_getdents64, pd, ents, sizeof(ents)) == -1 && errno == EBADF);
        close(pd);
    }
    errno = 0;
    CHECK(open(PT_DIR "/f", O_PATH | O_DIRECTORY) == -1 && errno == ENOTDIR);
    CHECK(open(PT_DIR, O_WRONLY) == -1 && errno == EISDIR);   /* still not for writing */
    int ep = open("/etc/passwd", O_PATH);   /* a file libc itself serves */
    CHECK(ep >= 0 && read(ep, buf, 1) == -1 && errno == EBADF);
    if (ep >= 0)
        close(ep);
    close(dfd);
}

/* Do a and b (absolute or relative, to the current directory) name one
 * file? */
static bool same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    return !stat(a, &sa) && !stat(b, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* As same_file, a relative to the directory descriptor dfd. */
static bool same_file_at(int dfd, const char *a, const char *b)
{
    struct stat sa, sb;
    return !fstatat(dfd, a, &sa, 0) && !stat(b, &sb) && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

/* A relative path goes on from where its base ends (the layer takes the
 * current directory's or a descriptor's text as it is, and walks the path
 * on from its end): from "/" (a mount named without a slash), from a
 * mount's root, from a directory by the current directory and by a
 * descriptor, with "." and ".." and slashes in it, from a directory that
 * was reached through a link, and from a /proc directory. */
static void path_bases(void)
{
    char cwd[PATH_MAX];
    CHECK(mkdir(PT_DIR "/pb", 0755) == 0 && mkdir(PT_DIR "/pb/x", 0755) == 0);
    CHECK(chdir("/") == 0 && same_file("data", "/data"));
    CHECK(same_file("data/./ptfiles//pb/x/..", PT_DIR "/pb"));
    CHECK(chdir("/data") == 0 && same_file(".", "/data") &&
          same_file("ptfiles/pb//x/", PT_DIR "/pb/x"));
    CHECK(chdir(PT_DIR "/pb") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR "/pb"));
    CHECK(same_file("x/../x/.", PT_DIR "/pb/x") && same_file("../pb/x", PT_DIR "/pb/x") &&
          same_file(".", PT_DIR "/pb") && same_file("..", PT_DIR));
    int dfd = open(PT_DIR "/pb/x", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    CHECK(chdir("/") == 0);   /* a descriptor's base is not the current directory's */
    CHECK(same_file_at(dfd, ".", PT_DIR "/pb/x") && same_file_at(dfd, "..", PT_DIR "/pb") &&
          same_file_at(dfd, "../x/./", PT_DIR "/pb/x") &&
          same_file_at(dfd, "../../pb", PT_DIR "/pb"));
    if (dfd >= 0)
        close(dfd);
    if (pt_data_ext()) {   /* the base is the physical directory the link led to */
        CHECK(symlink("x", PT_DIR "/pb/lnk") == 0 && chdir(PT_DIR "/pb/lnk") == 0);
        CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, PT_DIR "/pb/x") &&
              same_file("..", PT_DIR "/pb"));
        CHECK(chdir("/") == 0 && unlink(PT_DIR "/pb/lnk") == 0);
    }
    int pfd = open("/proc/self", O_RDONLY | O_DIRECTORY);   /* libc's own tree: its base is made */
    struct stat st;
    CHECK(pfd >= 0 && fstatat(pfd, "fd", &st, 0) == 0 && S_ISDIR(st.st_mode) &&
          fstatat(pfd, "fd/..", &st, 0) == 0 && S_ISDIR(st.st_mode));
    if (pfd >= 0)
        close(pfd);
    CHECK(rmdir(PT_DIR "/pb/x") == 0 && rmdir(PT_DIR "/pb") == 0);
}

/* Names of NAME_MAX bytes, one inside another through directory descriptors
 * (the start of base's DeleteDeep: 26 levels, deleted from the top). A
 * name of 255 bytes is made, opened, listed and removed, and 256 is
 * ENAMETOOLONG; the tree goes down as far as a path the layer keeps and the
 * fs protocol carries (4095 bytes: PATH_BYTES, FS_PATH_LEN) holds, 15 levels
 * of 256 bytes under PT_DIR/deep, and the next level is ENAMETOOLONG where
 * Linux goes on (a directory description here is a path; DeleteDeep's 26
 * levels need directory handles in the fs protocol). Removed bottom up. */
static void deep_names(void)
{
    enum { LEVELS = 24 };
    char name[258];
    memset(name, 'a', 255);
    name[255] = '\0';
    CHECK(mkdir(PT_DIR "/deep", 0755) == 0);
    int fds[LEVELS + 1];
    fds[0] = open(PT_DIR "/deep", O_RDONLY | O_DIRECTORY);
    int levels = 0;
    while (levels < LEVELS && fds[levels] >= 0) {
        if (mkdirat(fds[levels], name, 0755))
            break;
        int next = openat(fds[levels], name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        CHECK(next >= 0);
        fds[++levels] = next;
    }
    int why = errno;
    CHECK(levels >= 14 && levels < LEVELS && why == ENAMETOOLONG);   /* 13 + 256 * 15 is 3853 */
    CHECK(fds[levels] >= 0);
    if (fds[levels] >= 0) {
        struct stat st;
        int f = openat(fds[levels], "file.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(f >= 0 && write(f, "deep", 4) == 4);
        if (f >= 0)
            close(f);
        CHECK(fstatat(fds[levels], "file.txt", &st, 0) == 0 && st.st_size == 4);
        if (pt_data_ext())   /* base's tree has a link at the bottom too */
            CHECK(symlinkat("..", fds[levels], "up") == 0);
        DIR *d = fdopendir(dup(fds[levels]));
        bool listed = false;
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
            listed |= !strcmp(e->d_name, "file.txt");
        CHECK(d && listed);
        if (d)
            closedir(d);
        CHECK(unlinkat(fds[levels], "file.txt", 0) == 0);
        if (pt_data_ext())
            CHECK(unlinkat(fds[levels], "up", 0) == 0);
    }
    for (int i = levels; i > 0; i--) {   /* each directory is removed through its parent's */
        if (fds[i] >= 0)
            close(fds[i]);
        CHECK(unlinkat(fds[i - 1], name, AT_REMOVEDIR) == 0);
    }
    if (fds[0] >= 0)
        close(fds[0]);
    CHECK(rmdir(PT_DIR "/deep") == 0);
    /* The longest name, and one more. */
    int dfd = open(PT_DIR, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0 && mkdirat(dfd, name, 0755) == 0);
    struct stat st;
    CHECK(fstatat(dfd, name, &st, 0) == 0 && S_ISDIR(st.st_mode));
    CHECK(unlinkat(dfd, name, AT_REMOVEDIR) == 0);
    name[255] = 'a';
    name[256] = '\0';
    errno = 0;
    CHECK(mkdirat(dfd, name, 0755) == -1 && errno == ENAMETOOLONG);
    errno = 0;
    CHECK(openat(dfd, name, O_RDONLY) == -1 && errno == ENAMETOOLONG);
    if (dfd >= 0)
        close(dfd);
}

#define SF_BIG 150000   /* more than sendfile's 64 KiB at a time */

/* sendfile: from a file to a file at both descriptors' offsets, with an
 * offset pointer (the input's own offset stays), to a socket; the count
 * cut at the end of the file; a pipe or a socket as input is EINVAL (base's
 * CopyFileContentsWithSendfile falls back to a plain copy on it). */
static void sendfile_calls(void)
{
    char buf[32];
    int in = open(PT_DIR "/sf-in", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int out = open(PT_DIR "/sf-out", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(in >= 0 && out >= 0);
    if (in < 0 || out < 0)
        return;
    CHECK(write(in, "0123456789ABCDEF", 16) == 16 && write(out, "GHIJKL", 6) == 6);
    /* Both at 1: the output's offset is G and the rest of its old contents. */
    CHECK(lseek(in, 1, SEEK_SET) == 1 && lseek(out, 1, SEEK_SET) == 1);
    CHECK(sendfile(out, in, NULL, 15) == 15);
    CHECK(lseek(in, 0, SEEK_CUR) == 16 && lseek(out, 0, SEEK_CUR) == 16);
    memset(buf, 0, sizeof(buf));
    CHECK(pread(out, buf, 16, 0) == 16 && !memcmp(buf, "G123456789ABCDEF", 16));
    CHECK(sendfile(out, in, NULL, 10) == 0);   /* the input is at its end */
    /* An offset pointer: it moves on, the input's own offset doesn't. */
    off_t off = 4;
    CHECK(lseek(in, 2, SEEK_SET) == 2 && lseek(out, 0, SEEK_SET) == 0);
    CHECK(sendfile(out, in, &off, 5) == 5 && off == 9);
    CHECK(lseek(in, 0, SEEK_CUR) == 2 && lseek(out, 0, SEEK_CUR) == 5);
    memset(buf, 0, sizeof(buf));
    CHECK(pread(out, buf, 5, 0) == 5 && !memcmp(buf, "45678", 5));
    off = 12;
    CHECK(sendfile(out, in, &off, 100) == 4 && off == 16);   /* cut at the end */
    off = 16;
    CHECK(sendfile(out, in, &off, 5) == 0 && off == 16);
    off = 3;
    CHECK(sendfile(out, in, &off, 0) == 0 && off == 3);
    off = -1;
    errno = 0;
    CHECK(sendfile(out, in, &off, 1) == -1 && errno == EINVAL);
    /* The wrong kinds. */
    int wo = open(PT_DIR "/sf-out", O_WRONLY), ro = open(PT_DIR "/sf-out", O_RDONLY);
    errno = 0;
    CHECK(sendfile(out, wo, NULL, 1) == -1 && errno == EBADF);    /* input not readable */
    CHECK(sendfile(ro, in, NULL, 1) == -1 && errno == EBADF);     /* output not writable */
    CHECK(sendfile(out, 999, NULL, 1) == -1 && errno == EBADF);
    int pp[2];
    CHECK(pipe(pp) == 0);
    CHECK(sendfile(out, pp[0], NULL, 1) == -1 && errno == EINVAL);
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    CHECK(sendfile(out, sp[1], NULL, 1) == -1 && errno == EINVAL);
    /* To a socket and to a pipe. */
    CHECK(lseek(in, 0, SEEK_SET) == 0);
    CHECK(sendfile(sp[0], in, NULL, 16) == 16);
    memset(buf, 0, sizeof(buf));
    CHECK(read(sp[1], buf, 16) == 16 && !memcmp(buf, "0123456789ABCDEF", 16));
    off = 10;
    CHECK(sendfile(pp[1], in, &off, 6) == 6 && off == 16);
    memset(buf, 0, sizeof(buf));
    CHECK(read(pp[0], buf, 6) == 6 && !memcmp(buf, "ABCDEF", 6));
    for (int i = 0; i < 2; i++) {
        close(pp[i]);
        close(sp[i]);
    }
    if (wo >= 0)
        close(wo);
    if (ro >= 0)
        close(ro);
    close(in);
    close(out);
    /* More than one chunk, at an offset. */
    static unsigned char big[SF_BIG], back[SF_BIG];
    for (int i = 0; i < SF_BIG; i++)
        big[i] = (unsigned char)(i * 7 + (i >> 8));
    in = open(PT_DIR "/sf-big-in", O_RDWR | O_CREAT | O_TRUNC, 0644);
    out = open(PT_DIR "/sf-big-out", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(in >= 0 && out >= 0 && write(in, big, SF_BIG) == SF_BIG);
    off = 1000;
    CHECK(sendfile(out, in, &off, SF_BIG) == SF_BIG - 1000 && off == SF_BIG);
    CHECK(pread(out, back, SF_BIG - 1000, 0) == SF_BIG - 1000 &&
          !memcmp(back, big + 1000, SF_BIG - 1000));
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    unlink(PT_DIR "/sf-in");
    unlink(PT_DIR "/sf-out");
    unlink(PT_DIR "/sf-big-in");
    unlink(PT_DIR "/sf-big-out");
}

/* What stat and statvfs say about /data, /boot, "/" and /svc. */
static void stats(void)
{
    struct stat a, b, prog, cfg, boot, root;
    CHECK(stat(PT_DIR "/f", &a) == 0 && stat(PT_DIR "/./d/../f", &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_dev == b.st_dev && S_ISREG(a.st_mode));
    CHECK(stat(PT_DIR "/d", &b) == 0 && S_ISDIR(b.st_mode) && (b.st_mode & 0777) == 0755);
    CHECK(stat("/boot/bin/posixtest", &prog) == 0 && (prog.st_mode & 07777) == 0555);
    CHECK(stat("/boot/init.cfg", &cfg) == 0 && (cfg.st_mode & 07777) == 0444);
    CHECK(stat("/boot", &boot) == 0 && S_ISDIR(boot.st_mode) && boot.st_dev != a.st_dev);
    CHECK(stat("/", &root) == 0 && S_ISDIR(root.st_mode));
    if (pt_have_svc("stat of /svc"))
        CHECK(stat("/svc", &root) == 0 && S_ISDIR(root.st_mode));
    struct statvfs v;
    CHECK(statvfs("/boot", &v) == 0 && (v.f_flag & ST_RDONLY));
    CHECK(statvfs(PT_DIR, &v) == 0 && !(v.f_flag & ST_RDONLY) && v.f_bsize == 4096);
    CHECK(v.f_blocks > 0 && v.f_bfree <= v.f_blocks);
    errno = 0;
    CHECK(statvfs(PT_DIR "/none", &v) == -1 && errno == ENOENT);
}

/* MAP_PRIVATE of a file: a copy, zero past the end, its writes the
 * mapping's alone. */
static void map_private(void)
{
    static unsigned char data[MAP_BYTES];
    for (int i = 0; i < MAP_BYTES; i++)
        data[i] = (unsigned char)(i * 13 + 1);
    int fd = open(PT_DIR "/map", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, data, MAP_BYTES) == MAP_BYTES);
    unsigned char *m = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    CHECK(!memcmp(m, data, MAP_BYTES));
    bool zero = true;
    for (int i = MAP_BYTES; i < 3 * 4096; i++)
        zero &= m[i] == 0;
    CHECK(zero);
    m[0] = 0xEE;
    unsigned char c = 0;
    CHECK(pread(fd, &c, 1, 0) == 1 && c == data[0]);   /* the file never sees it */
    errno = 0;
    CHECK(munmap(m, 3 * 4096) == 0);
    if (errno)
        printf("posixtest: munmap: %s\n", strerror(errno));
    m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    CHECK(m != MAP_FAILED && !memcmp(m, data + 4096, 4096));
    if (m != MAP_FAILED)
        munmap(m, 4096);
    close(fd);
}

/* The files-many section, asked for by name (tools/shell-tests/posix-files.txt):
 * 10,000 files in one directory. On QEMU's USB stick each file takes
 * about 8 ms to make and 6.5 to remove (fat writes each through), too
 * long for the init run. */
void pt_files_many(char **argv)
{
    (void)argv;
    if (!pt_files_start()) {
        CHECK(!"no /data to test on");
        return;
    }
    many_files(pt_data_ext() ? MANY_EXT : MANY);
    pt_files_end();
}

void pt_files2(void)
{
    many_files(FEW);
    current_dir();
    at_calls();
    path_bases();
    deep_names();
    sendfile_calls();
    stats();
    map_private();
    pt_files_children();
}
