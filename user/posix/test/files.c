/* posixtest's files section, part one: every file call's errors against
 * the errno Linux gives for the same steps, then reads and writes.
 *
 * The expected answers are Linux's, written from POSIX.1-2024 and the
 * Linux man pages (man-pages 6.x) for each call: the page and its error
 * are named on each row. Linux couldn't be run beside this to check
 * them, so where its answer depends on the order of its own checks the
 * row says which check comes first (from the man page, or where the page
 * is silent, from fs/namei.c's documented order: the existence of the
 * last name before write access for mkdir and O_CREAT, write access
 * before it for unlink, rmdir and rename).
 *
 * Everything happens under PT_DIR on /data (a FAT volume) and on /boot
 * (the boot image: a read-only mount). files2.c has part two. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include "ptfiles.h"

#define BIG 200000   /* bytes: more than three transfer buffers */

static char longname[300], longpath[5000];

enum op { OPEN, MKDIR, RMDIR, UNLINK, RENAME, STAT, ACCESS, TRUNC, CHDIR, LINK, SYMLINK,
          READLINK, CHMOD };

/* One row: the call, its arguments, the answer (0: success), the source. */
struct row {
    enum op     op;
    const char *a, *b;
    long        n;        /* open's flags, access's mode, truncate's length */
    int         expect;   /* 0 or an errno */
    const char *why;      /* the man page and its line */
};

