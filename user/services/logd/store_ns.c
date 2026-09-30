/* logd: /data through the file namespace, with <os.h>'s file calls (see
 * logd.h). */
#include <os.h>
#include "logd.h"

static struct jfile file;   /* the log, while open */
static bool         is_open;

static status_t ns_mkdir(const char *path)
{
    return fs_mkdir(path);
}

static status_t ns_stat(const char *path)
{
    return fs_stat(path, NULL, NULL, NULL);
}

static status_t ns_open(const char *path, uint32_t flags, uint64_t *size)
{
    status_t st = file_open(path, flags, &file);
    if (st != OK)
        return st;
    is_open = true;
    *size = file.size;
    return OK;
}

static status_t ns_write(uint64_t offset, const void *data, uint32_t n)
{
    size_t done = 0;
    status_t st = file_write(&file, offset, data, n, &done);
    return st == OK && done != n ? ERR_IO : st;
}

static status_t ns_sync(void)
{
    return file_sync(&file);
}

static void ns_close(void)
{
    if (is_open)
        file_close(&file);
    is_open = false;
}

const struct store store_ns = {
    .root = "/data", .mkdir = ns_mkdir, .stat = ns_stat, .open = ns_open, .write = ns_write,
    .sync = ns_sync, .close = ns_close,
};
