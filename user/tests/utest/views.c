/* utest: views of a filesystem (fs.view, <fsview.h>) on the fat service
 * over a RAM disk, through the protocol itself: a read-only view refuses
 * every change and says so in statfs; a guarded one refuses changes at or
 * under the top-level etc however the path is spelled; a view of a view
 * keeps both; a bad flag, and more views than fat serves, are refused;
 * the views go when fat does. And fs_view_in_etc's own rules; a view that
 * hides etc/ssh (FS_VIEW_HIDE_KEYS) refuses every request there, by any
 * name, and fs_view_in_keys's rules; a view where etc doesn't exist
 * (FS_VIEW_NO_ETC, every program's) refuses every request at or under it,
 * a stat of it too, and leaves it out of the root's listing; the SSH
 * server's views (`:rk`, `:ra`) show its own files of etc and nothing else
 * of it; and fs_view_hides's rules. The packages' guard
 * (FS_VIEW_GUARD_PKG, every program's writable /data) the same for the
 * top-level pkg, by every name FAT takes for it, and a view without it
 * (bin/pkg's) that changes it; fs_view_in_pkg's rules. */
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

bool t_view_pkg_names(void)
{
    static const char *const in[] = { "pkg", "pkg/bin/vim", "PKG", "Pkg/x", "pkg.", "pkg ..",
                                      " pkg", "pkg./a", "  PkG . ./db/journal" };
    static const char *const out[] = { "", "pkgx", "pk", "x/pkg", "pkg2/a", "p.kg", "bin/vim",
                                       "etc", "pkg-old" };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++)
        CHECK(fs_view_in_pkg(in[i]));
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); i++)
        CHECK(!fs_view_in_pkg(out[i]));
    return true;
}

bool t_view_keys_names(void)
{
    static const char *const in[] = { "etc/ssh", "etc/ssh/.ed25519.sk", "ETC/SSH/x",
                                      "etc./ssh./ED2551~1.SK", " etc/ ssh/x", "Etc/Ssh .." };
    static const char *const out[] = { "", "etc", "etc/sshd", "etc/allow", "ssh/x",
                                       "x/etc/ssh", "etc/s/ssh", "etcssh" };
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++)
        CHECK(fs_view_in_keys(in[i]));
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); i++)
        CHECK(!fs_view_in_keys(out[i]));
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
    CHECK_ST(fs_statfs_until(ro.fs, now() + FAT_CALL_NS, NULL, NULL, &read_only, NULL, NULL, NULL,
                             NULL), OK);
    CHECK_EQ(read_only, 1);
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, NULL, NULL, &read_only, NULL, NULL, NULL,
                             NULL), OK);
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
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, FS_VIEW_FLAGS + 1, &bad),
             ERR_INVALID_ARGS);
    /* The whole channel itself still changes etc. */
    CHECK(put_file(&r, "/etc/allow", "z"));
    CHECK(file_is(&g, "/etc/allow", "z"));
    CHECK_ST(jam_handle_close(ro.fs), OK);
    CHECK_ST(jam_handle_close(g.fs), OK);
    return fat_stop(&r);
}

/* FS_VIEW_HIDE_KEYS: nothing at or under etc/ssh, however it is named,
 * by FAT's short name too; the rest of etc and of the volume as the view's
 * other flags say; a view of it can't show them again. */