#define D PT_DIR
static const struct row rows[] = {
    { OPEN, D "/none", 0, O_RDONLY, ENOENT, "open(2) ENOENT: no such file" },
    { OPEN, D "/f", 0, O_CREAT | O_EXCL | O_WRONLY, EEXIST, "open(2) EEXIST: O_CREAT|O_EXCL" },
    { OPEN, D "/d", 0, O_WRONLY, EISDIR, "open(2) EISDIR: a directory opened for writing" },
    { OPEN, D "/d", 0, O_RDWR, EISDIR, "open(2) EISDIR" },
    { OPEN, D "/d", 0, O_RDONLY | O_CREAT, EISDIR, "open(2) EISDIR: O_CREAT on a directory" },
    { OPEN, D "/d", 0, O_RDONLY, 0, "open(2): a directory may be opened to read" },
    { OPEN, D "/f", 0, O_RDONLY | O_DIRECTORY, ENOTDIR, "open(2) ENOTDIR: O_DIRECTORY" },
    { OPEN, D "/f/x", 0, O_RDONLY, ENOTDIR, "path_resolution(7): a prefix that is no directory" },
    { OPEN, D "/f/", 0, O_RDONLY, ENOTDIR, "path_resolution(7): a trailing slash" },
    { OPEN, D "/new/", 0, O_CREAT | O_WRONLY, EISDIR, "open(2) EISDIR: O_CREAT with a trailing slash" },
    { OPEN, D "/none/x", 0, O_CREAT | O_WRONLY, ENOENT, "open(2) ENOENT: a directory missing" },
    { OPEN, "", 0, O_RDONLY, ENOENT, "path_resolution(7): an empty path" },
    { OPEN, D "/f", 0, O_RDONLY | O_CREAT | O_DIRECTORY, EINVAL, "open(2) EINVAL: O_CREAT|O_DIRECTORY" },
    { OPEN, longname, 0, O_CREAT | O_WRONLY, ENAMETOOLONG, "open(2) ENAMETOOLONG: a name over 255" },
    { OPEN, longpath, 0, O_RDONLY, ENAMETOOLONG, "open(2) ENAMETOOLONG: over PATH_MAX" },
    { OPEN, "/svc", 0, O_WRONLY, EISDIR, "open(2) EISDIR: /svc lists the services" },
    { MKDIR, D "/d", 0, 0, EEXIST, "mkdir(2) EEXIST" },
    { MKDIR, D "/f", 0, 0, EEXIST, "mkdir(2) EEXIST: a file of that name" },
    { MKDIR, D "/none/x", 0, 0, ENOENT, "mkdir(2) ENOENT" },
    { MKDIR, D "/f/x", 0, 0, ENOTDIR, "mkdir(2) ENOTDIR" },
    { MKDIR, "/data", 0, 0, EEXIST, "mkdir(2) EEXIST: a mount point" },
    { MKDIR, "/nomount", 0, 0, EROFS, "mkdir(2) EROFS: \"/\" is read-only" },
    { RMDIR, D "/full", 0, 0, ENOTEMPTY, "rmdir(2) ENOTEMPTY" },
    { RMDIR, D "/f", 0, 0, ENOTDIR, "rmdir(2) ENOTDIR" },
    { RMDIR, D "/none", 0, 0, ENOENT, "rmdir(2) ENOENT" },
    { RMDIR, D "/d/.", 0, 0, EINVAL, "rmdir(2) EINVAL: the last name is ." },
    { RMDIR, D "/d/..", 0, 0, ENOTEMPTY, "rmdir(2) ENOTEMPTY: the last name is .." },
    { RMDIR, "/data", 0, 0, EBUSY, "rmdir(2) EBUSY: a mount point" },
    { UNLINK, D "/d", 0, 0, EISDIR, "unlink(2) EISDIR: Linux's answer for a directory" },
    { UNLINK, D "/none", 0, 0, ENOENT, "unlink(2) ENOENT" },
    { UNLINK, D "/f/", 0, 0, ENOTDIR, "unlink(2) ENOTDIR: a trailing slash on a file" },
    { UNLINK, D "/f/x", 0, 0, ENOTDIR, "unlink(2) ENOTDIR: a prefix that is a file" },
    { RENAME, D "/none", D "/x", 0, ENOENT, "rename(2) ENOENT" },
    { RENAME, D "/f", D "/d", 0, EISDIR, "rename(2) EISDIR: a file over a directory" },
    { RENAME, D "/d", D "/f", 0, ENOTDIR, "rename(2) ENOTDIR: a directory over a file" },
    { RENAME, D "/d", D "/full", 0, ENOTEMPTY, "rename(2) ENOTEMPTY: over a full directory" },
    { RENAME, D "/d", D "/d/sub", 0, EINVAL, "rename(2) EINVAL: into itself" },
    { RENAME, D "/f", "/boot/x", 0, EXDEV, "rename(2) EXDEV: another mount" },
    { RENAME, D "/f", D "/f", 0, 0, "rename(2): the same name does nothing" },
    { RENAME, D "/f/", D "/g", 0, ENOTDIR, "rename(2) ENOTDIR: a trailing slash on a file" },
    { RENAME, "/data", D "/m", 0, EBUSY, "rename(2) EBUSY: a mount point" },
    { STAT, D "/none", 0, 0, ENOENT, "stat(2) ENOENT" },
    { STAT, D "/f/x", 0, 0, ENOTDIR, "stat(2) ENOTDIR" },
    { STAT, D "/d/", 0, 0, 0, "stat(2): a trailing slash on a directory" },
    { ACCESS, D "/f", 0, X_OK, EACCES, "access(2) EACCES: mode 0644" },
    { ACCESS, D "/f", 0, R_OK | W_OK, 0, "access(2)" },
    { ACCESS, D "/d", 0, X_OK, 0, "access(2): a directory may be searched" },
    { ACCESS, D "/none", 0, F_OK, ENOENT, "access(2) ENOENT" },
    { ACCESS, D "/f", 0, 0x10, EINVAL, "access(2) EINVAL: a mode bit unknown" },
    { TRUNC, D "/d", 0, 0, EISDIR, "truncate(2) EISDIR" },
    { TRUNC, D "/f", 0, -1, EINVAL, "truncate(2) EINVAL: a negative length" },
    { TRUNC, D "/none", 0, 0, ENOENT, "truncate(2) ENOENT" },
    { CHDIR, D "/f", 0, 0, ENOTDIR, "chdir(2) ENOTDIR" },
    { CHDIR, D "/none", 0, 0, ENOENT, "chdir(2) ENOENT" },
    { LINK, D "/f", D "/l", 0, EPERM, "link(2) EPERM: the filesystem has no hard links (vfat)" },
    { LINK, D "/none", D "/l", 0, ENOENT, "link(2) ENOENT" },
    { LINK, D "/f", D "/d", 0, EEXIST, "link(2) EEXIST" },
    { SYMLINK, "f", D "/s", 0, EPERM, "symlink(2) EPERM: no symbolic links (vfat)" },
    { READLINK, D "/f", 0, 0, EINVAL, "readlink(2) EINVAL: not a symbolic link" },
    { READLINK, D "/none", 0, 0, ENOENT, "readlink(2) ENOENT" },
    { CHMOD, D "/f", 0, 0600, 0, "chmod(2): FAT keeps no modes; it succeeds" },
    { CHMOD, D "/none", 0, 0600, ENOENT, "chmod(2) ENOENT" },
    /* /boot: the boot image, a read-only mount */
    { OPEN, "/boot/bin/posixtest", 0, O_WRONLY, EROFS, "open(2) EROFS: write on read-only" },
    { OPEN, "/boot/bin/posixtest", 0, O_RDONLY | O_TRUNC, EROFS, "open(2) EROFS: O_TRUNC" },
    { OPEN, "/boot/new", 0, O_CREAT | O_WRONLY, EROFS, "open(2) EROFS: creating" },
    { OPEN, "/boot/new", 0, O_CREAT | O_RDONLY, EROFS, "open(2) EROFS: creating, even to read" },
    { OPEN, "/boot/bin/posixtest", 0, O_CREAT | O_RDONLY, 0, "open(2): O_CREAT of a file there" },
    { MKDIR, "/boot/bin", 0, 0, EEXIST, "mkdir(2) EEXIST: looked for before EROFS" },
    { MKDIR, "/boot/newdir", 0, 0, EROFS, "mkdir(2) EROFS" },
    { UNLINK, "/boot/bin/posixtest", 0, 0, EROFS, "unlink(2) EROFS" },
    { RMDIR, "/boot/bin", 0, 0, EROFS, "rmdir(2) EROFS" },
    { RENAME, "/boot/bin/posixtest", "/boot/x", 0, EROFS, "rename(2) EROFS" },
    { TRUNC, "/boot/bin/posixtest", 0, 0, EROFS, "truncate(2) EROFS" },
    { ACCESS, "/boot/bin/posixtest", 0, W_OK, EROFS, "access(2) EROFS: W_OK on read-only" },
    { ACCESS, "/boot/bin/posixtest", 0, R_OK | X_OK, 0, "access(2): a program may run" },
    { CHMOD, "/boot/bin/posixtest", 0, 0600, EROFS, "chmod(2) EROFS" },
};
#undef D

