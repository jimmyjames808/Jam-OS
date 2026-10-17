/* utest: the extfs service (bin/extfs, user/services/extfs; docs/M13.6-PLAN.md's
 * U4) over a RAM disk it formats, through the protocols themselves: files,
 * directories and readdir's types; symbolic and hard links, ERR_LINK_OUT
 * and fs.resolve; the permission bits; many writers, FS_APPEND, unlink and
 * truncate of open files; views judged where paths resolve (a link into
 * etc refused as etc is) and subviews; a damaged block turning the volume
 * read-only with the error recorded on it. Each disk is saved for Linux's
 * e2fsck when tools/extfs-svc-test.sh asks (extfs_save). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <idl/fsctl.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define SAVE_DIR "/data/extfs-e2fsck"

/* Each test has a disk of its own (a test that fails leaves its extfs and
 * its disk's session behind, which the next test must not trip over). */
static uint8_t big[64u << 10], back[64u << 10];

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static uint32_t L(const char *s)
{
    return (uint32_t)strlen(s);
}

/* pager: a copy handed to extfs (EXTFS_SR_PAGER, as volumes hands it), or 0. */
static bool start(struct fatrun *r, struct ramdisk *rd, bool read_only, const char *const *extra,
                  unsigned n, handle_t pager)
{
    handle_t block, serve, ctl;
    *r = (struct fatrun){ .rd = rd };
    if (!ramdisk_serve(rd, read_only, &block))
        return false;
    CHECK_ST(jam_channel_create(&r->fs, &serve), OK);
    CHECK_ST(jam_channel_create(&r->ctl, &ctl), OK);
    CHECK_ST(new_job(&r->job), OK);
    const char *argv[12] = { "extfs", "utest", EXTFS_ARG_FORMAT };
    int argc = 3;
    for (unsigned i = 0; i < n && argc < 12; i++)
        argv[argc++] = extra[i];
    struct spawn_handle x[4] = { { EXTFS_SR_BLOCK, block }, { EXTFS_SR_SERVE, serve },
                                 { EXTFS_SR_CTL, ctl } };
    rights_t xr[4] = { RIGHT_SAME, RIGHT_SAME, RIGHT_SAME, RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT };
    unsigned nx = 3;
    if (pager) {
        CHECK_ST(jam_handle_duplicate(pager, RIGHT_SAME, &x[nx].h), OK);
        x[nx++].role = EXTFS_SR_PAGER;
    }
    struct spawn_args a = {
        .path = "bin/extfs", .argc = argc, .argv = argv, .job = r->job, .extra = x, .nextra = nx,
        .extra_rights = xr,
    };
    CHECK_ST(spawn(&a, &r->proc), OK);
    return true;
}

bool extfs_start(struct fatrun *r, struct ramdisk *rd, bool read_only, const char *const *extra,
                 unsigned n)
{
    return start(r, rd, read_only, extra, n, HANDLE_INVALID);
}

bool extfs_start_pager(struct fatrun *r, struct ramdisk *rd, handle_t pager)
{
    return start(r, rd, false, NULL, 0, pager);
}

/* Saved sparse (the stick's /data is small): "jamspars", a u64 of the
 * disk's size, then each 4 KiB block that isn't all zeros as a u64 offset
 * and its bytes (tools/extfs-svc-test.sh makes the image again). */
bool extfs_save(struct ramdisk *rd, const char *name)
{
    static uint8_t rec[8 + 4096];
    bool dir = false;
    if (fs_stat(SAVE_DIR, NULL, &dir, NULL) != OK || !dir)
        return true;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.sparse", SAVE_DIR, name);
    struct jfile f;
    uint64_t size = (uint64_t)rd->blocks * RAMDISK_SECTOR, at = 0;
    size_t done = 0;
    CHECK_ST(file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f), OK);
    memcpy(rec, "jamspars", 8);
    memcpy(rec + 8, &size, 8);
    status_t st = file_write(&f, at, rec, 16, &done);
    at += 16;
    for (uint64_t off = 0; st == OK && off < size; off += 4096) {
        const uint8_t *b = rd->mem + off;
        unsigned i = 0;
        while (i < 4096 && !b[i])
            i++;
        if (i == 4096)
            continue;
        memcpy(rec, &off, 8);
        memcpy(rec + 8, b, 4096);
        st = file_write(&f, at, rec, sizeof(rec), &done);
        at += sizeof(rec);
    }
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    CHECK_ST(st, OK);
    printf("utest: %s: the disk saved as %s for e2fsck (%lu KiB)\n", utest_cur, path,
           (unsigned long)(at >> 10));
    return true;
}

status_t xmode(handle_t fs, const char *path, uint32_t flags, uint32_t *mode, uint32_t *nlink)
{
    uint32_t uid, gid, nl;
    uint64_t size, blocks, id;
    int64_t at, mt, ct;
    return fs_getattr_until(fs, dl(), path, L(path), flags, mode, nlink ? nlink : &nl, &uid,
                            &gid, &size, &blocks, &id, &at, &mt, &ct);
}

static status_t xsymlink(handle_t fs, const char *target, const char *path)
{
    return fs_symlink_until(fs, dl(), target, L(target), path, L(path));
}

