/* utest: the fat tests' helpers (fattest.h): starting and stopping a fat
 * process over a RAM disk, and the client calls. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fatsvc.h>
#include <fs_idl.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

static uint64_t deadline(void)
{
    return now() + FAT_CALL_NS;
}

/* A C string as the protocols' 256-byte path field (cut if too long). */
static void path_field(const char *s, uint8_t out[FS_PATH_MAX])
{
    memset(out, 0, FS_PATH_MAX);
    memcpy(out, s, strnlen(s, FS_PATH_MAX - 1));
}

static bool start(struct fatrun *r, struct ramdisk *rd, bool read_only, bool format)
{
    handle_t block, serve;
    *r = (struct fatrun){ .rd = rd };
    if (!ramdisk_serve(rd, read_only, &block))
        return false;
    CHECK_ST(jam_channel_create(&r->fs, &serve), OK);
    CHECK_ST(new_job(&r->job), OK);
    const char *argv[] = { "fat", "utest", FAT_ARG_FORMAT };
    struct spawn_handle x[2] = { { FAT_SR_BLOCK, block }, { FAT_SR_SERVE, serve } };
    struct spawn_args a = {
        .path = "bin/fat", .argc = format ? 3 : 2, .argv = argv, .job = r->job, .extra = x,
        .nextra = 2,
    };
    CHECK_ST(spawn(&a, &r->proc), OK);
    return true;
}

bool fat_start(struct fatrun *r, struct ramdisk *rd, bool read_only)
{
    return start(r, rd, read_only, true);
}

bool fat_start_plain(struct fatrun *r, struct ramdisk *rd, bool read_only)
{
    return start(r, rd, read_only, false);
}

bool fat_wait(struct fatrun *r, int code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(r->proc, FAT_CALL_NS, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, code);
    CHECK_ST(jam_handle_close(r->proc), OK);
    struct job_info ji;
    CHECK_ST(info_of(r->job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("fat's job still has %lu units of kind %u", (unsigned long)ji.used[k], k);
    CHECK_ST(jam_handle_close(r->job), OK);
    return true;
}

bool fat_stop(struct fatrun *r)
{
    CHECK_ST(jam_handle_close(r->fs), OK);
    return fat_wait(r, 0) && ramdisk_join(r->rd);
}

status_t t_open(const struct fatrun *r, const char *path, uint32_t flags, struct tfile *f)
{
    uint8_t p[FS_PATH_MAX];
    handle_t vmo = HANDLE_INVALID;
    uint64_t addr = 0;
    path_field(path, p);
    status_t st = fs_open_until(r->fs, deadline(), p, flags, &f->ch, &vmo, &f->size);
    if (st != OK)
        return st;
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, FAT_BUF, VMAR_READ | VMAR_WRITE,
                      &addr);
    jam_handle_close(vmo);   /* the mapping keeps the buffer */
    if (st != OK) {
        jam_handle_close(f->ch);
        return st;
    }
    f->buf = (uint8_t *)(uintptr_t)addr;
    return OK;
}

void t_close(struct tfile *f)
{
    /* Our own mapping: the unmap can't fail. */
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)f->buf, FAT_BUF);
    jam_handle_close(f->ch);
}

status_t t_read(struct tfile *f, uint64_t off, void *dst, uint32_t n, uint32_t *done)
{
    uint32_t got = 0;
    status_t st = file_read_until(f->ch, deadline(), off, n, &got);
    if (st != OK)
        return st;
    memcpy(dst, f->buf, got <= n ? got : n);
    *done = got;
    return OK;
}

status_t t_write(struct tfile *f, uint64_t off, const void *src, uint32_t n, uint32_t *done)
{
    memcpy(f->buf, src, n <= FAT_BUF ? n : FAT_BUF);
    return file_write_until(f->ch, deadline(), off, n, done);
}

status_t t_stat(const struct fatrun *r, const char *path, uint64_t *size, bool *is_dir,
                uint64_t *mtime)
{
    uint8_t p[FS_PATH_MAX], dir = 0;
    path_field(path, p);
    status_t st = fs_stat_until(r->fs, deadline(), p, size, &dir, mtime);
    if (st == OK && is_dir)
        *is_dir = dir != 0;
    return st;
}

status_t t_readdir(const struct fatrun *r, const char *path, uint32_t index, char *name,
                   bool *is_dir)
{
    uint8_t p[FS_PATH_MAX], dir = 0;
    path_field(path, p);
    status_t st = fs_readdir_until(r->fs, deadline(), p, index, (uint8_t *)name, &dir, NULL);
    if (st == OK && is_dir)
        *is_dir = dir != 0;
    return st;
}

status_t t_mkdir(const struct fatrun *r, const char *path)
{
    uint8_t p[FS_PATH_MAX];
    path_field(path, p);
    return fs_mkdir_until(r->fs, deadline(), p);
}

status_t t_unlink(const struct fatrun *r, const char *path)
{
    uint8_t p[FS_PATH_MAX];
    path_field(path, p);
    return fs_unlink_until(r->fs, deadline(), p);
}

status_t t_rename(const struct fatrun *r, const char *from, const char *to)
{
    uint8_t pf[FS_PATH_MAX], pt[FS_PATH_MAX];
    path_field(from, pf);
    path_field(to, pt);
    return fs_rename_until(r->fs, deadline(), pf, pt);
}

status_t t_sync(const struct fatrun *r)
{
    return fs_sync_until(r->fs, deadline());
}

status_t t_free(const struct fatrun *r, uint64_t *total, uint64_t *free_bytes)
{
    return fs_statfs_until(r->fs, deadline(), total, free_bytes, NULL, NULL);
}

bool put_file(const struct fatrun *r, const char *path, const char *text)
{
    struct tfile f;
    uint32_t n = (uint32_t)strlen(text), done = 0;
    CHECK_ST(t_open(r, path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f), OK);
    status_t st = t_write(&f, 0, text, n, &done);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(done, n);
    return true;
}

bool file_is(const struct fatrun *r, const char *path, const char *text)
{
    struct tfile f;
    static char got[4096];
    uint32_t n = (uint32_t)strlen(text), done = 0;
    CHECK(n < sizeof(got));
    CHECK_ST(t_open(r, path, FS_READ, &f), OK);
    uint64_t size = f.size;
    status_t st = t_read(&f, 0, got, sizeof(got), &done);
    t_close(&f);
    CHECK_ST(st, OK);
    if (size != n || done != n || memcmp(got, text, n))
        FAIL("%s holds %u bytes (size %lu), not \"%s\"", path, done, (unsigned long)size, text);
    return true;
}

bool dir_count(const struct fatrun *r, const char *path, const char *name, unsigned *count,
               bool *found)
{
    char got[FS_PATH_MAX];
    unsigned n = 0;
    status_t st;
    if (found)
        *found = false;
    while ((st = t_readdir(r, path, n, got, NULL)) == OK && n < 4096) {
        if (found && name && !strcmp(got, name))
            *found = true;
        n++;
    }
    CHECK_ST(st, ERR_NOT_FOUND);
    *count = n;
    return true;
}