static bool hides_keys(const struct fatrun *v)
{
    struct tfile f;
    static const char *const keys[] = { "/etc/ssh/.ed25519.sk", "/ETC/SSH/.ed25519.sk",
                                        "/x/../etc/ssh/.ed25519.sk", "/etc./ssh /.ed25519.sk",
                                        "/etc/ssh/ED2551~1.SK" };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        CHECK_ST(t_open(v, keys[i], FS_READ, &f), ERR_ACCESS_DENIED);
    uint64_t size = 0;
    uint8_t dir = 0;
    CHECK_ST(fs_stat_until(v->fs, now() + FAT_CALL_NS, "/etc/ssh", 8, &size, &dir, NULL, NULL),
             ERR_ACCESS_DENIED);
    static struct tbatch b;
    CHECK_ST(fs_readdir_until(v->fs, now() + FAT_CALL_NS, "/etc/ssh", 8, 0, &b.next, &b.count,
                              b.entries, &b.len), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/etc/ssh/.ed25519.sk", "/k"), ERR_ACCESS_DENIED);
    CHECK(file_is(v, "/etc/allow", "x"));   /* the rest of etc reads */
    CHECK(file_is(v, "/a.txt", "a"));
    return true;
}

/* A view with FS_VIEW_GUARD_PKG (every program's writable /data) refuses
 * every change at or under the top-level pkg, by every name FAT takes for
 * it, and reads it; one without the flag (bin/pkg's) changes it. */
static bool guards_pkg(const struct fatrun *v)
{
    struct tfile f;
    static const char *const pkg[] = { "/pkg/bin/vim", "/PKG/bin/vim", "/x/../pkg/new",
                                       "/./Pkg/db/x", "/pkg", "/pkg./db/y", "/ pkg/bin/vim",
                                       "/pkg ../bin/z", "/PKG/BIN/LONGFI~1.TXT" };
    for (unsigned i = 0; i < sizeof(pkg) / sizeof(pkg[0]); i++) {
        CHECK_ST(t_open(v, pkg[i], FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
        CHECK_ST(t_open(v, pkg[i], FS_WRITE | FS_TRUNCATE, &f), ERR_ACCESS_DENIED);
        CHECK_ST(t_mkdir(v, pkg[i]), ERR_ACCESS_DENIED);
        /* (not t_unlink: its stat first refuses a name fat can't hold) */
        CHECK_ST(t_unlink_flags(v, pkg[i], 0), ERR_ACCESS_DENIED);
        CHECK_ST(t_unlink_flags(v, pkg[i], FS_UNLINK_DIR), ERR_ACCESS_DENIED);
    }
    CHECK_ST(t_rename(v, "/pkg", "/old"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/PKG.", "/old"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/pkg/bin/a.txt"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/Pkg"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/pkg/bin/vim", "/vim"), ERR_ACCESS_DENIED);
    CHECK(file_is(v, "/pkg/bin/vim", "v"));   /* every program reads it */
    CHECK(file_is(v, "/PKG/BIN/LONGFI~1.TXT", "l"));
    CHECK(put_file(v, "/pkgs.txt", "y"));      /* and writes beside it */
    CHECK_ST(t_unlink(v, "/pkgs.txt"), OK);
    return true;
}

bool t_fat_view_pkg(void)
{
    struct fatrun r, g, both, w;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "a"));
    CHECK_ST(t_mkdir(&r, "/pkg"), OK);
    CHECK_ST(t_mkdir(&r, "/pkg/bin"), OK);
    CHECK_ST(t_mkdir(&r, "/pkg/db"), OK);
    CHECK(put_file(&r, "/pkg/bin/vim", "v"));
    CHECK(put_file(&r, "/pkg/bin/longfilename.txt", "l"));
    CHECK(put_file(&r, "/pkg/db/x", "x"));
    /* Every program's writable /data (`:w`): etc and pkg guarded. */
    if (!view_of(&r, FS_VIEW_GUARD_ETC | FS_VIEW_GUARD_PKG | FS_VIEW_NO_ETC, &g) ||
        !guards_pkg(&g))
        return false;
    /* A view of it asking for nothing more keeps the guard. */
    if (!view_of(&g, 0, &both) || !guards_pkg(&both))
        return false;
    CHECK_ST(jam_handle_close(both.fs), OK);
    /* bin/pkg's (`:wp`): no etc, but pkg is its to change. */
    if (!view_of(&r, FS_VIEW_GUARD_ETC | FS_VIEW_NO_ETC, &w))
        return false;
    CHECK(put_file(&w, "/pkg/bin/vim.new", "v2"));
    CHECK_ST(t_unlink(&w, "/pkg/bin/vim"), OK);
    CHECK_ST(t_rename(&w, "/pkg/bin/vim.new", "/pkg/bin/vim"), OK);
    CHECK(file_is(&g, "/pkg/bin/vim", "v2"));
    CHECK_ST(t_mkdir(&w, "/etc"), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(w.fs), OK);
    CHECK_ST(jam_handle_close(g.fs), OK);
    return fat_stop(&r);
}

bool t_fat_view_keys(void)
{
    struct fatrun r, h, ro, again;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "a"));
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK(put_file(&r, "/etc/allow", "x"));
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/ssh/.ed25519.sk", "secret"));
    /* The short name FAT made for it opens it on the whole channel. */
    CHECK(file_is(&r, "/etc/ssh/ED2551~1.SK", "secret"));
    if (!view_of(&r, FS_VIEW_HIDE_KEYS, &h) || !hides_keys(&h))
        return false;
    if (!view_of(&r, FS_VIEW_READ_ONLY | FS_VIEW_HIDE_KEYS, &ro) || !hides_keys(&ro) ||
        !refuses_changes(&ro))
        return false;
    if (!view_of(&h, 0, &again) || !hides_keys(&again))   /* never wider */
        return false;
    CHECK_ST(jam_handle_close(again.fs), OK);
    /* A read-only view without it (sshd's `:rk`) reads the key. */
    if (!view_of(&r, FS_VIEW_READ_ONLY | FS_VIEW_HIDE_ETC, &again) ||
        !file_is(&again, "/etc/ssh/.ed25519.sk", "secret"))
        return false;
    CHECK_ST(jam_handle_close(again.fs), OK);
    CHECK_ST(jam_handle_close(h.fs), OK);
    CHECK_ST(jam_handle_close(ro.fs), OK);
    return fat_stop(&r);
}

