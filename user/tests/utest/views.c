/* utest: views of a filesystem (fs.view, <fsview.h>) on the fat service
 * over a RAM disk, through the protocol itself: a read-only view refuses
 * every change and says so in statfs; a guarded one refuses changes at or
 * under the top-level etc however the path is spelled; a view of a view
 * keeps both; a bad flag, and more views than fat serves, are refused;
 * the views go when fat does. And fs_view_in_etc's own rules. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fsview.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

static struct ramdisk disk;

bool t_view_etc_names(void)
{
    static const char *const in[] = { "etc", "etc/allow", "ETC", "Etc/x", "etc.", "etc ..",
                                      " etc", "etc./a" };
    static const char *const out[] = { "", "etcx", "et", "x/etc", "etc2/a", "e.tc", "allow" };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++)
        CHECK(fs_view_in_etc(in[i]));
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); i++)
        CHECK(!fs_view_in_etc(out[i]));
    return true;
}

/* r with its fs channel replaced by a view of it with these flags. */
static bool view_of(const struct fatrun *r, uint32_t flags, struct fatrun *v)
{
    *v = *r;
    CHECK_ST(fs_view_until(r->fs, now() + FAT_CALL_NS, flags, &v->fs), OK);
    return true;
}

static bool refuses_changes(const struct fatrun *v)
{
    struct tfile f;
    CHECK_ST(t_open(v, "/a.txt", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(v, "/b.txt", FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_mkdir(v, "/d"), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(v, "/a.txt"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/c.txt"), ERR_ACCESS_DENIED);
    CHECK(file_is(v, "/a.txt", "a"));   /* reading is fine */
    return true;
}

static bool guards_etc(const struct fatrun *v)
{
    struct tfile f;
    static const char *const etc[] = { "/etc/allow", "/ETC/allow", "/x/../etc/allow",
                                       "/./etc/new", "/etc", "/Etc/sub/x" };
    for (unsigned i = 0; i < sizeof(etc) / sizeof(etc[0]); i++) {
        CHECK_ST(t_open(v, etc[i], FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
        CHECK_ST(t_mkdir(v, etc[i]), ERR_ACCESS_DENIED);
        CHECK_ST(t_unlink(v, etc[i]), ERR_ACCESS_DENIED);
    }
    CHECK_ST(t_rename(v, "/etc", "/old"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/etc/a.txt"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/ETC"), ERR_ACCESS_DENIED);
    CHECK(file_is(v, "/etc/allow", "x"));   /* read as anyone may */
    /* Everywhere else it may write. */
    CHECK(put_file(v, "/etcetera.txt", "y"));
    CHECK_ST(t_mkdir(v, "/d"), OK);
    CHECK_ST(t_rename(v, "/etcetera.txt", "/d/e.txt"), OK);
    CHECK_ST(t_unlink(v, "/d/e.txt"), OK);
    CHECK_ST(t_unlink(v, "/d"), OK);
    return true;
}

bool t_fat_views(void)
{
    struct fatrun r, ro, g, both;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "a"));
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK(put_file(&r, "/etc/allow", "x"));
    /* Read-only: every change refused, statfs says so. */
    if (!view_of(&r, FS_VIEW_READ_ONLY, &ro) || !refuses_changes(&ro))
        return false;
    uint8_t read_only = 0;
    CHECK_ST(fs_statfs_until(ro.fs, now() + FAT_CALL_NS, NULL, NULL, &read_only, NULL), OK);
    CHECK_EQ(read_only, 1);
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, NULL, NULL, &read_only, NULL), OK);
    CHECK_EQ(read_only, 0);
    /* Guarded: etc only. */
    if (!view_of(&r, FS_VIEW_GUARD_ETC, &g) || !guards_etc(&g))
        return false;
    /* A view of the guarded one asking for nothing more is still guarded;
     * one of the read-only one is still read-only. */
    if (!view_of(&g, 0, &both) || !guards_etc(&both))
        return false;
    CHECK_ST(jam_handle_close(both.fs), OK);
    if (!view_of(&ro, FS_VIEW_GUARD_ETC, &both) || !refuses_changes(&both))
        return false;
    CHECK_ST(jam_handle_close(both.fs), OK);
    handle_t bad;
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, 4, &bad), ERR_INVALID_ARGS);
    /* The whole channel itself still changes etc. */
    CHECK(put_file(&r, "/etc/allow", "z"));
    CHECK(file_is(&g, "/etc/allow", "z"));
    CHECK_ST(jam_handle_close(ro.fs), OK);
    CHECK_ST(jam_handle_close(g.fs), OK);
    return fat_stop(&r);
}

/* fat serves FAT_VIEWS of them (32), refuses more, and serves a new one
 * once one is closed; closing its fs channel ends fat though views are
 * open (they see ERR_PEER_CLOSED). */
bool t_fat_view_limits(void)
{
    enum { MAX = 32 };
    static handle_t v[MAX + 1];
    struct fatrun r;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    unsigned n = 0;
    for (; n <= MAX; n++)
        if (fs_view_until(r.fs, now() + FAT_CALL_NS, FS_VIEW_READ_ONLY, &v[n]) != OK)
            break;
    CHECK_EQ(n, MAX);
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, 0, &v[MAX]), ERR_NO_RESOURCES);
    CHECK_ST(jam_handle_close(v[0]), OK);
    status_t st = ERR_NO_RESOURCES;
    for (uint64_t until = now() + 5 * NS_PER_S; st == ERR_NO_RESOURCES && now() < until;) {
        st = fs_view_until(r.fs, now() + FAT_CALL_NS, 0, &v[0]);   /* once fat saw it close */
        if (st == ERR_NO_RESOURCES)
            jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    CHECK_ST(st, OK);
    CHECK(fat_stop(&r));
    uint64_t total = 0;
    CHECK_ST(fs_statfs_until(v[1], now() + FAT_CALL_NS, &total, NULL, NULL, NULL),
             ERR_PEER_CLOSED);
    for (unsigned i = 0; i < MAX; i++)
        CHECK_ST(jam_handle_close(v[i]), OK);
    return true;
}
