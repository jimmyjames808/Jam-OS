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

/* A C string's length, as the protocols' str takes it. */
static uint32_t len_of(const char *s)
{
    return (uint32_t)strlen(s);
}

/* pager: a copy handed to fat (FAT_SR_PAGER, as volumes hands it), or 0;
 * extra: test words (fat/test.c) after the format flag (n of them). */
static bool start(struct fatrun *r, struct ramdisk *rd, bool read_only, bool format,
                  handle_t pager, const char *const *extra, unsigned n)
{
    handle_t block, serve, ctl;
    *r = (struct fatrun){ .rd = rd };
    CHECK(n <= 4 && (format || !n));
    if (!ramdisk_serve(rd, read_only, &block))
        return false;
    CHECK_ST(jam_channel_create(&r->fs, &serve), OK);
    CHECK_ST(jam_channel_create(&r->ctl, &ctl), OK);
    CHECK_ST(new_job(&r->job), OK);
    const char *argv[7] = { "fat", "utest", FAT_ARG_FORMAT };
    for (unsigned i = 0; i < n; i++)
        argv[3 + i] = extra[i];
    struct spawn_handle x[4] = { { FAT_SR_BLOCK, block }, { FAT_SR_SERVE, serve },
                                 { FAT_SR_CTL, ctl } };
    rights_t xr[4] = { RIGHT_SAME, RIGHT_SAME, RIGHT_SAME, RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT };
    unsigned nx = 3;
    if (pager) {
        CHECK_ST(jam_handle_duplicate(pager, RIGHT_SAME, &x[nx].h), OK);
        x[nx++].role = FAT_SR_PAGER;
    }
    struct spawn_args a = {
        .path = "bin/fat", .argc = format ? 3 + n : 2, .argv = argv, .job = r->job, .extra = x,
        .nextra = nx, .extra_rights = xr,
    };
    CHECK_ST(spawn(&a, &r->proc), OK);
    return true;
}

bool fat_start(struct fatrun *r, struct ramdisk *rd, bool read_only)
{
    return start(r, rd, read_only, true, HANDLE_INVALID, NULL, 0);
}

bool fat_start_plain(struct fatrun *r, struct ramdisk *rd, bool read_only)
{
    return start(r, rd, read_only, false, HANDLE_INVALID, NULL, 0);
}

bool fat_start_pager(struct fatrun *r, struct ramdisk *rd, handle_t pager)
{
    return start(r, rd, false, true, pager, NULL, 0);
}

bool fat_start_words(struct fatrun *r, struct ramdisk *rd, const char *const *words, unsigned n)
{
    return start(r, rd, false, true, HANDLE_INVALID, words, n);
}

bool fat_wait(struct fatrun *r, int code)
{
    struct process_info info;
    CHECK_ST(spawn_wait(r->proc, FAT_CALL_NS, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, code);
    CHECK_ST(jam_handle_close(r->proc), OK);
    if (r->ctl)   /* none: a service someone else started (devmgr's) */
        CHECK_ST(jam_handle_close(r->ctl), OK);
    struct job_info ji;
    CHECK_ST(info_of(r->job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k] > (k == JOB_LIMIT_HANDLES ? r->kept : 0))
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
    handle_t vmo = HANDLE_INVALID;
    uint64_t addr = 0;
    status_t st = fs_open_until(r->fs, deadline(), path, len_of(path), flags, FS_MODE_FILE_DEFAULT,
                                &f->ch, &vmo,
                                &f->size);
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
    uint8_t dir = 0;
    status_t st = fs_stat_until(r->fs, deadline(), path, len_of(path), size, &dir, mtime, NULL);
    if (st == OK && is_dir)
        *is_dir = dir != 0;
    return st;
}

status_t t_stat_id(const struct fatrun *r, const char *path, uint64_t *id)
{
    return fs_stat_until(r->fs, deadline(), path, len_of(path), NULL, NULL, NULL, id);
}

status_t t_batch(const struct fatrun *r, const char *path, uint32_t cookie, struct tbatch *b)
{
    return fs_readdir_until(r->fs, deadline(), path, len_of(path), cookie, &b->next, &b->count,
                            b->entries, &b->len);
}

/* Record k of b (count of them) into *e: false if the batch is malformed. */
static bool batch_at(const struct tbatch *b, uint32_t k, struct fs_dirent *e)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i <= k; i++)
        if (!fs_dirent_get(b->entries, b->len, &at, e))
            return false;
    return true;
}