static long call(const struct row *r)
{
    switch (r->op) {
    case OPEN: {
        int fd = open(r->a, (int)r->n, 0644);
        if (fd >= 0)
            close(fd);
        return fd < 0 ? -1 : 0;
    }
    case MKDIR:    return mkdir(r->a, 0755);
    case RMDIR:    return rmdir(r->a);
    case UNLINK:   return unlink(r->a);
    case RENAME:   return rename(r->a, r->b);
    case STAT:     { struct stat st; return stat(r->a, &st); }
    case ACCESS:   return access(r->a, (int)r->n);
    case TRUNC:    return truncate(r->a, r->n);
    case CHDIR:    { long x = chdir(r->a); chdir("/"); return x; }
    case LINK:     return link(r->a, r->b);
    case SYMLINK:  return symlink(r->a, r->b);
    case READLINK: { char buf[64]; return readlink(r->a, buf, sizeof(buf)) < 0 ? -1 : 0; }
    case CHMOD:    return chmod(r->a, (mode_t)r->n);
    }
    return -2;
}

static void errno_table(void)
{
    memset(longname, 'n', sizeof(longname) - 1);
    memcpy(longname, PT_DIR "/", sizeof(PT_DIR));
    memset(longpath, 'p', sizeof(longpath) - 1);
    longpath[0] = '/';
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        errno = 0;
        long got = call(&rows[i]);
        int e = got ? errno : 0;
        if ((rows[i].expect == 0 && got == 0) || (got == -1 && e == rows[i].expect))
            continue;
        printf("posixtest: row %u (%s %s): got %ld, errno %d (%s), want %d (%s)\n", i,
               rows[i].a, rows[i].b ? rows[i].b : "", got, e, strerror(e), rows[i].expect,
               strerror(rows[i].expect));
        pt_fail(__FILE__, __LINE__, rows[i].why);
    }
}