bool t_view_hides_names(void)
{
    const uint32_t keys = FS_VIEW_HIDE_KEYS, etc = FS_VIEW_HIDE_ETC, auth = FS_VIEW_HIDE_AUTH;
    const uint32_t all = FS_VIEW_NO_ETC, rk = etc, ra = etc | keys;
    /* etc itself: a stat answers unless all of it is hidden. */
    static const char *const top[] = { "etc", "ETC", "Etc", "etc.", " etc", "etc .." };
    for (unsigned i = 0; i < sizeof(top) / sizeof(top[0]); i++) {
        CHECK(fs_view_hides(all, top[i], true) && fs_view_hides(all, top[i], false));
        CHECK(!fs_view_hides(rk, top[i], true) && fs_view_hides(rk, top[i], false));
        CHECK(!fs_view_hides(ra, top[i], true) && fs_view_hides(ra, top[i], false));
        CHECK(!fs_view_hides(keys, top[i], true) && !fs_view_hides(keys, top[i], false));
    }
    /* The rest of etc: every name but ssh and authorized_keys, the short
     * name of authorized_keys among them. */
    static const char *const rest[] = { "etc/allow", "ETC/settings", "Etc/history",
                                        "etc/allow.d/x", "etc./x", "etc/AUTHOR~1",
                                        "etc/authorized_keys/x", "etc/authorized_keysx",
                                        "etc/sshd", "etc/x/../allow" };
    for (unsigned i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
        CHECK(fs_view_hides(all, rest[i], true) && fs_view_hides(rk, rest[i], false));
        CHECK(fs_view_hides(ra, rest[i], false) && fs_view_hides(auth, rest[i], false));
        CHECK(!fs_view_hides(keys, rest[i], false));
    }
    static const char *const ssh[] = { "etc/ssh", "etc/ssh/.ed25519.sk", "ETC/SSH./x",
                                       " etc/ ssh/ED2551~1.SK" };
    for (unsigned i = 0; i < sizeof(ssh) / sizeof(ssh[0]); i++) {
        CHECK(fs_view_hides(all, ssh[i], true) && fs_view_hides(ra, ssh[i], false));
        CHECK(fs_view_hides(keys, ssh[i], false) && !fs_view_hides(rk, ssh[i], true));
        CHECK(!fs_view_hides(rk, ssh[i], false));
    }
    static const char *const ak[] = { "etc/authorized_keys", "ETC/AUTHORIZED_KEYS",
                                      "etc./authorized_keys. " };
    for (unsigned i = 0; i < sizeof(ak) / sizeof(ak[0]); i++) {
        CHECK(fs_view_hides(all, ak[i], true) && fs_view_hides(auth, ak[i], false));
        CHECK(!fs_view_hides(rk, ak[i], false) && !fs_view_hides(ra, ak[i], true));
        CHECK(!fs_view_hides(keys, ak[i], false));
    }
    static const char *const out[] = { "", "a.txt", "etcx", "x/etc", "x/etc/allow",
                                       "etc2/allow", "e.tc" };
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); i++)
        CHECK(!fs_view_hides(all, out[i], true) && !fs_view_hides(all, out[i], false));
    /* The other flags hide nothing. */
    CHECK(!fs_view_hides(FS_VIEW_READ_ONLY | FS_VIEW_GUARD_ETC, "etc/allow", false));
    CHECK(!fs_view_hides(0, "etc/ssh/x", false));
    return true;
}

