/* utest: the calls M13.6 added to the `fs` and `file` protocols (abi/idl/
 * fs.idl, file.idl), as fat and the boot image answer them: getattr and
 * setattr made up as libc made stat up, readdir's type byte, statfs's
 * magic, name length and features, open's mode and its two new flags, the
 * rename flag, and ERR_NOT_SUPPORTED for the calls only a filesystem with
 * links has. Then the views (<fsview.h>): the new calls are judged like the
 * old ones. And libos's wrappers for all of it, on a fat mount and on /boot. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fsview.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define G "/g"   /* the fat mount of the libos test */

static struct ramdisk disk;

/* ---- the calls on a channel -------------------------------------------------------- */

static status_t ga(handle_t ch, const char *p, uint32_t flags, struct fs_attr *a)
{
    return fs_getattr_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), flags, &a->mode,
                            &a->nlink, &a->uid, &a->gid, &a->size, &a->blocks, &a->id,
                            &a->atime_ns, &a->mtime_ns, &a->ctime_ns);
}

static status_t sa(handle_t ch, const char *p, uint32_t flags, uint32_t mask, uint32_t uid,
                   uint32_t gid)
{
    return fs_setattr_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), flags, mask, 0640,
                            uid, gid, 5, 7);
}

static status_t symlink_on(handle_t ch, const char *target, const char *p)
{
    return fs_symlink_until(ch, now() + FAT_CALL_NS, target, (uint32_t)strlen(target), p,
                            (uint32_t)strlen(p));
}

static status_t readlink_on(handle_t ch, const char *p)
{
    char target[FS_PATH_LEN + 1];
    uint32_t len;
    return fs_readlink_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), target, &len);
}

static status_t link_on(handle_t ch, const char *from, const char *to)
{
    return fs_link_until(ch, now() + FAT_CALL_NS, from, (uint32_t)strlen(from), to,
                         (uint32_t)strlen(to));
}

static status_t resolve_on(handle_t ch, const char *p)
{
    char next[FS_PATH_LEN + 1];
    uint8_t kind;
    uint16_t used;
    uint32_t len;
    return fs_resolve_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), 0, &kind, &used,
                            next, &len);
}

static status_t subview_on(handle_t ch, const char *p, uint32_t flags)
{
    handle_t h = HANDLE_INVALID;
    status_t st = fs_subview_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), flags, &h);
    if (st == OK)
        jam_handle_close(h);
    return st;
}

/* fs.open with a mode, the two handles closed again. */
static status_t open_mode(handle_t ch, const char *p, uint32_t flags, uint32_t mode)
{
    handle_t file = HANDLE_INVALID, buf = HANDLE_INVALID;
    uint64_t size;
    status_t st = fs_open_until(ch, now() + FAT_CALL_NS, p, (uint32_t)strlen(p), flags, mode,
                                &file, &buf, &size);
    if (st == OK) {
        jam_handle_close(file);
        jam_handle_close(buf);
    }
    return st;
}

static status_t kind_of(handle_t ch, uint32_t *magic, uint32_t *name_max, uint32_t *features,
                        uint8_t *ro)
{
    return fs_statfs_until(ch, now() + FAT_CALL_NS, NULL, NULL, ro, NULL, magic, name_max,
                           features);
}

/* ---- fat's answers ------------------------------------------------------------------ */