/* The first entry at cookie, as name (FS_NAME_MAX bytes). */
static status_t first_at(const struct tbatch *b, char *name, bool *is_dir, uint64_t *id)
{
    struct fs_dirent e;
    if (!b->count)
        return ERR_NOT_FOUND;
    if (!batch_at(b, 0, &e))
        return ERR_INTERNAL;
    memcpy(name, e.name, e.name_len);
    name[e.name_len] = '\0';
    if (is_dir)
        *is_dir = e.is_dir;
    if (id)
        *id = e.id;
    return OK;
}

status_t t_readdir_at(const struct fatrun *r, const char *path, uint32_t cookie, char *name,
                      bool *is_dir)
{
    static struct tbatch b;
    status_t st = t_batch(r, path, cookie, &b);
    return st == OK ? first_at(&b, name, is_dir, NULL) : st;
}

/* The batch t_readdir last fetched: of `path`, entries first .. first +
 * count - 1. */
static struct {
    char         path[FS_PATH_MAX];
    uint32_t     first;
    struct tbatch b;
} cache;

status_t t_readdir(const struct fatrun *r, const char *path, uint32_t index, char *name,
                   bool *is_dir)
{
    status_t st = OK;
    if (!index || index < cache.first || strcmp(cache.path, path)) {
        snprintf(cache.path, sizeof(cache.path), "%s", path);
        cache.first = 0;
        st = t_batch(r, path, 0, &cache.b);
    }
    while (st == OK && index >= cache.first + cache.b.count) {
        if (!cache.b.next)
            st = ERR_NOT_FOUND;
        else if (!cache.b.count)
            st = ERR_INTERNAL;   /* an empty batch that isn't the last */
        else {
            cache.first += cache.b.count;
            st = t_batch(r, path, cache.b.next, &cache.b);
        }
    }
    if (st != OK) {
        if (st != ERR_NOT_FOUND)
            cache.path[0] = '\0';
        return st;
    }
    struct fs_dirent e;
    if (!batch_at(&cache.b, index - cache.first, &e))
        return ERR_INTERNAL;
    memcpy(name, e.name, e.name_len);
    name[e.name_len] = '\0';
    if (is_dir)
        *is_dir = e.is_dir;
    return OK;
}

status_t t_mkdir(const struct fatrun *r, const char *path)
{
    return fs_mkdir_until(r->fs, deadline(), path, len_of(path), FS_MODE_DIR_DEFAULT);
}

status_t t_unlink(const struct fatrun *r, const char *path)
{
    bool dir = false;
    status_t st = t_stat(r, path, NULL, &dir, NULL);
    if (st == ERR_INVALID_ARGS)
        return st;
    return t_unlink_flags(r, path, st == OK && dir ? FS_UNLINK_DIR : 0);
}

status_t t_unlink_flags(const struct fatrun *r, const char *path, uint32_t flags)
{
    return fs_unlink_until(r->fs, deadline(), path, len_of(path), flags);
}

status_t t_rename(const struct fatrun *r, const char *from, const char *to)
{
    return t_rename_flags(r, from, to, 0);
}

status_t t_rename_flags(const struct fatrun *r, const char *from, const char *to,
                        uint32_t flags)
{
    return fs_rename_until(r->fs, deadline(), from, len_of(from), to, len_of(to), flags);
}

status_t t_sync(const struct fatrun *r)
{
    return fs_sync_until(r->fs, deadline());
}

status_t t_free(const struct fatrun *r, uint64_t *total, uint64_t *free_bytes)
{
    return fs_statfs_until(r->fs, deadline(), total, free_bytes, NULL, NULL, NULL, NULL, NULL);
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
    char got[FS_NAME_MAX];
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
