/* The file calls of <os.h> (the M8 namespace). A placeholder until the
 * namespace track fills it in: every call fails ERR_NOT_SUPPORTED, so
 * callers can be written and built against the final interface now. */
#include <os.h>

status_t file_open(const char *path, uint32_t flags, struct jfile *out)
{
    (void)path;
    (void)flags;
    (void)out;
    return ERR_NOT_SUPPORTED;
}

status_t file_read(struct jfile *f, uint64_t offset, void *dst, size_t n, size_t *done)
{
    (void)f;
    (void)offset;
    (void)dst;
    (void)n;
    (void)done;
    return ERR_NOT_SUPPORTED;
}

status_t file_write(struct jfile *f, uint64_t offset, const void *src, size_t n, size_t *done)
{
    (void)f;
    (void)offset;
    (void)src;
    (void)n;
    (void)done;
    return ERR_NOT_SUPPORTED;
}

status_t file_sync(struct jfile *f)
{
    (void)f;
    return ERR_NOT_SUPPORTED;
}

void file_close(struct jfile *f)
{
    (void)f;
}

status_t fs_stat(const char *path, uint64_t *size, bool *is_dir, uint64_t *mtime)
{
    (void)path;
    (void)size;
    (void)is_dir;
    (void)mtime;
    return ERR_NOT_SUPPORTED;
}

status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out)
{
    (void)path;
    (void)index;
    (void)out;
    return ERR_NOT_SUPPORTED;
}

status_t fs_mkdir(const char *path)
{
    (void)path;
    return ERR_NOT_SUPPORTED;
}

status_t fs_unlink(const char *path)
{
    (void)path;
    return ERR_NOT_SUPPORTED;
}

status_t fs_rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return ERR_NOT_SUPPORTED;
}

status_t fs_sync(const char *path)
{
    (void)path;
    return ERR_NOT_SUPPORTED;
}