static bool check_getattr(const struct fatrun *r)
{
    struct fs_attr a;
    uint64_t size = 0, mtime = 0, id = 0;
    bool dir = true;
    CHECK_ST(ga(r->fs, "/", 0, &a), OK);
    CHECK(a.mode == (FS_MODE_DIR | 0755) && a.nlink == 1 && a.uid == FS_ONE_USER &&
          a.gid == FS_ONE_USER && a.size == 0 && a.blocks == 0 && a.id == 1);
    CHECK(a.atime_ns == 0 && a.mtime_ns == 0 && a.ctime_ns == 0);   /* the root keeps no time */
    CHECK_ST(ga(r->fs, "/a.txt", 0, &a), OK);
    CHECK_ST(t_stat(r, "/a.txt", &size, &dir, &mtime), OK);
    CHECK_ST(t_stat_id(r, "/a.txt", &id), OK);
    CHECK(a.mode == (FS_MODE_REG | 0644) && a.nlink == 1 && a.uid == FS_ONE_USER &&
          a.gid == FS_ONE_USER && a.size == 5 && size == 5 && a.blocks == 1 && a.id == id);
    CHECK(mtime > 0 && a.mtime_ns == (int64_t)mtime * 1000000000ll && a.atime_ns == a.mtime_ns &&
          a.ctime_ns == a.mtime_ns);
    CHECK_ST(ga(r->fs, "/d", 0, &a), OK);
    CHECK(a.mode == (FS_MODE_DIR | 0755) && a.size == 0);
    CHECK_ST(ga(r->fs, "/d/../a.txt", FS_NOFOLLOW, &a), OK);   /* nothing is a link */
    CHECK_EQ(a.size, 5);
    CHECK_ST(ga(r->fs, "/BIG.bin", 0, &a), OK);   /* names ignore case on FAT */
    CHECK(a.size == 1030 && a.blocks == 3);   /* 512-byte units, rounded up */
    CHECK_ST(ga(r->fs, "/missing", 0, &a), ERR_NOT_FOUND);
    CHECK_ST(ga(r->fs, "/a.txt/x", 0, &a), ERR_NOT_DIR);
    CHECK_ST(ga(r->fs, "/a.txt", FS_READ, &a), ERR_INVALID_ARGS);   /* not a getattr flag */
    CHECK_ST(ga(r->fs, "/a.txt", 0x8000, &a), ERR_INVALID_ARGS);
    /* The open file says the same. */
    struct tfile f;
    struct fs_attr b;
    CHECK_ST(t_open(r, "/a.txt", FS_READ, &f), OK);
    CHECK_ST(file_getattr_until(f.ch, now() + FAT_CALL_NS, &b.mode, &b.nlink, &b.uid, &b.gid,
                                &b.size, &b.blocks, &b.id, &b.atime_ns, &b.mtime_ns,
                                &b.ctime_ns), OK);
    CHECK_ST(ga(r->fs, "/a.txt", 0, &a), OK);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    t_close(&f);
    return true;
}

