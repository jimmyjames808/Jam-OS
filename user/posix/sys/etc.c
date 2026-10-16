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
 * There is no localtime: TZ is always set (the start adds it). The files
 * are fixed text, read only (opening one to write, or creating one, is
 * EROFS); a description's priv is the file's entry in `files`, its offset
 * the description's (under its lock, fd.h). /etc stats as a directory;
 * listing it is not built. */
#include <errno.h>
#include <fcntl.h>
#include "kinds.h"

#define ETC_DIR "/etc/"
#define SYS_DIR "/sys/"

struct etc_file {
    const char *path;   /* absolute, under /etc or /sys */
    const char *text;
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

bool etc_path(const char *abs)
{
    if (!strncmp(abs, SYS_DIR, sizeof(SYS_DIR) - 1))
        return file_at(abs) && !mounted("/sys");
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
    st->st_size = (off_t)strlen(f->text);
    st->st_ino = (ino_t)(f - files) + 1;
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

const struct fd_ops etc_ops = {
    .name = "/etc file",
    .read = etc_read,
    .seek = etc_seek,
    .stat = etc_fstat,
    /* .ready, .arm: none (always ready to read) */
    .give = etc_give,
};

/* The file at abs for an open with flags: 0 and *out, or -errno. */
static long etc_lookup(const char *abs, int flags, const struct etc_file **out)
{
    const struct etc_file *f = file_at(abs);
    if (!f && !strcmp(abs, "/etc"))
        return (flags & FD_ACCMODE) != O_RDONLY ? -EISDIR : -ENOTSUP;   /* no listing */
    if (!f)
        return (flags & O_CREAT) ? -EROFS : -ENOENT;
    if ((flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;
    if ((flags & FD_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        return -EROFS;
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
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
    return d ? fd_install(d, 0, flags & O_CLOEXEC) : -ENOMEM;
}

long etc_adopt(const struct posixfds_fd *rec, const void *data, const handle_t *hs,
               struct fd_desc **out)
{
    const struct etc_file *f;
    long r = etc_lookup(data, (int)(rec->status & FD_ACCMODE), &f);   /* a string: checked */
    if (r)
        return r;
    *out = fd_new(&etc_ops, rec->status, (void *)f);
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
    if (!f)
        return -ENOENT;
    stat_of(f, st);
    return 0;
}
