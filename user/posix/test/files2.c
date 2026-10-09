/* posixtest's files section, part two: a directory of 10,000 files
 * listed, telldir and seekdir, the current directory, the *at calls,
 * what stat and statvfs say, and MAP_PRIVATE of a file. The answers are
 * Linux's (files.c says how they were written) but where M13's plan
 * chose otherwise: ".." stops at a mount's root (docs/M13-PLAN.md
 * section 3), so chdir("/data/..") stays in /data. */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "ptfiles.h"

#define MANY      10000   /* files in one directory */
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

/* 10,000 files in one directory: made, listed (every name once, "." and
 * ".." too), a listing taken up again with seekdir, removed. */
static void many_files(void)
{
    static unsigned char seen[MANY];
    CHECK(mkdir(PT_DIR "/many", 0755) == 0);
    char name[64];
    uint64_t t0 = ms_now();
    for (int i = 0; i < MANY; i++) {
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
    CHECK(list_rest(d, seen, &dots) == MANY && dots == 2);
    /* telldir and seekdir halfway, then the same name again */
    rewinddir(d);
    for (int i = 0; i < MANY / 2; i++)
        CHECK(readdir(d) != NULL);
    long at = telldir(d);
    struct dirent *e = readdir(d);
    char first[NAME_MAX + 1] = "";
    if (e)
        snprintf(first, sizeof(first), "%s", e->d_name);
    seekdir(d, at);
    e = readdir(d);
    CHECK(e && !strcmp(e->d_name, first));
    closedir(d);
    uint64_t t2 = ms_now();
    int gone = 0;
    for (int i = 0; i < MANY; i++) {
        snprintf(name, sizeof(name), PT_DIR "/many/f%05d", i);
        gone += unlink(name) == 0;
    }
    CHECK(gone == MANY && rmdir(PT_DIR "/many") == 0);
    printf("posixtest: %d files made in %llu ms, listed in %llu ms, removed in %llu ms\n",
           MANY, (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
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
    CHECK(chdir("/data/..") == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/data"));
    errno = 0;
    CHECK(getcwd(cwd, 3) == NULL && errno == ERANGE);   /* getcwd(3) ERANGE */
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
    close(dfd);
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
    CHECK(munmap(m, 3 * 4096) == 0);
    m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    CHECK(m != MAP_FAILED && !memcmp(m, data + 4096, 4096));
    if (m != MAP_FAILED)
        munmap(m, 4096);
    close(fd);
}

void pt_files2(void)
{
    many_files();
    current_dir();
    at_calls();
    stats();
    map_private();
    pt_files_children();
}