static bool check_setattr(const struct fatrun *r)
{
    uint32_t all = FS_ATTR_MODE | FS_ATTR_UID | FS_ATTR_GID | FS_ATTR_ATIME | FS_ATTR_MTIME;
    struct fs_attr a, b;
    CHECK_ST(ga(r->fs, "/a.txt", 0, &a), OK);
    CHECK_ST(sa(r->fs, "/a.txt", 0, all, FS_ONE_USER, FS_ONE_USER), OK);
    CHECK_ST(sa(r->fs, "/a.txt", FS_NOFOLLOW, FS_ATTR_MODE, 0, 0), OK);   /* uid unasked: any */
    CHECK_ST(sa(r->fs, "/a.txt", 0, 0, 0, 0), OK);
    CHECK_ST(sa(r->fs, "/d", 0, FS_ATTR_MODE, 0, 0), OK);
    CHECK_ST(sa(r->fs, "/", 0, FS_ATTR_MODE, 0, 0), OK);
    CHECK_ST(sa(r->fs, "/a.txt", 0, FS_ATTR_UID, 0, FS_ONE_USER), ERR_ACCESS_DENIED);   /* no root */
    CHECK_ST(sa(r->fs, "/a.txt", 0, FS_ATTR_GID, FS_ONE_USER, 1001), ERR_ACCESS_DENIED);
    CHECK_ST(sa(r->fs, "/a.txt", 0, FS_ATTR_MASK + 1, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(sa(r->fs, "/a.txt", FS_READ, FS_ATTR_MODE, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(sa(r->fs, "/missing", 0, FS_ATTR_MODE, 0, 0), ERR_NOT_FOUND);
    CHECK_ST(sa(r->fs, "/a.txt/x", 0, FS_ATTR_MODE, 0, 0), ERR_NOT_DIR);
    /* FAT keeps none of it: the same attributes after. */
    CHECK_ST(ga(r->fs, "/a.txt", 0, &b), OK);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    struct tfile f;
    CHECK_ST(t_open(r, "/a.txt", FS_READ, &f), OK);   /* fchmod needs no write access */
    CHECK_ST(file_setattr_until(f.ch, now() + FAT_CALL_NS, all, 0600, FS_ONE_USER, FS_ONE_USER,
                                1, 2), OK);
    CHECK_ST(file_setattr_until(f.ch, now() + FAT_CALL_NS, FS_ATTR_UID, 0, 0, 0, 0, 0),
             ERR_ACCESS_DENIED);
    CHECK_ST(file_setattr_until(f.ch, now() + FAT_CALL_NS, 1u << 9, 0, 0, 0, 0, 0),
             ERR_INVALID_ARGS);
    t_close(&f);
    return true;
}

static bool check_open_and_dirs(const struct fatrun *r)
{
    struct fs_attr a;
    /* open's and mkdir's mode: FAT keeps none, what it made is the made-up. */
    CHECK_ST(open_mode(r->fs, "/n.txt", FS_WRITE | FS_CREATE, 0), OK);
    CHECK_ST(ga(r->fs, "/n.txt", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_REG | 0644);
    CHECK_ST(fs_mkdir_until(r->fs, now() + FAT_CALL_NS, "/m", 2, 0700), OK);
    CHECK_ST(ga(r->fs, "/m", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0755);
    /* FS_NOFOLLOW opens as it did; FS_DIRECTORY tells the kinds. */
    CHECK_ST(open_mode(r->fs, "/a.txt", FS_READ | FS_NOFOLLOW, 0644), OK);
    CHECK_ST(open_mode(r->fs, "/a.txt", FS_READ | FS_DIRECTORY, 0), ERR_NOT_DIR);
    CHECK_ST(open_mode(r->fs, "/d", FS_READ | FS_DIRECTORY, 0), ERR_IS_DIR);
    CHECK_ST(open_mode(r->fs, "/", FS_READ | FS_DIRECTORY, 0), ERR_IS_DIR);
    CHECK_ST(open_mode(r->fs, "/missing", FS_READ | FS_DIRECTORY, 0), ERR_NOT_FOUND);
    CHECK_ST(open_mode(r->fs, "/nope", FS_WRITE | FS_CREATE | FS_DIRECTORY, 0), ERR_INVALID_ARGS);
    CHECK_ST(t_stat(r, "/nope", NULL, NULL, NULL), ERR_NOT_FOUND);   /* and nothing was made */
    CHECK_ST(open_mode(r->fs, "/a.txt", FS_READ | 0x200, 0), ERR_INVALID_ARGS);
    return true;
}

/* readdir's type byte: the kind of each entry. */
static bool check_dirent_types(const struct fatrun *r)
{
    static struct tbatch b;
    struct fs_dirent e;
    uint32_t at = 0;
    unsigned files = 0, dirs = 0;
    CHECK_ST(t_batch(r, "/", 0, &b), OK);
    while (at < b.len) {
        CHECK(fs_dirent_get(b.entries, b.len, &at, &e));
        CHECK_EQ(e.type, e.is_dir ? FS_DT_DIR : FS_DT_REG);
        if (e.is_dir)
            dirs++;
        else
            files++;
    }
    CHECK(files >= 2 && dirs >= 2);
    return true;
}

/* A record's type byte against its flag, in the put and the get. */
bool t_dirent_types(void)
{
    uint8_t buf[256];
    uint32_t len = 0, at = 0;
    struct fs_dirent e;
    static const struct { uint8_t type, is_dir; bool ok; } cases[] = {
        { FS_DT_UNKNOWN, 0, true }, { FS_DT_UNKNOWN, 1, true }, { FS_DT_DIR, 1, true },
        { FS_DT_REG, 0, true }, { FS_DT_LNK, 0, true }, { FS_DT_DIR, 0, false },
        { FS_DT_REG, 1, false }, { FS_DT_LNK, 1, false }, { 1, 0, false }, { 12, 0, false },
        { 255, 1, false },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct fs_dirent d = {
            .id = 7, .size = 3, .mtime = 9, .is_dir = cases[i].is_dir, .type = cases[i].type,
            .name_len = 4, .name = "name",
        };
        len = 0;
        at = 0;
        CHECK(fs_dirent_put(buf, sizeof(buf), &len, &d));
        CHECK_EQ(len, FS_DIRENT_FIXED + 4);
        CHECK_EQ(buf[27], cases[i].type);
        CHECK_EQ(fs_dirent_get(buf, len, &at, &e), cases[i].ok);
        if (cases[i].ok)
            CHECK(e.type == cases[i].type && e.is_dir == !!cases[i].is_dir && at == len);
    }
    return true;
}

static bool check_rename_and_links(const struct fatrun *r)
{
    CHECK(put_file(r, "/x1", "one"));
    CHECK(put_file(r, "/x2", "two"));
    CHECK_ST(t_rename_flags(r, "/x1", "/x2", FS_RENAME_EXCHANGE), ERR_NOT_SUPPORTED);
    CHECK_ST(t_rename_flags(r, "/x1", "/x2", FS_RENAME_EXCHANGE | FS_RENAME_REPLACE),
             ERR_INVALID_ARGS);
    CHECK_ST(t_rename_flags(r, "/x1", "/x2", 4), ERR_INVALID_ARGS);
    CHECK(file_is(r, "/x1", "one") && file_is(r, "/x2", "two"));   /* nothing moved */
    CHECK_ST(t_unlink(r, "/x1"), OK);
    CHECK_ST(t_unlink(r, "/x2"), OK);
    /* What only a filesystem with links has. */
    CHECK_ST(symlink_on(r->fs, "a.txt", "/l"), ERR_NOT_SUPPORTED);
    CHECK_ST(t_stat(r, "/l", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(readlink_on(r->fs, "/a.txt"), ERR_NOT_SUPPORTED);
    CHECK_ST(link_on(r->fs, "/a.txt", "/b.txt"), ERR_NOT_SUPPORTED);
    CHECK_ST(resolve_on(r->fs, "/a.txt"), ERR_NOT_SUPPORTED);
    CHECK_ST(subview_on(r->fs, "/d", 0), ERR_NOT_SUPPORTED);
    return true;
}

static bool check_statfs(const struct fatrun *r, uint8_t want_ro)
{
    uint32_t magic = 0, name_max = 0, features = 9;
    uint8_t ro = 9;
    CHECK_ST(kind_of(r->fs, &magic, &name_max, &features, &ro), OK);
    CHECK(magic == 0x4d44 && name_max == 255 && features == 0 && ro == want_ro);
    return true;
}

bool t_fat_attrs(void)
{
    struct fatrun r;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    static char bytes[1031];
    memset(bytes, 'b', sizeof(bytes) - 1);
    CHECK(put_file(&r, "/a.txt", "hello"));
    CHECK(put_file(&r, "/Big.bin", bytes));
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    bool ok = check_getattr(&r) && check_setattr(&r) && check_open_and_dirs(&r) &&
              check_dirent_types(&r) && check_rename_and_links(&r) && check_statfs(&r, 0);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}

/* A read-only volume: the write bits gone, a change refused. */
bool t_fat_attrs_read_only(void)
{
    struct fatrun r;
    struct fs_attr a;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "hello"));
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK(fat_stop(&r));
    if (!fat_start(&r, &disk, true))
        return false;
    CHECK_ST(ga(r.fs, "/a.txt", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_REG | 0444);
    CHECK_ST(ga(r.fs, "/d", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0555);
    CHECK_ST(ga(r.fs, "/", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0555);
    CHECK_ST(sa(r.fs, "/a.txt", 0, FS_ATTR_MODE, 0, 0), ERR_ACCESS_DENIED);
    CHECK_ST(sa(r.fs, "/missing", 0, FS_ATTR_MODE, 0, 0), ERR_ACCESS_DENIED);
    struct tfile f;
    CHECK_ST(t_open(&r, "/a.txt", FS_READ, &f), OK);
    CHECK_ST(file_setattr_until(f.ch, now() + FAT_CALL_NS, FS_ATTR_MODE, 0600, 0, 0, 0, 0),
             ERR_ACCESS_DENIED);
    struct fs_attr b;
    CHECK_ST(file_getattr_until(f.ch, now() + FAT_CALL_NS, &b.mode, &b.nlink, &b.uid, &b.gid,
                                &b.size, &b.blocks, &b.id, &b.atime_ns, &b.mtime_ns,
                                &b.ctime_ns), OK);
    CHECK_EQ(b.mode, FS_MODE_REG | 0444);
    t_close(&f);
    bool ok = check_statfs(&r, 1);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}

/* ---- views --------------------------------------------------------------------------- */

static bool view_of(const struct fatrun *r, uint32_t flags, handle_t *v)
{
    CHECK_ST(fs_view_until(r->fs, now() + FAT_CALL_NS, flags, v), OK);
    return true;
}

/* A read-only view: no change at all, getattr without write bits, the reads as they are. */
static bool check_read_only_view(handle_t v)
{
    struct fs_attr a;
    CHECK_ST(ga(v, "/a.txt", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_REG | 0444);
    CHECK_ST(ga(v, "/d", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0555);
    CHECK_ST(sa(v, "/a.txt", 0, FS_ATTR_MODE, 0, 0), ERR_ACCESS_DENIED);
    CHECK_ST(symlink_on(v, "a.txt", "/l"), ERR_ACCESS_DENIED);
    CHECK_ST(link_on(v, "/a.txt", "/b"), ERR_ACCESS_DENIED);
    CHECK_ST(readlink_on(v, "/a.txt"), ERR_NOT_SUPPORTED);   /* a read: the service's answer */
    CHECK_ST(resolve_on(v, "/a.txt"), ERR_NOT_SUPPORTED);
    CHECK_ST(subview_on(v, "/d", 0), ERR_NOT_SUPPORTED);
    return true;
}

/* A guarded view: changes at or under etc refused, in any name, by every new call. */
static bool check_guard_view(handle_t v)
{
    static const char *const etc[] = { "/etc/allow", "/ETC/allow", "/x/../etc/allow", "/etc" };
    for (unsigned i = 0; i < sizeof(etc) / sizeof(etc[0]); i++) {
        CHECK_ST(sa(v, etc[i], 0, FS_ATTR_MODE, 0, 0), ERR_ACCESS_DENIED);
        CHECK_ST(symlink_on(v, "t", etc[i]), ERR_ACCESS_DENIED);   /* the name is the change */
        CHECK_ST(link_on(v, "/a.txt", etc[i]), ERR_ACCESS_DENIED);
        CHECK_ST(link_on(v, etc[i], "/b"), ERR_ACCESS_DENIED);     /* and the source */
    }
    CHECK_ST(symlink_on(v, "/etc/allow", "/l"), ERR_NOT_SUPPORTED);   /* the target is only text */
    CHECK_ST(sa(v, "/a.txt", 0, FS_ATTR_MODE, 0, 0), OK);
    CHECK_ST(link_on(v, "/a.txt", "/b"), ERR_NOT_SUPPORTED);
    /* A new root at or under etc would hide where it is: refused unless it is read-only. */
    CHECK_ST(subview_on(v, "/etc", 0), ERR_ACCESS_DENIED);
    CHECK_ST(subview_on(v, "/ETC/sub", 0), ERR_ACCESS_DENIED);
    CHECK_ST(subview_on(v, "/etc", FS_VIEW_READ_ONLY), ERR_NOT_SUPPORTED);
    CHECK_ST(subview_on(v, "/d", 0), ERR_NOT_SUPPORTED);
    return true;
}

/* A view that hides etc/ssh: every new call that names it, reads too. */
static bool check_keys_view(handle_t v)
{
    static const char *const keys[] = { "/etc/ssh/k", "/ETC/SSH/k", "/x/../etc/ssh/k", "/etc/ssh" };
    struct fs_attr a;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CHECK_ST(ga(v, keys[i], 0, &a), ERR_ACCESS_DENIED);
        CHECK_ST(sa(v, keys[i], 0, FS_ATTR_MODE, 0, 0), ERR_ACCESS_DENIED);
        CHECK_ST(readlink_on(v, keys[i]), ERR_ACCESS_DENIED);
        CHECK_ST(resolve_on(v, keys[i]), ERR_ACCESS_DENIED);
        CHECK_ST(symlink_on(v, "t", keys[i]), ERR_ACCESS_DENIED);
        CHECK_ST(link_on(v, keys[i], "/b"), ERR_ACCESS_DENIED);
        CHECK_ST(link_on(v, "/a.txt", keys[i]), ERR_ACCESS_DENIED);
        CHECK_ST(subview_on(v, keys[i], FS_VIEW_READ_ONLY), ERR_ACCESS_DENIED);
    }
    CHECK_ST(open_mode(v, "/etc/ssh/k", FS_READ | FS_NOFOLLOW, 0), ERR_ACCESS_DENIED);
    CHECK_ST(subview_on(v, "/etc", FS_VIEW_READ_ONLY), ERR_ACCESS_DENIED);   /* ssh is below it */
    CHECK_ST(ga(v, "/etc/allow", 0, &a), OK);   /* the rest of etc is as ever */
    return true;
}

bool t_fat_attr_views(void)
{
    struct fatrun r;
    handle_t ro, g, k;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "a"));
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK(put_file(&r, "/etc/allow", "x"));
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/ssh/k", "secret"));
    if (!view_of(&r, FS_VIEW_READ_ONLY, &ro) || !view_of(&r, FS_VIEW_GUARD_ETC, &g) ||
        !view_of(&r, FS_VIEW_HIDE_KEYS, &k))
        return false;
    bool ok = check_read_only_view(ro) && check_guard_view(g) && check_keys_view(k);
    jam_handle_close(ro);
    jam_handle_close(g);
    jam_handle_close(k);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}

/* ---- libos's wrappers: on a fat mount and on /boot ------------------------------------ */

static bool wrappers_on_fat(void)
{
    struct fs_attr a, b;
    struct fs_entry e;
    struct jfile f;
    char buf[8], next[4096];
    uint8_t kind;
    uint16_t used;
    handle_t h;
    uint32_t magic = 0, name_max = 0, features = 9;
    CHECK_ST(ns_put(G "/f.txt", "12345"), OK);
    CHECK_ST(fs_mkdir_mode(G "/dir", 0700), OK);
    CHECK_ST(fs_getattr(G "/f.txt", 0, &a), OK);
    CHECK(a.mode == (FS_MODE_REG | 0644) && a.size == 5 && a.nlink == 1 && a.uid == FS_ONE_USER);
    CHECK_ST(fs_getattr(G "/dir", FS_NOFOLLOW, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0755);
    CHECK_ST(fs_getattr(G "/nothing", 0, &a), ERR_NOT_FOUND);
    CHECK_ST(fs_getattr("/", 0, &a), OK);   /* made up by the namespace */
    CHECK(a.mode == (FS_MODE_DIR | 0555) && a.uid == FS_ONE_USER);
    CHECK_ST(fs_getattr("/nomount/x", 0, &a), ERR_NOT_FOUND);
    struct fs_attr_set set = { .mask = FS_ATTR_MODE | FS_ATTR_UID, .mode = 0600,
                               .uid = FS_ONE_USER };
    CHECK_ST(fs_setattr(G "/f.txt", 0, &set), OK);
    set.uid = 0;
    CHECK_ST(fs_setattr(G "/f.txt", 0, &set), ERR_ACCESS_DENIED);
    CHECK_ST(fs_setattr("/", 0, &set), ERR_ACCESS_DENIED);
    CHECK_ST(fs_symlink("f.txt", G "/l"), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_readlink(G "/f.txt", buf, sizeof(buf)), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_link(G "/f.txt", G "/h"), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_resolve(G "/f.txt", 0, &kind, &used, next), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_subview(G "/dir", 0, &h), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_statfs_kind(G, &magic, &name_max, &features), OK);
    CHECK(magic == 0x4d44 && name_max == 255 && features == 0);
    CHECK_ST(fs_statfs_kind(G, NULL, NULL, NULL), OK);
    /* readdir's type, through libos's batches. */
    bool saw_file = false, saw_dir = false;
    for (uint32_t i = 0; fs_readdir(G, i, &e) == OK; i++) {
        saw_file |= !strcmp(e.name, "f.txt") && e.type == FS_DT_REG;
        saw_dir |= !strcmp(e.name, "dir") && e.type == FS_DT_DIR;
    }
    CHECK(saw_file && saw_dir);
    /* open's mode, the flags, and the open file's attributes. */
    CHECK_ST(file_open_mode(G "/m.txt", FS_WRITE | FS_CREATE, 0, &f), OK);
    CHECK_ST(file_getattr(&f, &b), OK);
    CHECK(b.mode == (FS_MODE_REG | 0644) && b.size == 0);
    file_close(&f);
    CHECK_ST(file_open(G "/f.txt", FS_READ | FS_DIRECTORY, &f), ERR_NOT_DIR);
    CHECK_ST(file_open(G "/dir", FS_READ | FS_DIRECTORY, &f), ERR_IS_DIR);
    CHECK_ST(file_open(G "/f.txt", FS_READ | FS_NOFOLLOW, &f), OK);
    CHECK_ST(file_getattr(&f, &b), OK);
    CHECK_ST(fs_getattr(G "/f.txt", 0, &a), OK);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK_ST(file_setattr(&f, &(struct fs_attr_set){ .mask = FS_ATTR_MODE, .mode = 0600 }), OK);
    CHECK_ST(file_setattr(&f, &(struct fs_attr_set){ .mask = FS_ATTR_GID, .gid = 5 }),
             ERR_ACCESS_DENIED);
    file_close(&f);
    CHECK_ST(file_open(G "/f.txt", FS_READ | 0x200, &f), ERR_INVALID_ARGS);
    return true;
}

static bool wrappers_on_boot(void)
{
    struct fs_attr a, b;
    struct fs_entry e;
    struct jfile f;
    char buf[8], next[4096];
    uint8_t kind;
    uint16_t used;
    handle_t h;
    uint32_t magic = 0, name_max = 0, features = 9;
    uint64_t size = 0;
    CHECK_ST(fs_getattr("/boot/bin/utest", 0, &a), OK);   /* a program: 0555 on a read-only image */
    CHECK_ST(fs_stat("/boot/bin/utest", &size, NULL, NULL), OK);
    CHECK(a.mode == (FS_MODE_REG | 0555) && a.size == size && a.blocks == (size + 511) / 512 &&
          a.nlink == 1 && a.uid == FS_ONE_USER && a.gid == FS_ONE_USER && a.mtime_ns == 0);
    CHECK_ST(fs_getattr("/boot/init.cfg", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_REG | 0444);
    CHECK_ST(fs_getattr("/boot/bin", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0555);
    CHECK_ST(fs_getattr("/boot", 0, &a), OK);
    CHECK_EQ(a.mode, FS_MODE_DIR | 0555);
    CHECK_ST(fs_getattr("/boot/nothing", 0, &a), ERR_NOT_FOUND);
    CHECK_ST(fs_getattr("/boot/init.cfg", FS_READ, &a), ERR_INVALID_ARGS);
    struct fs_attr_set set = { .mask = FS_ATTR_MODE, .mode = 0777 };
    CHECK_ST(fs_setattr("/boot/init.cfg", 0, &set), ERR_ACCESS_DENIED);   /* read-only */
    CHECK_ST(fs_setattr("/boot/nothing", 0, &set), ERR_NOT_FOUND);
    set.mask = FS_ATTR_MASK + 1;
    CHECK_ST(fs_setattr("/boot/init.cfg", 0, &set), ERR_INVALID_ARGS);
    CHECK_ST(fs_symlink("x", "/boot/l"), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_readlink("/boot/init.cfg", buf, sizeof(buf)), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_link("/boot/init.cfg", "/boot/h"), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_link("/boot/init.cfg", G "/h"), ERR_NOT_SUPPORTED);   /* two mounts */
    CHECK_ST(fs_resolve("/boot/init.cfg", 0, &kind, &used, next), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_subview("/boot/bin", 0, &h), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_statfs_kind("/boot", &magic, &name_max, &features), OK);
    CHECK(magic == 0x858458f6 && name_max == 255 && features == 0);
    bool saw_file = false, saw_dir = false;
    for (uint32_t i = 0; fs_readdir("/boot", i, &e) == OK; i++) {
        saw_file |= !strcmp(e.name, "init.cfg") && e.type == FS_DT_REG && !e.is_dir;
        saw_dir |= !strcmp(e.name, "bin") && e.type == FS_DT_DIR && e.is_dir;
    }
    CHECK(saw_file && saw_dir);
    CHECK_ST(file_open("/boot/init.cfg", FS_READ | FS_DIRECTORY, &f), ERR_NOT_DIR);
    CHECK_ST(file_open("/boot/bin", FS_READ | FS_DIRECTORY, &f), ERR_IS_DIR);
    CHECK_ST(file_open("/boot/bin/utest", FS_READ | FS_NOFOLLOW, &f), OK);
    CHECK_ST(file_getattr(&f, &b), OK);
    CHECK_ST(fs_getattr("/boot/bin/utest", 0, &a), OK);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK_ST(file_setattr(&f, &(struct fs_attr_set){ .mask = FS_ATTR_MODE }), ERR_ACCESS_DENIED);
    file_close(&f);
    return true;
}

bool t_fs_attr_ns(void)
{
    struct fatrun r;
    handle_t fs;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &fs), OK);
    CHECK_ST(ns_mount(G, fs), OK);
    bool ok = wrappers_on_fat();
    bool dir = false;
    if (fs_stat("/boot", NULL, &dir, NULL) == OK && dir)
        ok = wrappers_on_boot() && ok;
    else
        printf("utest: %s: no /boot in our namespace: its half skipped\n", utest_cur);
    CHECK_ST(ns_unmount(G), OK);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}

/* ---- directory channels: servers without them --------------------------------------- */

/* fs.opendir, rename_at and link_at on a server without them: the
 * generated dispatch's ERR_NOT_SUPPORTED, no handle given, and statfs
 * without FS_FEAT_DIRS. */
static bool no_dirs(handle_t ch)
{
    handle_t dir = HANDLE_INVALID;
    uint64_t id = 7, volume = 7, deadline = now() + FAT_CALL_NS;
    uint32_t features = 0;
    CHECK_ST(fs_opendir_until(ch, deadline, "", 0, FS_READ, &dir, &id, &volume),
             ERR_NOT_SUPPORTED);
    CHECK(dir == HANDLE_INVALID);
    CHECK_ST(fs_opendir_until(ch, deadline, "/d", 2, 0, &dir, &id, &volume), ERR_NOT_SUPPORTED);
    CHECK(dir == HANDLE_INVALID);
    CHECK_ST(fs_rename_at_until(ch, deadline, "a", 1, 2, "b", 1, 0), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_link_at_until(ch, deadline, "a", 1, 2, "b", 1), ERR_NOT_SUPPORTED);
    CHECK_ST(kind_of(ch, NULL, NULL, &features, NULL), OK);
    CHECK(!(features & FS_FEAT_DIRS));
    return true;
}

/* fat, a fat view (the shared view code passes the methods on when it
 * refuses nothing in them: fs_view_dirs), the boot image and the tests'
 * ramfs: no directory channels, whatever is asked. */
bool t_fs_no_dirs(void)
{
    struct fatrun r;
    struct ram ram;
    handle_t v = HANDLE_INVALID, ch = HANDLE_INVALID;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    bool ok = view_of(&r, FS_VIEW_GUARD_ETC | FS_VIEW_HIDE_KEYS, &v) && no_dirs(r.fs) &&
              no_dirs(v);
    if (v != HANDLE_INVALID)
        jam_handle_close(v);
    ok = fat_stop(&r) && ramdisk_destroy(&disk) && ok;
    if (ns_channel("/boot", &ch) == OK) {
        ok = no_dirs(ch) && ok;
        jam_handle_close(ch);
    } else {
        printf("utest: %s: no /boot in our namespace: its part skipped\n", utest_cur);
    }
    CHECK(ram_start("/nd", &ram));
    CHECK_ST(ns_channel("/nd", &ch), OK);
    ok = no_dirs(ch) && ok;
    jam_handle_close(ch);
    return ram_stop("/nd", &ram) && ok;
}

/* A method the shared view code doesn't know (the fs protocol's 99th) on
 * ch: the status it is answered with. */
static status_t unknown_method(handle_t ch)
{
    struct idl_req_hdr q = { .txid = 0, .ordinal = FS_PROTOCOL_ID << 16 | 99 };
    struct idl_rep_hdr a = { 0 };
    handle_t hs[IDL_REP_HANDLES];
    uint32_t an = 0, hn = 0;
    status_t st = idl_call(ch, &q, sizeof(q), &a, sizeof(a), &an, hs, IDL_REP_HANDLES, &hn,
                           false, now() + FAT_CALL_NS);
    if (st != OK)
        return st;
    return an == sizeof(a) && !hn ? a.status : ERR_INTERNAL;
}

/* The directory channels' methods through views of fat (ro read-only, g
 * guarding etc and pkg, h hiding etc), which the shared view code judges
 * before fat says ERR_NOT_SUPPORTED to what gets past. */
static bool view_dirs(handle_t whole, handle_t ro, handle_t g, handle_t h)
{
    handle_t dir = HANDLE_INVALID;
    uint64_t id = 0, volume = 0, dl = now() + FAT_CALL_NS;
    /* opendir is a read; rename_at and link_at change */
    CHECK_ST(fs_opendir_until(ro, dl, "d", 1, FS_READ, &dir, &id, &volume), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_rename_at_until(ro, dl, "a", 1, 2, "b", 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_link_at_until(ro, dl, "a", 1, 2, "b", 1), ERR_ACCESS_DENIED);
    /* the guards: `from` as rename's and link's, `to` too when it starts
     * at the view's root; a relative `to` starts at to_dir (the service's
     * to judge), so its "etc" is no top-level etc here */
    CHECK_ST(fs_rename_at_until(g, dl, "etc/allow", 9, 2, "b", 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_rename_at_until(g, dl, "/x/../PKG/a", 11, 2, "b", 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_rename_at_until(g, dl, "a", 1, 2, "/etc/b", 6, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_link_at_until(g, dl, "Etc./allow", 10, 2, "b", 1), ERR_ACCESS_DENIED);
    CHECK_ST(fs_link_at_until(g, dl, "a", 1, 2, "/pkg/b", 6), ERR_ACCESS_DENIED);
    CHECK_ST(fs_rename_at_until(g, dl, "a", 1, 2, "etc/b", 5, 0), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_link_at_until(g, dl, "a", 1, 2, "pkg/b", 5), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_opendir_until(g, dl, "etc", 3, FS_READ, &dir, &id, &volume), ERR_NOT_SUPPORTED);
    /* etc hidden: opendir of it or in it; rename_at and link_at from it,
     * or to it from the root */
    CHECK_ST(fs_opendir_until(h, dl, "etc", 3, 0, &dir, &id, &volume), ERR_ACCESS_DENIED);
    CHECK_ST(fs_opendir_until(h, dl, "/ETC./ssh", 9, FS_READ, &dir, &id, &volume),
             ERR_ACCESS_DENIED);
    CHECK_ST(fs_opendir_until(h, dl, "d", 1, FS_READ, &dir, &id, &volume), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_rename_at_until(h, dl, "etc/allow", 9, 2, "b", 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_rename_at_until(h, dl, "a", 1, 2, "/etc/b", 6, 0), ERR_ACCESS_DENIED);
    CHECK_ST(fs_link_at_until(h, dl, "a", 1, 2, "/etc/ssh/k", 10), ERR_ACCESS_DENIED);
    CHECK_ST(fs_link_at_until(h, dl, "a", 1, 2, "etc/b", 5), ERR_NOT_SUPPORTED);
    CHECK(dir == HANDLE_INVALID);
    /* a method the view code doesn't know: refused by every view, the
     * whole volume's channel leaves it to fat */
    CHECK_ST(unknown_method(ro), ERR_ACCESS_DENIED);
    CHECK_ST(unknown_method(g), ERR_ACCESS_DENIED);
    CHECK_ST(unknown_method(h), ERR_ACCESS_DENIED);
    CHECK_ST(unknown_method(whole), ERR_NOT_SUPPORTED);
    return true;
}

/* The shared view code (user/lib/fsview.c) knows opendir, rename_at and
 * link_at (DIRHANDLE-PLAN's DP notes) and refuses an fs method it doesn't
 * know on a view. */
bool t_fs_view_dirs(void)
{
    struct fatrun r;
    handle_t ro = HANDLE_INVALID, g = HANDLE_INVALID, h = HANDLE_INVALID;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    bool ok = view_of(&r, FS_VIEW_READ_ONLY, &ro) &&
              view_of(&r, FS_VIEW_GUARD_ETC | FS_VIEW_GUARD_PKG, &g) &&
              view_of(&r, FS_VIEW_NO_ETC, &h) && view_dirs(r.fs, ro, g, h);
    handle_t vs[] = { ro, g, h };
    for (unsigned i = 0; i < 3; i++)
        if (vs[i] != HANDLE_INVALID)
            jam_handle_close(vs[i]);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
