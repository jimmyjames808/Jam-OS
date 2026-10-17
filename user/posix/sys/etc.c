/* /etc (docs/M13-PLAN.md sections 3 and 11): libc's own when the
 * namespace has no /etc mount, so musl's getpwnam, getgrgid,
 * getservbyname and the host lookup find what they read on Linux.
 *
 *   passwd       the one user: jam, uid and gid 1000, home /data, the
 *                Jam OS shell
 *   group        the user's group, jam (1000)
 *   hosts        localhost, and this machine's name (uname's nodename)
 *   services     ssh, domain, http and ntp
 *   resolv.conf  empty: names are resolved through /svc/dns
 *   shells       the Jam OS shell
 *
 * and two files of Linux's /sys that base's SysInfo::GetHardwareInfo reads
 * (the machine's maker and model, which Jam OS can't tell: /sys is no
 * mount here), served the same way if the namespace has no /sys mount:
 *
 *   /sys/devices/virtual/dmi/id/sys_vendor, product_name
 *
 * and the three files base's GetFileDriveInfo reads for a file's drive,
 * one set for each mount (st_dev is the mount's place in the namespace
 * counting from 1, so its device number is 0:<place>; path.c mount_dev),
 * likewise only if there is no /sys mount:
 *
 *   /sys/dev/block/0:<n>/removable          "0": the layer is not told a drive's kind
 *   /sys/dev/block/0:<n>/queue/rotational   "0": ... and every one is flash here
 *   /sys/dev/block/0:<n>/size               the volume's 512-byte sectors (fs.statfs's total)
 *
 * There is no localtime: TZ is always set (the start adds it). The files
 * are fixed text (the size's is made at the open), read only (opening one
 * to write, or creating one, is EROFS); a description's priv is the file's
 * entry in `files`, or a copy made for it (`dynamic`: freed at its close),
 * its offset the description's (under its lock, fd.h). /etc stats as a
 * directory; listing it is not built. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_idl.h>
#include <string.h>
#include "files.h"

#define ETC_DIR  "/etc/"
#define SYS_DIR   "/sys/"
#define BLOCK_DIR "/sys/dev/block/"
#define SECTOR    512u    /* sysfs's size is counted in these, whatever the device's */
#define SYSFS_ATTR_SIZE 4096   /* st_size of a sysfs attribute (its text is shorter) */

struct etc_file {
    const char *path;   /* absolute, under /etc or /sys */
    const char *text;
};

/* An etc_file made for one open: a block device's file (BLOCK_DIR). */
struct dynamic {
    struct etc_file f;   /* first: free() takes &f */
    uint64_t        ino;
    char            path[64];
    char            text[24];
};

static const struct etc_file files[] = {
    { "/etc/passwd", "jam:x:1000:1000:jam:/data:/boot/bin/shell\n" },
    { "/etc/group", "jam:x:1000:jam\n" },
    { "/etc/hosts", "127.0.0.1\tlocalhost\n127.0.1.1\tjamos\n" },
    { "/etc/services", "ssh\t\t22/tcp\n"
                       "domain\t\t53/tcp\n"
                       "domain\t\t53/udp\n"
                       "http\t\t80/tcp\n"
                       "ntp\t\t123/udp\n" },
    { "/etc/resolv.conf", "" },
    { "/etc/shells", "/boot/bin/shell\n" },
    { "/sys/devices/virtual/dmi/id/sys_vendor", "JamOS\n" },
    { "/sys/devices/virtual/dmi/id/product_name", "JamOS machine\n" },
};

static const struct etc_file *file_of(struct fd_desc *d)
{
    return d->priv;
}

/* Is dir a mount of the namespace (then it is the mount's, not ours)? */
static bool mounted(const char *dir)
{
    char name[NS_NAME_MAX];
    for (unsigned i = 0; ns_mount_at(i, name); i++)
        if (!strcmp(name, dir))
            return true;
    return false;
}

static const struct etc_file *file_at(const char *abs)
{
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (!strcmp(abs, files[i].path))
            return &files[i];
    return NULL;
}

/* ---- a mount's block device (BLOCK_DIR) ------------------------------------- */

enum block_leaf { BL_REMOVABLE, BL_ROTATIONAL, BL_SIZE };

/* Is abs BLOCK_DIR "<major>:<minor>/<leaf>" for the device of a mount of
 * ours? Its place in the namespace (counting from 1) and the leaf. The
 * device number is the old encoding <linux/kdev_t.h> has and base reads
 * st_dev with: the major above the low 8 bits. */