/* Errors of calls on descriptors. */
static void descriptor_errors(void)
{
    char buf[8];
    int ro = open(PT_DIR "/f", O_RDONLY), wo = open(PT_DIR "/f", O_WRONLY);
    int dir = open(PT_DIR "/d", O_RDONLY | O_DIRECTORY);
    CHECK(ro >= 0 && wo >= 0 && dir >= 0);
    errno = 0;
    CHECK(read(wo, buf, 1) == -1 && errno == EBADF);          /* read(2) EBADF */
    CHECK(write(ro, "x", 1) == -1 && errno == EBADF);         /* write(2) EBADF */
    CHECK(read(dir, buf, 1) == -1 && errno == EISDIR);        /* read(2) EISDIR */
    CHECK(close(-1) == -1 && errno == EBADF);                 /* close(2) EBADF */
    CHECK(close(4000) == -1 && errno == EBADF);
    CHECK(pread(ro, buf, 1, -1) == -1 && errno == EINVAL);    /* pread(2) EINVAL */
    CHECK(ftruncate(ro, 0) == -1 && errno == EINVAL);         /* ftruncate(2): not for writing */
    CHECK(ftruncate(wo, -1) == -1 && errno == EINVAL);
    CHECK(lseek(ro, 0, 99) == -1 && errno == EINVAL);         /* lseek(2) EINVAL: whence */
    CHECK(lseek(ro, -1, SEEK_SET) == -1 && errno == EINVAL);  /* lseek(2): a negative offset */
    CHECK(lseek(ro, 100, SEEK_DATA) == -1 && errno == ENXIO); /* lseek(2) ENXIO: past the end */
    CHECK(fchdir(ro) == -1 && errno == ENOTDIR);              /* fchdir(2) ENOTDIR */
    CHECK(openat(ro, "x", O_RDONLY) == -1 && errno == ENOTDIR);   /* openat(2) ENOTDIR */
    CHECK(openat(4000, "x", O_RDONLY) == -1 && errno == EBADF);   /* openat(2) EBADF */
    int abs = openat(4000, PT_DIR "/f", O_RDONLY);            /* openat(2): dirfd unused */
    CHECK(abs >= 0);
    close(abs);
    CHECK(fsync(4000) == -1 && errno == EBADF);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, wo, 0) == MAP_FAILED && errno == EACCES);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, dir, 0) == MAP_FAILED && errno == ENODEV);
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0) == MAP_FAILED && errno == ENODEV);
    close(ro);
    close(wo);
    close(dir);
}

/* Reads, writes, seeks and sizes of one file, across buffer-fulls. */
static void read_write(void)
{
    static unsigned char big[BIG], back[BIG];
    for (int i = 0; i < BIG; i++)
        big[i] = (unsigned char)(i * 7 + (i >> 9));
    int fd = open(PT_DIR "/io", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, big, BIG) == BIG);
    CHECK(lseek(fd, 0, SEEK_CUR) == BIG);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, back, BIG + 10) == BIG && !memcmp(big, back, BIG));
    CHECK(read(fd, back, 10) == 0);   /* the end */
    CHECK(pread(fd, back, 100, BIG - 50) == 50 && !memcmp(back, big + BIG - 50, 50));
    CHECK(lseek(fd, 0, SEEK_CUR) == BIG);   /* pread moved nothing */
    CHECK(pwrite(fd, "XYZ", 3, 10) == 3);
    CHECK(pread(fd, back, 5, 9) == 5 && !memcmp(back + 1, "XYZ", 3));
    struct iovec v[3] = { { "ab", 2 }, { "", 0 }, { "cde", 3 } };
    CHECK(lseek(fd, -5, SEEK_END) == BIG - 5);
    CHECK(writev(fd, v, 3) == 5);
    char got[6] = { 0 };
    struct iovec r[2] = { { got, 1 }, { got + 1, 4 } };
    CHECK(preadv(fd, r, 2, BIG - 5) == 5 && !memcmp(got, "abcde", 5));
    CHECK(lseek(fd, 10, SEEK_DATA) == 10 && lseek(fd, 10, SEEK_HOLE) == BIG);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == BIG && S_ISREG(st.st_mode));
    CHECK((st.st_mode & 0777) == 0644 && st.st_nlink == 1 && st.st_uid == 1000);
    CHECK(st.st_blocks == (BIG + 511) / 512 && st.st_blksize == 65536);
    CHECK(ftruncate(fd, 1000) == 0 && fstat(fd, &st) == 0 && st.st_size == 1000);
    CHECK(ftruncate(fd, 5000) == 0 && pread(fd, back, 10, 4990) == 10);
    CHECK(back[0] == 0 && back[9] == 0);   /* grown with zeros */
    CHECK(fsync(fd) == 0 && fdatasync(fd) == 0);
    CHECK(close(fd) == 0);
    CHECK(stat(PT_DIR "/io", &st) == 0 && st.st_size == 5000);
    CHECK(truncate(PT_DIR "/io", 7) == 0 && stat(PT_DIR "/io", &st) == 0 && st.st_size == 7);
}

/* O_EXCL makes a file once; O_TRUNC empties it. */
static void exclusive(void)
{
    int fd = open(PT_DIR "/excl", O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && write(fd, "1234", 4) == 4);
    close(fd);
    errno = 0;
    CHECK(open(PT_DIR "/excl", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    fd = open(PT_DIR "/excl", O_WRONLY | O_CREAT, 0644);   /* there: opened, not emptied */
    struct stat st;
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 4);
    close(fd);
    fd = open(PT_DIR "/excl", O_RDONLY | O_TRUNC);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 0);
    close(fd);
}