static status_t xlink(handle_t fs, const char *from, const char *to)
{
    return fs_link_until(fs, dl(), from, L(from), to, L(to));
}

static status_t xchmod(handle_t fs, const char *path, uint32_t mode)
{
    return fs_setattr_until(fs, dl(), path, L(path), 0, FS_ATTR_MODE, mode, 0, 0, 0, 0);
}

static status_t xresolve(handle_t fs, const char *path, uint32_t flags, uint8_t *kind,
                         uint16_t *used, char *next)
{
    uint32_t n = 0;
    status_t st = fs_resolve_until(fs, dl(), path, L(path), flags, kind, used, next, &n);
    next[st == OK ? n : 0] = '\0';
    return st;
}

static uint64_t free_bytes(const struct fatrun *r)
{
    uint64_t total = 0, fr = 0;
    return t_free(r, &total, &fr) == OK ? fr : 0;
}

/* A view of r's fs channel: the same client calls on it. */
static bool view_of(const struct fatrun *r, handle_t ch, struct fatrun *v)
{
    *v = *r;
    v->fs = ch;
    return true;
}

/* The first batch of a directory has an entry `name` of DT_* type `type`. */
static bool has_entry(handle_t fs, const char *dir, const char *name, uint8_t type)
{
    static struct tbatch b;
    CHECK_ST(fs_readdir_until(fs, dl(), dir, L(dir), 0, &b.next, &b.count, b.entries, &b.len),
             OK);
    uint32_t at = 0;
    struct fs_dirent e;
    while (fs_dirent_get(b.entries, b.len, &at, &e))
        if (e.name_len == L(name) && !memcmp(e.name, name, e.name_len))
            return e.type == type;
    return false;
}

/* ---- files and directories --------------------------------------------------- */

bool t_extfs_basic(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    uint64_t total = 0, fr = 0;
    uint8_t ro = 1, label[16] = { 0 };
    uint32_t magic = 0, name_max = 0, feat = 0;
    CHECK_ST(fs_statfs_until(r.fs, dl(), &total, &fr, &ro, label, &magic, &name_max, &feat), OK);
    CHECK_EQ(magic, 0xef53u);
    CHECK_EQ(name_max, 255u);
    CHECK_EQ(feat, FS_FEAT_LINKS | FS_FEAT_MODES | FS_FEAT_CASE | FS_FEAT_BIG |
                   FS_FEAT_MULTI_WRITER);
    CHECK(!ro && total > 28u << 20 && fr < total && !memcmp(label, "jam", 4));

    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK_ST(t_mkdir(&r, "/d"), ERR_ALREADY_EXISTS);
    CHECK(put_file(&r, "/d/a.txt", "hello"));
    CHECK(file_is(&r, "/d/a.txt", "hello"));
    CHECK_ST(t_stat(&r, "/d/A.txt", NULL, NULL, NULL), ERR_NOT_FOUND);   /* names keep case */
    CHECK_ST(t_stat(&r, "/d/a.txt/", NULL, NULL, NULL), ERR_NOT_DIR);
    CHECK_ST(t_stat(&r, "/d/a.txt/x", NULL, NULL, NULL), ERR_NOT_DIR);
    CHECK(has_entry(r.fs, "/d", "a.txt", FS_DT_REG));
    CHECK(has_entry(r.fs, "/", "d", FS_DT_DIR));

    /* 320 KiB through the 64 KiB buffer, read back. */
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(&r, "/d/big", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    for (uint32_t c = 0; c < 5; c++) {
        for (uint32_t i = 0; i < sizeof(big); i++)
            big[i] = (uint8_t)(i * 7 + c);
        CHECK_ST(t_write(&f, (uint64_t)c * sizeof(big), big, sizeof(big), &done), OK);
        CHECK_EQ(done, sizeof(big));
    }
    for (uint32_t c = 0; c < 5; c++) {
        CHECK_ST(t_read(&f, (uint64_t)c * sizeof(big), back, sizeof(back), &done), OK);
        CHECK_EQ(done, sizeof(back));
        for (uint32_t i = 0; i < sizeof(back); i++)
            if (back[i] != (uint8_t)(i * 7 + c))
                FAIL("byte %u of chunk %u: %u", i, c, back[i]);
    }
    t_close(&f);
    uint64_t size = 0;
    CHECK_ST(t_stat(&r, "/d/big", &size, NULL, NULL), OK);
    CHECK_EQ(size, 5ull * sizeof(big));

    /* A directory of 300 names, listed in batches. */
    CHECK_ST(t_mkdir(&r, "/many"), OK);
    char p[64];
    for (unsigned i = 0; i < 300; i++) {
        snprintf(p, sizeof(p), "/many/file-with-a-longish-name-%03u", i);
        CHECK(put_file(&r, p, "m"));
    }
    unsigned count = 0;
    bool found = false;
    CHECK(dir_count(&r, "/many", "file-with-a-longish-name-299", &count, &found));
    CHECK(count == 300 && found);

    /* Renames: the id stays (it is the inode); one onto another replaces only with REPLACE. */
    uint64_t id = 0, id2 = 0;
    CHECK_ST(t_stat_id(&r, "/d/a.txt", &id), OK);
    CHECK_ST(t_mkdir(&r, "/e"), OK);
    CHECK_ST(t_rename(&r, "/d/a.txt", "/e/b.txt"), OK);
    CHECK_ST(t_stat_id(&r, "/e/b.txt", &id2), OK);
    CHECK_EQ(id, id2);
    CHECK(put_file(&r, "/e/c.txt", "c"));
    CHECK_ST(t_rename(&r, "/e/c.txt", "/e/b.txt"), ERR_ALREADY_EXISTS);
    CHECK_ST(t_rename_flags(&r, "/e/c.txt", "/e/b.txt", FS_RENAME_REPLACE), OK);
    CHECK(file_is(&r, "/e/b.txt", "c"));
    CHECK_ST(t_rename(&r, "/e", "/e/inside"), ERR_INVALID_ARGS);
    CHECK_ST(t_rename(&r, "/", "/x"), ERR_INVALID_ARGS);

    CHECK_ST(t_unlink(&r, "/e"), ERR_NOT_EMPTY);
    CHECK_ST(t_unlink(&r, "/e/b.txt"), OK);
    CHECK_ST(t_unlink(&r, "/e"), OK);
    CHECK_ST(t_unlink_flags(&r, "/", FS_UNLINK_DIR), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink_flags(&r, "/d/big", FS_UNLINK_DIR), ERR_NOT_DIR);
    CHECK_ST(t_unlink_flags(&r, "/d", 0), ERR_IS_DIR);

    /* Everything is on the disk at each answer: a new extfs reads it. */
    CHECK(fat_stop(&r));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_stat(&r, "/d/big", &size, NULL, NULL), OK);
    CHECK_EQ(size, 5ull * sizeof(big));
    CHECK_ST(t_stat(&r, "/e", NULL, NULL, NULL), ERR_NOT_FOUND);
    count = 0;
    CHECK(dir_count(&r, "/many", NULL, &count, NULL));
    CHECK_EQ(count, 300u);
    CHECK_ST(fsctl_stop_until(r.ctl, dl()), OK);
    CHECK(fat_wait(&r, 0));
    CHECK_ST(jam_handle_close(r.fs), OK);
    CHECK(ramdisk_join(&xd));
    CHECK(extfs_save(&xd, "basic"));
    return ramdisk_destroy(&xd);
}