static bool block_parse(const char *abs, unsigned *place, enum block_leaf *leaf)
{
    if (strncmp(abs, BLOCK_DIR, sizeof(BLOCK_DIR) - 1))
        return false;
    const char *s = abs + sizeof(BLOCK_DIR) - 1;
    unsigned long dev[2];
    for (int i = 0; i < 2; i++) {
        char *end;
        if (*s < '0' || *s > '9' || (*s == '0' && s[1] >= '0' && s[1] <= '9'))
            return false;
        dev[i] = strtoul(s, &end, 10);
        if (end - s > 4 || *end != (i ? '/' : ':'))
            return false;
        s = end + 1;
    }
    unsigned long n = dev[0] << 8 | dev[1];
    char m[NS_NAME_MAX];
    if (dev[1] > 255 || !n || n > NS_MAX_MOUNTS || !ns_mount_at((unsigned)n - 1, m))
        return false;
    if (!strcmp(s, "removable"))
        *leaf = BL_REMOVABLE;
    else if (!strcmp(s, "queue/rotational"))
        *leaf = BL_ROTATIONAL;
    else if (!strcmp(s, "size"))
        *leaf = BL_SIZE;
    else
        return false;
    *place = (unsigned)n;
    return true;
}

/* A block file's text into out: false if the mount can't tell. */
static bool block_text(unsigned place, enum block_leaf leaf, char out[24])
{
    if (leaf != BL_SIZE) {
        memcpy(out, "0\n", 3);   /* the layer isn't told what a drive is */
        return true;
    }
    char m[NS_NAME_MAX];
    handle_t fs;
    if (!ns_mount_at(place - 1, m) || ns_channel(m, &fs) != OK)
        return false;
    uint64_t total, free_bytes;
    uint8_t ro, label[16];
    uint32_t magic, name_max, features;
    status_t st = fs_statfs_within(fs, FILE_TIMEOUT, &total, &free_bytes, &ro, label, &magic,
                                   &name_max, &features);
    jam_handle_close(fs);
    if (st != OK)
        return false;
    snprintf(out, 24, "%llu\n", (unsigned long long)(total / SECTOR));
    return true;
}

static uint64_t block_ino(unsigned place, enum block_leaf leaf)
{
    return 0x10000 + (uint64_t)place * 4 + leaf;
}

/* Is f one of `files` (and not made for an open)? */
static bool is_static(const struct etc_file *f)
{
    return (uintptr_t)f >= (uintptr_t)files && (uintptr_t)f < (uintptr_t)(files + sizeof(files) / sizeof(files[0]));
}

bool etc_path(const char *abs)
{
    unsigned place;
    enum block_leaf leaf;
    if (!strncmp(abs, SYS_DIR, sizeof(SYS_DIR) - 1))
        return (file_at(abs) || block_parse(abs, &place, &leaf)) && !mounted("/sys");
    return (!strcmp(abs, "/etc") || !strncmp(abs, ETC_DIR, sizeof(ETC_DIR) - 1)) &&
           !mounted("/etc");
}

static long etc_read(struct fd_desc *d, const struct iovec *iov, int iovcnt, int64_t pos)
{
    long n = fd_iov_total(iov, iovcnt);
    if (n <= 0)
        return n;
    const char *text = file_of(d)->text;
    size_t len = strlen(text);
    lock_take(&d->lock);
    uint64_t off = pos == FD_AT_OFFSET ? (uint64_t)d->offset : (uint64_t)pos;
    size_t k = off >= len ? 0 : len - off < (size_t)n ? len - (size_t)off : (size_t)n;
    if (fd_iov_scatter(iov, iovcnt, 0, text + (k ? off : 0), k)) {
        lock_give(&d->lock);
        return -EFAULT;
    }
    if (pos == FD_AT_OFFSET)
        d->offset += (int64_t)k;
    lock_give(&d->lock);
    return (long)k;
}