/* Two descriptions of one file with O_APPEND, written in turn: every
 * write lands at the end, in order, and each description's offset is
 * just past its own last write (write(2): O_APPEND moves the offset to
 * the end before each write). */
static void append_two(void)
{
    int a = open(PT_DIR "/app", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    int b = open(PT_DIR "/app", O_WRONLY | O_APPEND);
    CHECK(a >= 0 && b >= 0);
    char want[2000];
    size_t n = 0;
    for (int i = 0; i < 50; i++) {
        char line[16];
        int k = snprintf(line, sizeof(line), "%c%02d\n", i % 2 ? 'b' : 'a', i);
        CHECK(write(i % 2 ? b : a, line, (size_t)k) == k);
        memcpy(want + n, line, (size_t)k);
        n += (size_t)k;
    }
    CHECK(lseek(a, 0, SEEK_CUR) == (off_t)n - 4 && lseek(b, 0, SEEK_CUR) == (off_t)n);
    /* musl's pwrite is pwritev2 with RWF_NOAPPEND (pwritev2(2), Linux 6.9):
     * it writes where it is told, O_APPEND or not, as POSIX says */
    CHECK(pwrite(a, "Z", 1, 0) == 1 && lseek(a, 0, SEEK_CUR) == (off_t)n - 4);
    want[0] = 'Z';
    close(a);
    close(b);
    char got[2100];
    int fd = open(PT_DIR "/app", O_RDONLY);
    CHECK(fd >= 0 && read(fd, got, sizeof(got)) == (ssize_t)n && !memcmp(got, want, n));
    CHECK(fcntl(fd, F_GETFL) == (O_RDONLY | O_LARGEFILE));   /* musl's open adds it, as Linux */
    close(fd);
    /* F_SETFL turning O_APPEND on for a description opened without it */
    fd = open(PT_DIR "/app", O_WRONLY);
    CHECK(fd >= 0 && fcntl(fd, F_SETFL, O_APPEND) == 0 && write(fd, "E", 1) == 1);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t)n + 1);
    close(fd);
}

/* rename over a file and over an empty directory. */
static void rename_over(void)
{
    int fd = open(PT_DIR "/ra", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "aaa", 3) == 3);
    close(fd);
    fd = open(PT_DIR "/rb", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "bbbbbb", 6) == 6);
    close(fd);
    CHECK(rename(PT_DIR "/ra", PT_DIR "/rb") == 0);
    struct stat st;
    errno = 0;
    CHECK(stat(PT_DIR "/ra", &st) == -1 && errno == ENOENT);
    char got[8] = { 0 };
    fd = open(PT_DIR "/rb", O_RDONLY);
    CHECK(fd >= 0 && read(fd, got, sizeof(got)) == 3 && !memcmp(got, "aaa", 3));
    close(fd);
    CHECK(mkdir(PT_DIR "/rd1", 0755) == 0 && mkdir(PT_DIR "/rd2", 0755) == 0);
    CHECK(rename(PT_DIR "/rd1", PT_DIR "/rd2") == 0);   /* over an empty directory */
    CHECK(stat(PT_DIR "/rd2", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(rmdir(PT_DIR "/rd2") == 0 && unlink(PT_DIR "/rb") == 0);
    CHECK(renameat2(AT_FDCWD, PT_DIR "/f", AT_FDCWD, PT_DIR "/excl", RENAME_NOREPLACE) == -1 &&
          errno == EEXIST);   /* rename(2): RENAME_NOREPLACE */
}

/* The fixtures the errno table needs: f (a file), d (empty), full/x. */
static bool fixtures(void)
{
    int fd = open(PT_DIR "/f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, "hello", 5) == 5;
    if (fd >= 0)
        close(fd);
    ok = ok && mkdir(PT_DIR "/d", 0755) == 0 && mkdir(PT_DIR "/full", 0755) == 0;
    fd = ok ? open(PT_DIR "/full/x", O_WRONLY | O_CREAT, 0644) : -1;
    if (fd >= 0)
        close(fd);
    return ok && fd >= 0;
}

void pt_files(char **argv)
{
    (void)argv;
    if (!pt_files_start()) {
        CHECK(!"no /data to test on");
        return;
    }
    CHECK(fixtures());
    errno_table();
    descriptor_errors();
    read_write();
    exclusive();
    append_two();
    rename_over();
    pt_files2();
    pt_files_end();
}