/* ---- links ------------------------------------------------------------------ */

bool t_extfs_links(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    char next[FS_PATH_LEN + 1], target[FS_PATH_LEN + 1];
    uint8_t kind = 9;
    uint16_t used = 0;
    uint32_t mode = 0, nlink = 0, n = 0;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_mkdir(&r, "/t"), OK);
    CHECK_ST(t_mkdir(&r, "/t/sub"), OK);
    CHECK(put_file(&r, "/t/f", "x"));

    CHECK_ST(xsymlink(r.fs, "f", "/t/l1"), OK);
    CHECK_ST(xsymlink(r.fs, "g", "/t/l1"), ERR_ALREADY_EXISTS);
    CHECK(file_is(&r, "/t/l1", "x"));
    CHECK_ST(fs_readlink_until(r.fs, dl(), "/t/l1", 5, target, &n), OK);
    CHECK(n == 1 && target[0] == 'f');
    CHECK_ST(fs_readlink_until(r.fs, dl(), "/t/f", 4, target, &n), ERR_INVALID_ARGS);
    CHECK_ST(xmode(r.fs, "/t/l1", FS_NOFOLLOW, &mode, NULL), OK);
    CHECK_EQ(mode & FS_MODE_FMT, FS_MODE_LNK);
    CHECK_ST(xmode(r.fs, "/t/l1", 0, &mode, NULL), OK);
    CHECK_EQ(mode & FS_MODE_FMT, FS_MODE_REG);
    CHECK(has_entry(r.fs, "/t", "l1", FS_DT_LNK));
    struct tfile f;
    CHECK_ST(t_open(&r, "/t/l1", FS_READ | FS_NOFOLLOW, &f), ERR_INVALID_ARGS);
    CHECK_ST(t_mkdir(&r, "/t/l1"), ERR_ALREADY_EXISTS);

    /* Relative links, ".." physical: sub/l2 -> ../f; a long target lives in a block. */
    CHECK_ST(xsymlink(r.fs, "../f", "/t/sub/l2"), OK);
    CHECK(file_is(&r, "/t/sub/l2", "x"));
    CHECK_ST(xsymlink(r.fs, "sub", "/t/ls"), OK);
    CHECK(file_is(&r, "/t/ls/../f", "x"));   /* .. of sub, through the link: /t */
    memset(target, 0, sizeof(target));
    for (unsigned i = 0; i < 100; i++)
        memcpy(target + 2 * i, "./", 2);
    memcpy(target + 200, "f", 2);
    CHECK_ST(xsymlink(r.fs, target, "/t/long"), OK);
    CHECK(file_is(&r, "/t/long", "x"));

    /* Leading out: an absolute link, and ".." above the root. */
    CHECK_ST(xsymlink(r.fs, "/abs/x", "/t/la"), OK);
    CHECK_ST(t_stat(&r, "/t/la/rest", NULL, NULL, NULL), ERR_LINK_OUT);
    CHECK_ST(xresolve(r.fs, "/t/la/rest", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_ABSOLUTE && used == L("/t/la") && !strcmp(next, "/abs/x/rest"));
    CHECK_ST(t_stat(&r, "/t/../../up/x", NULL, NULL, NULL), ERR_LINK_OUT);
    CHECK_ST(xresolve(r.fs, "/t/../../up/x", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_ABOVE && used == L("/t/../..") && !strcmp(next, "up/x"));
    CHECK_ST(xsymlink(r.fs, "../../out", "/t/lup"), OK);
    CHECK_ST(xresolve(r.fs, "/t/lup/y", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_ABOVE && used == L("/t/lup") && !strcmp(next, "out/y"));
    CHECK_ST(xresolve(r.fs, "/t/sub/l2", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_INSIDE && used == L("/t/sub/l2") && !strcmp(next, "t/f"));
    CHECK_ST(xresolve(r.fs, "/t/sub/l2", FS_NOFOLLOW, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_INSIDE && !strcmp(next, "t/sub/l2"));
    CHECK_ST(xresolve(r.fs, "/t/ls/new", 0, &kind, &used, next), OK);   /* not there yet */
    CHECK(kind == FS_RESOLVE_INSIDE && !strcmp(next, "t/sub/new"));

    /* A loop, and dangling links. */
    CHECK_ST(xsymlink(r.fs, "lp", "/t/lp"), OK);
    CHECK_ST(t_stat(&r, "/t/lp", NULL, NULL, NULL), ERR_LINK_LOOP);
    CHECK_ST(xsymlink(r.fs, "nothere", "/t/dang"), OK);
    CHECK_ST(t_open(&r, "/t/dang", FS_WRITE | FS_CREATE | FS_EXCL, &f), ERR_ALREADY_EXISTS);
    CHECK_ST(t_mkdir(&r, "/t/dang"), ERR_ALREADY_EXISTS);
    CHECK(put_file(&r, "/t/dang", "made"));   /* through the link: its target made */
    CHECK(file_is(&r, "/t/nothere", "made"));

    /* Hard links. */
    CHECK_ST(xlink(r.fs, "/t/f", "/t/h"), OK);
    CHECK_ST(xmode(r.fs, "/t/f", 0, &mode, &nlink), OK);
    CHECK_EQ(nlink, 2u);
    CHECK_ST(xlink(r.fs, "/t/f", "/t/h"), ERR_ALREADY_EXISTS);
    CHECK_ST(xlink(r.fs, "/t/sub", "/t/hd"), ERR_IS_DIR);
    CHECK(put_file(&r, "/t/h", "via h"));
    CHECK(file_is(&r, "/t/f", "via h"));
    CHECK_ST(t_unlink(&r, "/t/f"), OK);
    CHECK_ST(xmode(r.fs, "/t/h", 0, &mode, &nlink), OK);
    CHECK_EQ(nlink, 1u);
    CHECK(file_is(&r, "/t/h", "via h"));

    /* The exchange. */
    CHECK(put_file(&r, "/t/e1", "one"));
    CHECK(put_file(&r, "/t/sub/e2", "two"));
    CHECK_ST(t_rename_flags(&r, "/t/e1", "/t/sub/e2", FS_RENAME_EXCHANGE), OK);
    CHECK(file_is(&r, "/t/e1", "two") && file_is(&r, "/t/sub/e2", "one"));
    CHECK_ST(t_rename_flags(&r, "/t/e1", "/t/none", FS_RENAME_EXCHANGE), ERR_NOT_FOUND);

    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, "links"));
    return ramdisk_destroy(&xd);
}

/* ---- the permission bits -------------------------------------------------------- */

bool t_extfs_perms(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    struct tfile f;
    uint32_t mode = 0;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_mkdir(&r, "/p"), OK);
    CHECK(put_file(&r, "/p/f", "x"));
    CHECK_ST(xmode(r.fs, "/p/f", 0, &mode, NULL), OK);
    CHECK_EQ(mode, FS_MODE_REG | FS_MODE_FILE_DEFAULT);

    CHECK_ST(xchmod(r.fs, "/p/f", 0), OK);
    CHECK_ST(xmode(r.fs, "/p/f", 0, &mode, NULL), OK);
    CHECK_EQ(mode, FS_MODE_REG);
    CHECK_ST(t_open(&r, "/p/f", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&r, "/p/f", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(xchmod(r.fs, "/p/f", 0444), OK);
    CHECK(file_is(&r, "/p/f", "x"));
    CHECK_ST(t_open(&r, "/p/f", FS_WRITE | FS_TRUNCATE, &f), ERR_ACCESS_DENIED);

    /* A directory: search to look in it, write (and search) to name in it, read to list it. */
    CHECK_ST(xchmod(r.fs, "/p", 0644), OK);
    CHECK_ST(t_stat(&r, "/p/f", NULL, NULL, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(xchmod(r.fs, "/p", 0555), OK);
    CHECK_ST(t_stat(&r, "/p/f", NULL, NULL, NULL), OK);
    CHECK_ST(t_open(&r, "/p/new", FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(&r, "/p/f"), ERR_ACCESS_DENIED);
    CHECK_ST(xchmod(r.fs, "/p", 0311), OK);
    static struct tbatch b;
    CHECK_ST(t_batch(&r, "/p", 0, &b), ERR_ACCESS_DENIED);
    CHECK_ST(xchmod(r.fs, "/p", 0755), OK);
    CHECK_ST(t_batch(&r, "/p", 0, &b), OK);

    /* Owners: the one user only; the create's mode as asked. */
    CHECK_ST(fs_setattr_until(r.fs, dl(), "/p/f", 4, 0, FS_ATTR_UID, 0, 0, 0, 0, 0),
             ERR_ACCESS_DENIED);
    CHECK_ST(fs_setattr_until(r.fs, dl(), "/p/f", 4, 0, FS_ATTR_UID | FS_ATTR_GID, 0,
                              FS_ONE_USER, FS_ONE_USER, 0, 0), OK);
    CHECK_ST(fs_setattr_until(r.fs, dl(), "/p/f", 4, 0, 64, 0, 0, 0, 0, 0), ERR_INVALID_ARGS);
    handle_t ch, vmo;
    uint64_t size;
    CHECK_ST(fs_open_until(r.fs, dl(), "/p/m", 4, FS_WRITE | FS_CREATE, 0600, &ch, &vmo, &size),
             OK);
    jam_handle_close(ch);
    jam_handle_close(vmo);
    CHECK_ST(xmode(r.fs, "/p/m", 0, &mode, NULL), OK);
    CHECK_EQ(mode, FS_MODE_REG | 0600u);
    CHECK_ST(fs_mkdir_until(r.fs, dl(), "/p/dm", 5, 0700), OK);
    CHECK_ST(xmode(r.fs, "/p/dm", 0, &mode, NULL), OK);
    CHECK_EQ(mode, FS_MODE_DIR | 0700u);

    /* Times, to the nanosecond. */
    const int64_t t = 1700000000123456789ll;
    CHECK_ST(fs_setattr_until(r.fs, dl(), "/p/f", 4, 0, FS_ATTR_MTIME | FS_ATTR_ATIME, 0, 0, 0,
                              t, t + 1), OK);
    uint32_t uid, gid, nl;
    uint64_t blocks, id;
    int64_t at, mt, ct;
    CHECK_ST(fs_getattr_until(r.fs, dl(), "/p/f", 4, 0, &mode, &nl, &uid, &gid, &size, &blocks,
                              &id, &at, &mt, &ct), OK);
    CHECK(at == t && mt == t + 1 && uid == FS_ONE_USER && gid == FS_ONE_USER && nl == 1);
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, "perms"));
    return ramdisk_destroy(&xd);
}

/* ---- many writers, open files unlinked and cut ----------------------------------- */

bool t_extfs_writers(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    struct tfile a, b;
    uint32_t done = 0;
    char got[64];
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_open(&r, "/w", FS_READ | FS_WRITE | FS_CREATE, &a), OK);
    CHECK_ST(t_open(&r, "/w", FS_READ | FS_WRITE, &b), OK);   /* a second writer */
    CHECK_ST(t_write(&a, 0, "aaaa", 4, &done), OK);
    CHECK_ST(t_read(&b, 0, got, 4, &done), OK);
    CHECK(done == 4 && !memcmp(got, "aaaa", 4));
    CHECK_ST(t_write(&b, 4, "bbbb", 4, &done), OK);
    CHECK_ST(t_read(&a, 0, got, 8, &done), OK);
    CHECK(done == 8 && !memcmp(got, "aaaabbbb", 8));
    t_close(&a);
    t_close(&b);

    /* FS_APPEND from two opens: every write at the end as it is then. */
    CHECK_ST(t_open(&r, "/ap", FS_WRITE | FS_APPEND | FS_CREATE, &a), OK);
    CHECK_ST(t_open(&r, "/ap", FS_WRITE | FS_APPEND, &b), OK);
    for (unsigned i = 0; i < 10; i++) {
        CHECK_ST(t_write(&a, 0, "a", 1, &done), OK);
        CHECK_ST(t_write(&b, 0, "b", 1, &done), OK);
    }
    t_close(&a);
    t_close(&b);
    CHECK(file_is(&r, "/ap", "abababababababababab"));

    /* Unlinked while open: still read and written; freed at the last close. */
    uint64_t before = free_bytes(&r);
    CHECK_ST(t_open(&r, "/u", FS_READ | FS_WRITE | FS_CREATE, &a), OK);
    for (uint32_t i = 0; i < 16; i++) {
        memset(big, (int)i, sizeof(big));
        CHECK_ST(t_write(&a, (uint64_t)i * sizeof(big), big, sizeof(big), &done), OK);
    }
    CHECK_ST(t_open(&r, "/u", FS_READ, &b), OK);
    CHECK_ST(t_unlink(&r, "/u"), OK);
    CHECK_ST(t_stat(&r, "/u", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_read(&b, 15ull * sizeof(big), back, sizeof(back), &done), OK);
    CHECK(done == sizeof(back) && back[0] == 15 && back[sizeof(back) - 1] == 15);
    uint32_t mode, nlink = 9, uid, gid;
    uint64_t size, blocks, id;
    int64_t at, mt, ct;
    CHECK_ST(file_getattr_until(b.ch, dl(), &mode, &nlink, &uid, &gid, &size, &blocks, &id, &at,
                                &mt, &ct), OK);
    CHECK(nlink == 0 && size == 16ull * sizeof(big));
    CHECK(free_bytes(&r) <= before - (1u << 20));   /* still held */
    t_close(&a);
    CHECK_ST(t_read(&b, 0, back, 16, &done), OK);
    t_close(&b);
    CHECK_ST(t_stat(&r, "/", NULL, NULL, NULL), OK);   /* the close is seen first */
    CHECK(free_bytes(&r) >= before - (64u << 10));

    /* A rename over an open file; a truncate that gives the blocks back. */
    CHECK(put_file(&r, "/o", "old"));
    CHECK(put_file(&r, "/n", "new"));
    CHECK_ST(t_open(&r, "/o", FS_READ, &a), OK);
    CHECK_ST(t_rename_flags(&r, "/n", "/o", FS_RENAME_REPLACE), OK);
    CHECK_ST(t_read(&a, 0, got, 3, &done), OK);
    CHECK(done == 3 && !memcmp(got, "old", 3));
    t_close(&a);
    CHECK(file_is(&r, "/o", "new"));
    before = free_bytes(&r);
    CHECK_ST(t_open(&r, "/tr", FS_READ | FS_WRITE | FS_CREATE, &a), OK);
    for (uint32_t i = 0; i < 32; i++)
        CHECK_ST(t_write(&a, (uint64_t)i * sizeof(big), big, sizeof(big), &done), OK);
    CHECK_ST(file_truncate_until(a.ch, dl(), 1000), OK);
    uint8_t isdir;
    uint64_t mtime;
    CHECK_ST(file_stat_until(a.ch, dl(), &size, &isdir, &mtime, &id), OK);
    CHECK_EQ(size, 1000ull);
    CHECK_ST(file_truncate_until(a.ch, dl(), 100000), OK);   /* a hole after it */
    CHECK_ST(t_read(&a, 50000, back, 16, &done), OK);
    CHECK(done == 16 && !back[0] && !back[15]);
    t_close(&a);
    CHECK(free_bytes(&r) >= before - (64u << 10));
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, "writers"));
    return ramdisk_destroy(&xd);
}

/* ---- views ------------------------------------------------------------------------ */

bool t_extfs_views(void)
{
    static struct ramdisk xd;
    struct fatrun r, v, g, s, ro;
    struct tfile f;
    char next[FS_PATH_LEN + 1];
    uint8_t kind;
    uint16_t used;
    handle_t ch;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/allow", "secret"));
    CHECK(put_file(&r, "/etc/ssh/key", "k"));
    CHECK_ST(t_mkdir(&r, "/usr"), OK);
    CHECK_ST(t_mkdir(&r, "/usr/etc"), OK);
    CHECK(put_file(&r, "/usr/etc/conf", "c"));
    CHECK_ST(xsymlink(r.fs, "etc/allow", "/x"), OK);
    CHECK_ST(xsymlink(r.fs, "etc", "/e"), OK);
    CHECK_ST(xsymlink(r.fs, ".", "/dot"), OK);
    CHECK_ST(xsymlink(r.fs, "../etc/ssh", "/usr/k"), OK);

    /* A program's view: etc hidden and guarded. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_NO_ETC | FS_VIEW_GUARD_ETC, &ch), OK);
    CHECK(view_of(&r, ch, &v));
    CHECK_ST(t_open(&v, "/etc/allow", FS_READ, &f), ERR_ACCESS_DENIED);   /* as written */
    CHECK_ST(t_open(&v, "/x", FS_READ, &f), ERR_ACCESS_DENIED);   /* where it resolves */
    CHECK_ST(t_stat(&v, "/e", NULL, NULL, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(t_stat(&v, "/e/allow", NULL, NULL, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&v, "/usr/k/key", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_stat(&v, "/usr/k/../allow", NULL, NULL, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(xlink(v.fs, "/e/allow", "/hl"), ERR_ACCESS_DENIED);
    CHECK(!has_entry(v.fs, "/", "etc", FS_DT_DIR) && has_entry(v.fs, "/", "usr", FS_DT_DIR));
    CHECK(!has_entry(v.fs, "/dot", "etc", FS_DT_DIR) && has_entry(v.fs, "/dot", "x", FS_DT_LNK));
    CHECK(file_is(&v, "/usr/etc/conf", "c"));

    /* A subview rooted at usr: its etc is no etc; nothing above it. */
    CHECK_ST(fs_subview_until(v.fs, dl(), "/usr", 4, 0, &ch), OK);
    CHECK(view_of(&r, ch, &s));
    CHECK(file_is(&s, "/etc/conf", "c"));
    CHECK(put_file(&s, "/etc/conf2", "c2"));
    CHECK(file_is(&r, "/usr/etc/conf2", "c2"));
    CHECK_ST(t_stat(&s, "/..", NULL, NULL, NULL), ERR_LINK_OUT);
    CHECK_ST(xresolve(s.fs, "/../etc/allow", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_ABOVE && !strcmp(next, "etc/allow"));
    CHECK_ST(xresolve(s.fs, "/k/key", 0, &kind, &used, next), OK);   /* k -> ../etc/ssh */
    CHECK(kind == FS_RESOLVE_ABOVE && !strcmp(next, "etc/ssh/key"));
    CHECK_ST(xresolve(s.fs, "/etc/conf", 0, &kind, &used, next), OK);
    CHECK(kind == FS_RESOLVE_INSIDE && !strcmp(next, "etc/conf"));
    handle_t ch2;
    CHECK_ST(fs_view_until(s.fs, dl(), FS_VIEW_READ_ONLY, &ch2), OK);   /* still at usr */
    CHECK(file_is(&(struct fatrun){ .fs = ch2 }, "/etc/conf", "c"));
    CHECK_ST(t_mkdir(&(struct fatrun){ .fs = ch2 }, "/new"), ERR_ACCESS_DENIED);
    jam_handle_close(ch2);
    CHECK_ST(fs_subview_until(v.fs, dl(), "/etc", 4, 0, &ch2), ERR_ACCESS_DENIED);
    CHECK_ST(fs_subview_until(v.fs, dl(), "/e", 2, FS_VIEW_READ_ONLY, &ch2), ERR_ACCESS_DENIED);
    CHECK_ST(fs_subview_until(v.fs, dl(), "/usr/etc/conf", 13, 0, &ch2), ERR_NOT_DIR);
    jam_handle_close(s.fs);

    /* Guarded only: etc is read, never changed, whatever names it. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_ETC, &ch), OK);
    CHECK(view_of(&r, ch, &g));
    CHECK(file_is(&g, "/x", "secret"));
    CHECK_ST(t_open(&g, "/x", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&g, "/x", FS_READ, &f), OK);
    CHECK_ST(file_setattr_until(f.ch, dl(), FS_ATTR_MODE, 0777, 0, 0, 0, 0), ERR_ACCESS_DENIED);
    t_close(&f);
    CHECK_ST(xchmod(g.fs, "/x", 0777), ERR_ACCESS_DENIED);
    CHECK_ST(xlink(g.fs, "/e/allow", "/hl"), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(&g, "/e/allow"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&g, "/usr/etc/conf", "/e/conf"), ERR_ACCESS_DENIED);
    CHECK(put_file(&g, "/usr/etc/conf", "ok"));
    jam_handle_close(g.fs);

    /* The packages' guard, judged where a path resolves: pkg through a
     * link is pkg; a view without the flag changes it. */
    CHECK_ST(t_mkdir(&r, "/pkg"), OK);
    CHECK(put_file(&r, "/pkg/f", "p"));
    CHECK_ST(xsymlink(r.fs, "pkg", "/pk"), OK);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_ETC | FS_VIEW_GUARD_PKG, &ch), OK);
    CHECK(view_of(&r, ch, &g));
    CHECK(file_is(&g, "/pk/f", "p"));
    CHECK_ST(t_open(&g, "/pkg/f", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(&g, "/pk/f"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&g, "/x", "/pk/x"), ERR_ACCESS_DENIED);
    /* ext's names have case, but the guard folds it as FAT does: stricter */
    CHECK_ST(t_mkdir(&g, "/PKG"), ERR_ACCESS_DENIED);
    CHECK_ST(fs_subview_until(g.fs, dl(), "/pkg", 4, 0, &ch2), ERR_ACCESS_DENIED);
    CHECK_ST(fs_subview_until(g.fs, dl(), "/pk", 3, FS_VIEW_READ_ONLY, &ch2), OK);
    jam_handle_close(ch2);
    jam_handle_close(g.fs);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_ETC, &ch), OK);
    CHECK(view_of(&r, ch, &g));
    CHECK(put_file(&g, "/pk/f", "q"));
    jam_handle_close(g.fs);

    /* Read-only: no write bit shown, no change made. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_READ_ONLY, &ch), OK);
    CHECK(view_of(&r, ch, &ro));
    uint32_t mode = 0;
    CHECK_ST(xmode(ro.fs, "/usr/etc/conf", 0, &mode, NULL), OK);
    CHECK_EQ(mode & 0222u, 0u);
    CHECK_ST(t_open(&ro, "/usr/etc/conf", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(xsymlink(ro.fs, "a", "/b"), ERR_ACCESS_DENIED);
    jam_handle_close(ro.fs);
    jam_handle_close(v.fs);
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, "views"));
    return ramdisk_destroy(&xd);
}

/* ---- a damaged block ----------------------------------------------------------- */

/* Every copy of `name` on the disk gets a byte changed (the directory
 * block that holds it, and the journal's copies). */
static unsigned spoil(struct ramdisk *rd, const char *name)
{
    size_t n = strlen(name), size = (size_t)rd->blocks * RAMDISK_SECTOR;
    unsigned hits = 0;
    for (size_t i = 0; i + n <= size; i++)
        if (rd->mem[i] == (uint8_t)name[0] && !memcmp(rd->mem + i, name, n)) {
            rd->mem[i + n - 1] ^= 1;
            hits++;
        }
    return hits;
}

bool t_extfs_error(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    uint64_t total, fr;
    uint8_t ro = 0, label[16];
    uint32_t magic, nmax, feat;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_mkdir(&r, "/bad"), OK);
    CHECK(put_file(&r, "/bad/zz-spoilt-name", "x"));
    CHECK(fat_stop(&r));
    CHECK(spoil(&xd, "zz-spoilt-name") >= 1);
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_stat(&r, "/bad/zz-spoilt-name", NULL, NULL, NULL), ERR_IO);
    CHECK_ST(fs_statfs_until(r.fs, dl(), &total, &fr, &ro, label, &magic, &nmax, &feat), OK);
    CHECK(ro);
    CHECK_ST(t_mkdir(&r, "/q"), ERR_ACCESS_DENIED);
    CHECK(fat_stop(&r));
    uint16_t state;
    memcpy(&state, xd.mem + 1024 + 58, 2);   /* s_state: errors found */
    CHECK(state & 2);
    CHECK(extfs_start(&r, &xd, false, NULL, 0));   /* read-only until e2fsck */
    CHECK_ST(fs_statfs_until(r.fs, dl(), &total, &fr, &ro, label, &magic, &nmax, &feat), OK);
    CHECK(ro);
    CHECK_ST(t_stat(&r, "/bad", NULL, NULL, NULL), OK);
    CHECK(fat_stop(&r));
    return ramdisk_destroy(&xd);
}

/* ---- a disk with a cache (docs/M13.6-REVIEW.md's R1) ---------------------------- */

/* Without EXTFS_ARG_FUA the disk's writes may sit in its cache (a USB
 * stick's), so the journal's order and every answer need block.sync: a
 * request that writes is flushed before its commit block and after it, an
 * fs.sync flushes what was written home since, and a second one, with
 * nothing new, sends nothing. With the word (drv/nvme's FUA writes) nothing
 * is ever flushed. */
bool t_extfs_flush(void)
{
    static struct ramdisk xd;
    struct fatrun r;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    uint32_t syncs = ramdisk_syncs(&xd);
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK(ramdisk_syncs(&xd) >= syncs + 2);   /* the commit block's two */
    CHECK(put_file(&r, "/d/a", "cached"));
    CHECK_ST(t_sync(&r), OK);
    syncs = ramdisk_syncs(&xd);
    CHECK_ST(t_sync(&r), OK);
    CHECK_EQ(ramdisk_syncs(&xd), syncs);   /* nothing written since: nothing sent */
    CHECK(fat_stop(&r));
    const char *fua = EXTFS_ARG_FUA;
    CHECK(extfs_start(&r, &xd, false, &fua, 1));
    syncs = ramdisk_syncs(&xd);
    CHECK(put_file(&r, "/d/b", "fua"));
    CHECK_ST(t_sync(&r), OK);
    CHECK_EQ(ramdisk_syncs(&xd), syncs);
    CHECK(file_is(&r, "/d/a", "cached") && file_is(&r, "/d/b", "fua"));
    CHECK(fat_stop(&r));
    return ramdisk_destroy(&xd);
}

/* ---- many at once (docs/M13.6-REVIEW.md's R3) ------------------------------------ */

#define MANY_FILES 200u
#define MANY_VIEWS 100u

/* 200 files open at once and 100 views, all usable together: one extfs
 * serves the whole system's volume on a drive boot (32 of each was the
 * limit, and 64 threads' temporary files ran out). */
bool t_extfs_many(void)
{
    static struct ramdisk xd;
    static struct tfile f[MANY_FILES];
    static handle_t v[MANY_VIEWS];
    struct fatrun r;
    char p[32], got[32];
    uint32_t done = 0;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(t_mkdir(&r, "/m"), OK);
    for (unsigned i = 0; i < MANY_FILES; i++) {
        snprintf(p, sizeof(p), "/m/f%03u", i);
        CHECK_ST(t_open(&r, p, FS_READ | FS_WRITE | FS_CREATE, &f[i]), OK);
        CHECK_ST(t_write(&f[i], 0, p, L(p), &done), OK);
    }
    for (unsigned i = 0; i < MANY_VIEWS; i++)
        CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_READ_ONLY, &v[i]), OK);
    for (unsigned i = 0; i < MANY_FILES; i++) {
        snprintf(p, sizeof(p), "/m/f%03u", i);
        CHECK_ST(t_read(&f[i], 0, got, L(p), &done), OK);
        CHECK(done == L(p) && !memcmp(got, p, done));
    }
    for (unsigned i = 0; i < MANY_VIEWS; i += MANY_VIEWS - 1) {
        struct fatrun w = { .fs = v[i] };
        CHECK(file_is(&w, "/m/f199", "/m/f199"));
    }
    for (unsigned i = 0; i < MANY_FILES; i++)
        t_close(&f[i]);
    for (unsigned i = 0; i < MANY_VIEWS; i++)
        CHECK_ST(jam_handle_close(v[i]), OK);
    CHECK(fat_stop(&r));
    return ramdisk_destroy(&xd);
}
