/* logd: /data as one `fs` channel, called directly (see logd.h). Every
 * call has a deadline, so a filesystem that stops answering makes logd
 * treat /data as gone instead of hanging with it. */
#include <fs_idl.h>
#include <os.h>
#include "logd.h"

#define CALL_WAIT (10 * NS_PER_S)

static handle_t fs;             /* the filesystem */
static handle_t file, buffer;   /* the log while open: its channel and transfer buffer */

static uint64_t deadline(void)
{
    return now() + CALL_WAIT;
}

/* A path as the protocol carries it: FS_PATH_MAX bytes, NUL-padded. */
static void wire_path(uint8_t out[FS_PATH_MAX], const char *path)
{
    memset(out, 0, FS_PATH_MAX);
    memcpy(out, path, strnlen(path, FS_PATH_MAX - 1));
}

static status_t direct_mkdir(const char *path)
{
    uint8_t p[FS_PATH_MAX];
    wire_path(p, path);
    return fs_mkdir_until(fs, deadline(), p);
}

static status_t direct_stat(const char *path)
{
    uint8_t p[FS_PATH_MAX];
    wire_path(p, path);
    return fs_stat_until(fs, deadline(), p, NULL, NULL, NULL);
}

static status_t direct_open(const char *path, uint32_t flags, uint64_t *size)
{
    uint8_t p[FS_PATH_MAX];
    wire_path(p, path);
    return fs_open_until(fs, deadline(), p, flags, &file, &buffer, size);
}

static status_t direct_write(uint64_t offset, const void *data, uint32_t n)
{
    uint32_t actual = 0;
    status_t st = jam_vmo_write(buffer, 0, data, n);
    if (st == OK)
        st = file_write_until(file, deadline(), offset, n, &actual);
    return st == OK && actual != n ? ERR_IO : st;
}

static status_t direct_sync(void)
{
    return file_sync_until(file, deadline());
}

static void direct_close(void)
{
    if (file)
        jam_handle_close(file);
    if (buffer)
        jam_handle_close(buffer);
    file = buffer = HANDLE_INVALID;
}

static const struct store store_direct = {
    .root = "", .mkdir = direct_mkdir, .stat = direct_stat, .open = direct_open,
    .write = direct_write, .sync = direct_sync, .close = direct_close,
};

const struct store *store_fs(handle_t ch)
{
    fs = ch;
    return &store_direct;
}