static long etc_seek(struct fd_desc *d, int64_t off, int whence)
{
    lock_take(&d->lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? d->offset :
                   whence == SEEK_END ? (int64_t)strlen(file_of(d)->text) : -1;
    int64_t to;
    long r = base < 0 || __builtin_add_overflow(base, off, &to) || to < 0 ? -EINVAL : 0;
    if (!r)
        d->offset = to;
    lock_give(&d->lock);
    return r ? r : (long)to;
}

static void stat_of(const struct etc_file *f, struct stat *st)
{
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    if (is_static(f)) {
        st->st_size = (off_t)strlen(f->text);
        st->st_ino = (ino_t)(f - files) + 1;
    } else {
        st->st_size = SYSFS_ATTR_SIZE;
        st->st_ino = (ino_t)((const struct dynamic *)f)->ino;
    }
    st->st_blksize = 4096;
}

static long etc_fstat(struct fd_desc *d, struct stat *st)
{
    stat_of(file_of(d), st);
    return 0;
}

/* A child gets the path; its read starts at the beginning (only a file
 * or a directory has an offset in the hand-over). */
static long etc_give(struct fd_desc *d, struct fd_given *out)
{
    const char *path = file_of(d)->path;
    size_t n = strlen(path);
    long r = 0;
    void *data = kind_data(path, n + 1, &r);
    if (r)
        return r;
    out->kind = POSIXFDS_ETC;
    out->status = fd_status(d);
    out->offset = 0;
    out->nhandles = 0;
    out->data = data;
    out->data_len = (uint32_t)n + 1;
    return 0;
}

/* A copy made for an open (block_file) goes with its description. */
static void etc_close(struct fd_desc *d)
{
    const struct etc_file *f = file_of(d);
    if (!is_static(f))
        free((void *)f);
}

const struct fd_ops etc_ops = {
    .name = "/etc file",
    .read = etc_read,
    .seek = etc_seek,
    .stat = etc_fstat,
    /* .ready, .arm: none (always ready to read) */
    .give = etc_give,
    .close = etc_close,
};

/* The copy of a block device's file for one open, its text made now: NULL
 * and *err if the mount can't say. */
static const struct etc_file *block_file(const char *abs, unsigned place, enum block_leaf leaf,
                                         long *err)
{
    struct dynamic *c = calloc(1, sizeof(*c));
    if (!c) {
        *err = -ENOMEM;
        return NULL;
    }
    if (!block_text(place, leaf, c->text)) {
        free(c);
        *err = -EIO;
        return NULL;
    }
    snprintf(c->path, sizeof(c->path), "%s", abs);
    c->f = (struct etc_file){ .path = c->path, .text = c->text };
    c->ino = block_ino(place, leaf);
    return &c->f;
}

/* The file at abs for an open with flags: 0 and *out (a copy to free with
 * its description unless it is one of `files`), or -errno. */
static long etc_lookup(const char *abs, int flags, const struct etc_file **out)
{
    unsigned place = 0;
    enum block_leaf leaf = BL_SIZE;
    const struct etc_file *f = file_at(abs);
    bool block = !f && block_parse(abs, &place, &leaf);
    if (!f && !block && !strcmp(abs, "/etc"))
        return (flags & FD_ACCMODE) != O_RDONLY ? -EISDIR : -ENOTSUP;   /* no listing */
    if (!f && !block)
        return (flags & O_CREAT) ? -EROFS : -ENOENT;
    if ((flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;
    if ((flags & FD_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return -EROFS;
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    long err = 0;
    if (block && !(f = block_file(abs, place, leaf, &err)))
        return err;
    *out = f;
    return 0;
}

/* files[] is never written through a description (etc_ops only reads
 * its priv). */
long etc_open(const char *abs, int flags)
{
    const struct etc_file *f;
    long r = etc_lookup(abs, flags, &f);
    if (r)
        return r;
    struct fd_desc *d = fd_new(&etc_ops, (uint32_t)flags & OPEN_STATUS, (void *)f);
    if (!d) {
        if (!is_static(f))
            free((void *)f);
        return -ENOMEM;
    }
    return fd_install(d, 0, flags & O_CLOEXEC);
}

long etc_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
               struct fd_desc **out)
{
    const struct etc_file *f;
    long r = etc_lookup(data, (int)(rec->status & FD_ACCMODE), &f);   /* a string: checked */
    if (r)
        return r;
    *out = fd_new(&etc_ops, rec->status, (void *)f);
    if (!*out && !is_static(f))
        free((void *)f);
    return *out ? 0 : -ENOMEM;
}

long etc_stat(const char *abs, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (!strcmp(abs, "/etc")) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_blksize = 4096;
        return 0;
    }
    const struct etc_file *f = file_at(abs);
    unsigned place;
    enum block_leaf leaf;
    if (!f && block_parse(abs, &place, &leaf)) {   /* no text needed: sysfs says 4096 */
        const struct dynamic b = { .f = { .path = abs, .text = "" }, .ino = block_ino(place, leaf) };
        stat_of(&b.f, st);
        return 0;
    }
    if (!f)
        return -ENOENT;
    stat_of(f, st);
    return 0;
}