#define ROOT_FILES 300   /* the root's listing in two batches */

/* A view where etc doesn't exist (FS_VIEW_NO_ETC): no request at or under
 * it however it is spelled, a stat of it neither; the root's listing (its
 * batches in turn) without it; the rest of the volume as v's other flags
 * say (writes: it may write). */
static bool no_etc(const struct fatrun *v, bool writes)
{
    struct tfile f;
    static struct tbatch b;
    static const char *const etc[] = { "/etc", "/ETC", "/Etc", "/etc.", "/x/../etc", "/./etc",
                                       "/etc/allow", "/ETC/settings", "/Etc/history",
                                       "/etc/authorized_keys", "/etc/AUTHOR~1", "/etc/ssh",
                                       "/etc/ssh/.ed25519.sk", "/etc/x/../allow", "/Etc/new" };
    for (unsigned i = 0; i < sizeof(etc) / sizeof(etc[0]); i++) {
        CHECK_ST(t_stat(v, etc[i], NULL, NULL, NULL), ERR_ACCESS_DENIED);
        CHECK_ST(t_open(v, etc[i], FS_READ, &f), ERR_ACCESS_DENIED);
        CHECK_ST(t_open(v, etc[i], FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
        CHECK_ST(t_batch(v, etc[i], 0, &b), ERR_ACCESS_DENIED);
        CHECK_ST(t_mkdir(v, etc[i]), ERR_ACCESS_DENIED);
        CHECK_ST(t_unlink_flags(v, etc[i], 0), ERR_ACCESS_DENIED);
        CHECK_ST(t_unlink_flags(v, etc[i], FS_UNLINK_DIR), ERR_ACCESS_DENIED);
    }
    CHECK_ST(t_rename(v, "/etc", "/old"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/etc/allow", "/allow"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(v, "/a.txt", "/ETC/a.txt"), ERR_ACCESS_DENIED);
    unsigned count = 0;
    bool found = true;
    CHECK(dir_count(v, "/", "etc", &count, &found));
    CHECK(!found);
    CHECK_EQ(count, ROOT_FILES + 1);   /* a.txt and the f files */
    CHECK(file_is(v, "/a.txt", "a"));
    if (writes) {   /* everywhere else it may write */
        CHECK(put_file(v, "/etcetera.txt", "y"));
        CHECK_ST(t_unlink(v, "/etcetera.txt"), OK);
    }
    return true;
}

bool t_fat_view_no_etc(void)
{
    struct fatrun r, ro, w, again, rk, ra;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK(put_file(&r, "/a.txt", "a"));
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK(put_file(&r, "/etc/allow", "x"));
    CHECK(put_file(&r, "/etc/settings", "s"));
    CHECK(put_file(&r, "/etc/authorized_keys", "k"));
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/ssh/.ed25519.sk", "secret"));
    char name[64];
    for (unsigned i = 0; i < ROOT_FILES; i++) {
        snprintf(name, sizeof(name), "/F%03uABCD.TXT", i);   /* one entry each: FAT16's root */
        CHECK(put_file(&r, name, "f"));
    }
    /* The short name FAT made for authorized_keys opens it on the whole
     * channel, and the whole channel lists etc. */
    CHECK(file_is(&r, "/etc/AUTHOR~1", "k"));
    unsigned count = 0;
    bool found = false;
    CHECK(dir_count(&r, "/", "etc", &count, &found));
    CHECK(found);
    CHECK_EQ(count, ROOT_FILES + 2);
    /* Every program's: `:r` and `:w`. */
    if (!view_of(&r, FS_VIEW_READ_ONLY | FS_VIEW_NO_ETC, &ro) || !no_etc(&ro, false) ||
        !refuses_changes(&ro))
        return false;
    if (!view_of(&r, FS_VIEW_GUARD_ETC | FS_VIEW_NO_ETC, &w) || !no_etc(&w, true))
        return false;
    if (!view_of(&w, 0, &again) || !no_etc(&again, true))   /* never wider */
        return false;
    CHECK_ST(jam_handle_close(again.fs), OK);
    /* The SSH server's (`:rk`): its two files, by their own names only;
     * etc stats and is listed in the root, but its own listing is refused. */
    if (!view_of(&r, FS_VIEW_READ_ONLY | FS_VIEW_HIDE_ETC, &rk))
        return false;
    CHECK(file_is(&rk, "/etc/authorized_keys", "k"));
    CHECK(file_is(&rk, "/ETC/Authorized_Keys", "k"));
    CHECK(file_is(&rk, "/etc/ssh/.ed25519.sk", "secret"));
    struct tfile f;
    static struct tbatch b;
    CHECK_ST(t_open(&rk, "/etc/AUTHOR~1", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&rk, "/etc/allow", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&rk, "/etc/settings", FS_READ, &f), ERR_ACCESS_DENIED);
    bool dir = false;
    CHECK_ST(t_stat(&rk, "/etc", NULL, &dir, NULL), OK);
    CHECK(dir);
    CHECK_ST(t_batch(&rk, "/etc", 0, &b), ERR_ACCESS_DENIED);
    CHECK_ST(t_batch(&rk, "/etc/ssh", 0, &b), OK);
    CHECK(dir_count(&rk, "/", "etc", &count, &found));
    CHECK(found);
    /* The key check's (`:ra`): authorized_keys alone; nor can a view of it
     * show the host key again. */
    if (!view_of(&rk, FS_VIEW_HIDE_KEYS, &ra))
        return false;
    CHECK(file_is(&ra, "/etc/authorized_keys", "k"));
    CHECK_ST(t_open(&ra, "/etc/ssh/.ed25519.sk", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&ra, "/etc/allow", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_stat(&ra, "/etc", NULL, NULL, NULL), OK);
    if (!view_of(&ra, 0, &again))
        return false;
    CHECK_ST(t_open(&again, "/etc/ssh/.ed25519.sk", FS_READ, &f), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(again.fs), OK);
    CHECK_ST(jam_handle_close(ra.fs), OK);
    CHECK_ST(jam_handle_close(rk.fs), OK);
    CHECK_ST(jam_handle_close(w.fs), OK);
    CHECK_ST(jam_handle_close(ro.fs), OK);
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
            jam_sleep_until(now() + 10 * NS_PER_MS);
    }
    CHECK_ST(st, OK);
    CHECK(fat_stop(&r));
    uint64_t total = 0;
    CHECK_ST(fs_statfs_until(v[1], now() + FAT_CALL_NS, &total, NULL, NULL, NULL, NULL, NULL, NULL),
             ERR_PEER_CLOSED);
    for (unsigned i = 0; i < MAX; i++)
        CHECK_ST(jam_handle_close(v[i]), OK);
    return true;
}
