/* utest: extfs's directory channels (fs.opendir, rename_at, link_at;
 * user/services/extfs/dirs.c, docs/DIRHANDLE-PLAN.md's DE1), one process
 * (and its children) speaking the protocol to an extfs over a RAM disk.
 *
 * extfs_dirs: a channel's paths start at its directory ("" itself, '/'
 * from the view's root), ".." up to the view's root and ERR_LINK_OUT above
 * it, opendir's flags and answers, fs.resolve's place found by climbing (a
 * renamed directory followed), links inside and out, view, subview and
 * watch refused; DeleteDeep's tree: 26 levels of 255-byte names made,
 * listed and removed through channels (no path over 300 bytes sent but
 * one of 4,095), resolve's ERR_NAME_TOO_LONG past 4,095 bytes, and a
 * removed directory's channel.
 *
 * extfs_dirs_views: each view flag through a channel (read-only, the etc
 * guard, the keys hidden, no etc: the root's listing through ".."), a
 * channel opened from a channel keeping the narrower view; a subview's
 * channel cut off while its directory lies above the root, a keys-hidden
 * view's while it lies in etc/ssh; a subview whose root is moved judged
 * where it lies now (DE0's stale root); rename_at and link_at (another
 * directory, one the view doesn't reach, a removed one, a file's id, a
 * hidden one, a `to` from the view's root, a directory moved by one).
 *
 * extfs_dirs_limits: 64 channels from one process and the 65th refused,
 * three children's 64 each handed to us (256), a closed channel's slot
 * taken again at once, and a process with one refused its second.
 *
 * extfs_dirs_gone (DE2, the plan's 3.3): a held directory removed (rmdir
 * through the mount's channel and through its parent's, a rename over it):
 * its channel answers "" and "." (no links), sync and statfs, opendir of
 * "" (a second channel on it), ".." its old parent (a name made there, the
 * root above it) and nothing else; one made again at its name is another
 * directory, and no new one takes its inode; ".." from one whose parent
 * went too (ERR_NOT_FOUND) or is removed and held (it answers as a removed
 * one, its ".." on); its last channel closed, its block is free again; a
 * directory moved below itself by rename_at refused; one left held at
 * the stop goes then (the saved disk's e2fsck).
 *
 * extfs_restart_dirs: across restarts (utest's supervisor, extfs_restart.c):
 * a channel carried on (its view's flags too), and one whose reply died
 * made again.
 *
 * extfs_restart_gone (DE2): held directories removed with extfs ended at
 * each point of the removal (its ".." recorded, the journal's commit, the
 * slot's, the answer; an rmdir and a rename over one): each kept across the
 * restarts, its channel and ".." as before; then a fresh start: every
 * channel closed (ERR_PEER_CLOSED, the request in progress's too) and the
 * removed directories dropped by its mount. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <fsview.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define SHARE    64u    /* extfs's XF_DIRS_PROC */
#define CHANNELS 256u   /* ... and XF_DIRS */
#define S_DIR    0040000u
#define S_TYPE   0170000u

static struct ramdisk dd;

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static uint32_t L(const char *s)
{
    return (uint32_t)strlen(s);
}

static struct fatrun on(handle_t ch)
{
    return (struct fatrun){ .fs = ch };
}

/* fs.opendir of path on ch: the channel in *out (HANDLE_INVALID on an
 * error), its id in *id (may be NULL). */
static status_t od(handle_t ch, const char *path, uint32_t flags, handle_t *out, uint64_t *id)
{
    uint64_t i = 0, v = 0;
    *out = HANDLE_INVALID;
    status_t st = fs_opendir_until(ch, dl(), path, L(path), flags, out, &i, &v);
    if (id)
        *id = i;
    return st;
}

static status_t mk(handle_t ch, const char *path)
{
    return fs_mkdir_until(ch, dl(), path, L(path), 0755);
}

static status_t mode_of(handle_t ch, const char *path, uint32_t *mode)
{
    return xmode(ch, path, 0, mode, NULL);
}

static bool dir_at(handle_t ch, const char *path)
{
    uint32_t mode = 0;
    CHECK_ST(mode_of(ch, path, &mode), OK);
    CHECK_EQ(mode & S_TYPE, S_DIR);
    return true;
}

/* fs.resolve of path on ch: *kind, and where it goes on in out
 * (FS_PATH_LEN + 1 bytes). */
static status_t rs(handle_t ch, const char *path, uint8_t *kind, char *out)
{
    uint16_t used = 0;
    uint32_t n = 0;
    status_t st = fs_resolve_until(ch, dl(), path, L(path), 0, kind, &used, out, &n);
    out[st == OK ? n : 0] = '\0';
    return st;
}

static char got[FS_PATH_LEN + 1];

/* path on ch resolves inside the view, at want (its place below the
 * view's root). */
static bool inside_is(handle_t ch, const char *path, const char *want)
{
    uint8_t kind = 0;
    CHECK_ST(rs(ch, path, &kind, got), OK);
    CHECK_EQ(kind, FS_RESOLVE_INSIDE);
    if (strcmp(got, want))
        FAIL("\"%s\" is at \"%s\", not \"%s\"", path, got, want);
    return true;
}

static void close_all(handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (hs[i] != HANDLE_INVALID)
            jam_handle_close(hs[i]);
}

static bool start(struct fatrun *r)
{
    CHECK(ramdisk_create(&dd, EXT_DISK_SECTORS));
    CHECK(extfs_start(r, &dd, false, NULL, 0));
    return true;
}

static bool stop(struct fatrun *r, const char *name)
{
    CHECK(fat_stop(r));
    CHECK(extfs_save(&dd, name));
    return ramdisk_destroy(&dd);
}

/* ---- paths from a directory ---------------------------------------------------------- */

#define LEVELS 26u
#define NM     255u   /* the longest name */

static char name[NM + 1];
static char longest[FS_PATH_LEN + 1];
static struct tbatch batch;

/* DeleteDeep's tree under /deep (base's files_util tests): LEVELS
 * directories of 255-byte names, each made, opened and listed through its
 * parent's channel, a file at the bottom, then removed from the bottom. */
static bool deep(struct fatrun *r)
{
    handle_t ch[LEVELS + 1];
    unsigned count = 0;
    bool found = false;
    uint8_t kind = 0;
    struct tfile f;
    memset(name, 'd', NM);
    name[NM] = '\0';
    CHECK_ST(t_mkdir(r, "/deep"), OK);
    CHECK_ST(od(r->fs, "/deep", FS_READ, &ch[0], NULL), OK);
    for (unsigned i = 0; i < LEVELS; i++) {
        CHECK_ST(mk(ch[i], name), OK);
        CHECK_ST(od(ch[i], name, FS_READ, &ch[i + 1], NULL), OK);
        struct fatrun p = on(ch[i]);
        CHECK(dir_count(&p, "", name, &count, &found));
        CHECK(count == 1 && found);
    }
    struct fatrun bottom = on(ch[LEVELS]);
    CHECK(put_file(&bottom, "f", "deep"));
    CHECK(file_is(&bottom, "f", "deep"));
    CHECK(dir_at(ch[LEVELS], "../../.."));

    /* A relative path of 4,095 bytes: the 16 names below level 10. */
    size_t at = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (i)
            longest[at++] = '/';
        memcpy(longest + at, name, NM);
        at += NM;
    }
    longest[at] = '\0';
    CHECK_EQ(at, FS_PATH_LEN);
    CHECK(dir_at(ch[10], longest));

    /* resolve's place, named by climbing: "deep" and 15 names fit 4,095
     * bytes, 16 don't. */
    CHECK_ST(rs(ch[15], "", &kind, got), OK);
    CHECK_EQ(kind, FS_RESOLVE_INSIDE);
    CHECK_EQ(strlen(got), 4 + 15 * (NM + 1));
    CHECK(!strncmp(got, "deep/", 5) && !strcmp(got + strlen(got) - NM, name));
    CHECK_ST(rs(ch[16], "", &kind, got), ERR_NAME_TOO_LONG);
    CHECK_ST(rs(ch[16], "..", &kind, got), OK);   /* level 15 */

    /* Removed from the bottom, each through its parent's channel; the
     * bottom one while its channel is open: nothing more is made in it. */
    CHECK_ST(t_unlink_flags(&bottom, "f", 0), OK);
    CHECK_ST(fs_unlink_until(ch[LEVELS - 1], dl(), name, NM, FS_UNLINK_DIR), OK);
    CHECK_ST(mk(ch[LEVELS], "x"), ERR_NOT_FOUND);
    CHECK_ST(t_open(&bottom, "g", FS_WRITE | FS_CREATE, &f), ERR_NOT_FOUND);
    CHECK_ST(t_batch(&bottom, "", 0, &batch), ERR_NOT_FOUND);
    for (unsigned i = LEVELS; i > 0; i--) {
        jam_handle_close(ch[i]);
        if (i < LEVELS)
            CHECK_ST(fs_unlink_until(ch[i - 1], dl(), name, NM, FS_UNLINK_DIR), OK);
    }
    struct fatrun top = on(ch[0]);
    CHECK(dir_count(&top, "", NULL, &count, NULL));
    CHECK_EQ(count, 0u);
    jam_handle_close(ch[0]);
    CHECK_ST(t_unlink(r, "/deep"), OK);
    return true;
}

bool t_extfs_dirs(void)
{
    struct fatrun r;
    handle_t a, a2, b, h;
    uint64_t id = 0, id2 = 0, vol1 = 0, vol2 = 0;
    uint8_t kind = 0;
    CHECK(start(&r));
    CHECK_ST(t_mkdir(&r, "/a"), OK);
    CHECK_ST(t_mkdir(&r, "/a/sub"), OK);
    CHECK(put_file(&r, "/a/f", "f"));
    CHECK_ST(fs_symlink_until(r.fs, dl(), "a", 1, "/la", 3), OK);

    /* opendir's answers: the id getattr gives, one volume for both. */
    CHECK_ST(fs_opendir_until(r.fs, dl(), "/a", 2, FS_READ, &a, &id, &vol1), OK);
    CHECK_ST(t_stat_id(&r, "/a", &id2), OK);
    CHECK_EQ(id, id2);
    CHECK_ST(fs_opendir_until(r.fs, dl(), "la", 2, 0, &a2, &id2, &vol2), OK);   /* followed */
    CHECK(id2 == id && vol2 == vol1 && vol1);
    jam_handle_close(a2);
    CHECK_ST(od(r.fs, "/la", FS_NOFOLLOW, &h, NULL), ERR_INVALID_ARGS);
    CHECK_ST(od(r.fs, "/a/f", 0, &h, NULL), ERR_NOT_DIR);
    CHECK_ST(od(r.fs, "/a/f/", 0, &h, NULL), ERR_NOT_DIR);
    CHECK_ST(od(r.fs, "/none", 0, &h, NULL), ERR_NOT_FOUND);
    CHECK_ST(od(r.fs, "/a", FS_WRITE, &h, NULL), ERR_INVALID_ARGS);
    CHECK(h == HANDLE_INVALID);

    /* Its paths: "" and "." the directory, a name in it, '/' from the root. */
    struct fatrun ar = on(a);
    CHECK(dir_at(a, ""));
    CHECK(dir_at(a, "."));
    CHECK(dir_at(a, "sub"));
    CHECK(file_is(&ar, "f", "f"));
    CHECK(file_is(&ar, "/a/f", "f"));
    CHECK(put_file(&ar, "g", "g"));
    CHECK(file_is(&r, "/a/g", "g"));
    unsigned count = 0;
    bool found = false;
    CHECK(dir_count(&ar, "", "g", &count, &found));
    CHECK(count == 3 && found);

    /* ".." to the view's root and no further. */
    CHECK(dir_at(a, ".."));
    CHECK(dir_at(a, "sub/../.."));
    CHECK_ST(mode_of(a, "../..", &(uint32_t){ 0 }), ERR_LINK_OUT);
    CHECK_ST(rs(a, "../../x/y", &kind, got), OK);
    CHECK(kind == FS_RESOLVE_ABOVE && !strcmp(got, "x/y"));
    CHECK(inside_is(a, "", "a"));
    CHECK(inside_is(a, "sub", "a/sub"));
    CHECK(inside_is(a, "..", ""));
    CHECK(inside_is(a, "/la/sub", "a/sub"));

    /* Links inside followed, an absolute one ERR_LINK_OUT. */
    CHECK_ST(fs_symlink_until(a, dl(), "f", 1, "in", 2), OK);
    CHECK_ST(fs_symlink_until(a, dl(), "../a/sub", 8, "up", 2), OK);
    CHECK_ST(fs_symlink_until(a, dl(), "/elsewhere", 10, "out", 3), OK);
    CHECK(file_is(&ar, "in", "f"));
    CHECK(dir_at(a, "up"));
    CHECK(inside_is(a, "up", "a/sub"));
    CHECK_ST(mode_of(a, "out/x", &(uint32_t){ 0 }), ERR_LINK_OUT);
    CHECK_ST(rs(a, "out/x", &kind, got), OK);
    CHECK(kind == FS_RESOLVE_ABSOLUTE && !strcmp(got, "/elsewhere/x"));

    /* Renamed (it, then a directory above it): the channel follows. */
    CHECK_ST(t_rename(&r, "/a", "/b"), OK);
    CHECK(file_is(&ar, "g", "g"));
    CHECK(inside_is(a, "", "b"));
    CHECK_ST(t_mkdir(&r, "/top"), OK);
    CHECK_ST(od(a, "sub", 0, &b, NULL), OK);
    CHECK_ST(t_rename(&r, "/b", "/top/b"), OK);
    CHECK(inside_is(b, "", "top/b/sub"));
    CHECK(inside_is(b, "../..", "top"));
    CHECK(dir_at(b, "../../.."));
    CHECK_ST(mode_of(b, "../../../..", &(uint32_t){ 0 }), ERR_LINK_OUT);
    CHECK_ST(t_rename(&r, "/top/b", "/a"), OK);
    jam_handle_close(b);

    /* view, subview and watch: on the mount's channel only. */
    CHECK_ST(fs_view_until(a, dl(), FS_VIEW_READ_ONLY, &h), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_subview_until(a, dl(), "sub", 3, 0, &h), ERR_NOT_SUPPORTED);
    CHECK_ST(fs_watch_until(a, dl(), &h), ERR_NOT_SUPPORTED);
    jam_handle_close(a);

    CHECK(deep(&r));
    return stop(&r, "dirs");
}

/* ---- views ---------------------------------------------------------------------------- */

bool t_extfs_dirs_views(void)
{
    struct fatrun r;
    handle_t v, k, s, d, e, x, y, dp, dq, dpi, kp;
    uint64_t id_p = 0, id_q = 0, id_a = 0, id_gone = 0, id_f = 0, id_ssh = 0, total, fr;
    uint32_t mode = 0, nlink = 0, magic, name_max, feat;
    uint8_t ro = 0, label[16];
    unsigned count = 0;
    bool found = false;
    CHECK(start(&r));
    CHECK_ST(t_mkdir(&r, "/etc"), OK);
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/ssh/key", "k"));
    CHECK_ST(t_mkdir(&r, "/a"), OK);
    CHECK_ST(t_mkdir(&r, "/a/sub"), OK);
    CHECK(put_file(&r, "/a/f", "f"));

    /* Read-only: nothing changes through its channels, nor through one
     * opened from one of them. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_READ_ONLY, &v), OK);
    CHECK_ST(od(v, "/a", FS_READ, &d, NULL), OK);
    CHECK_ST(mk(d, "x"), ERR_ACCESS_DENIED);
    CHECK(file_is(&(struct fatrun){ .fs = d }, "f", "f"));
    CHECK_ST(od(d, "sub", 0, &e, NULL), OK);
    jam_handle_close(d);
    jam_handle_close(v);
    CHECK_ST(mk(e, "y"), ERR_ACCESS_DENIED);
    CHECK_ST(mode_of(e, "", &mode), OK);
    CHECK(!(mode & 0222u));
    CHECK_ST(fs_statfs_until(e, dl(), &total, &fr, &ro, label, &magic, &name_max, &feat), OK);
    CHECK(ro && (feat & FS_FEAT_DIRS));
    jam_handle_close(e);

    /* The etc guard where a path resolves: etc's channel, ".." into etc
     * from another; a relative "etc" elsewhere is not the volume's. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_ETC, &v), OK);
    CHECK_ST(od(v, "/etc", 0, &d, NULL), OK);
    CHECK_ST(mk(d, "x"), ERR_ACCESS_DENIED);
    CHECK_ST(od(v, "/a", 0, &e, NULL), OK);
    CHECK_ST(mk(e, "../etc/y"), ERR_ACCESS_DENIED);
    CHECK_ST(mk(e, "etc"), OK);
    CHECK_ST(mk(e, "etc/z"), OK);
    close_all((handle_t[]){ d, e, v }, 3);
    CHECK_ST(t_mkdir(&r, "/pkg"), OK);   /* pkg's the same */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_PKG, &v), OK);
    CHECK_ST(od(v, "/pkg", 0, &d, NULL), OK);
    CHECK_ST(mk(d, "x"), ERR_ACCESS_DENIED);
    CHECK_ST(od(v, "/a", 0, &e, NULL), OK);
    CHECK_ST(mk(e, "../pkg/y"), ERR_ACCESS_DENIED);
    CHECK_ST(mk(e, "pkg"), OK);
    close_all((handle_t[]){ d, e, v }, 3);

    /* The keys hidden: etc/ssh from etc's channel. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_HIDE_KEYS, &k), OK);
    CHECK_ST(od(k, "/etc", 0, &d, NULL), OK);
    CHECK_ST(mode_of(d, "ssh", &mode), ERR_ACCESS_DENIED);
    CHECK_ST(mode_of(d, "ssh/key", &mode), ERR_ACCESS_DENIED);
    CHECK_ST(od(d, "ssh", 0, &e, NULL), ERR_ACCESS_DENIED);
    CHECK(dir_at(d, "../a"));
    jam_handle_close(d);

    /* No etc: the root's listing through ".." leaves it out. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_NO_ETC, &v), OK);
    CHECK_ST(od(v, "/a", 0, &d, NULL), OK);
    struct fatrun dr = on(d);
    CHECK(dir_count(&dr, "..", "etc", &count, &found));
    CHECK(!found);
    CHECK(dir_count(&dr, "..", "a", &count, &found));
    CHECK(found);
    CHECK_ST(mode_of(d, "../etc", &mode), ERR_ACCESS_DENIED);
    CHECK_ST(od(v, "/etc", 0, &e, NULL), ERR_ACCESS_DENIED);
    close_all((handle_t[]){ d, v }, 2);

    /* A subview: ".." stops at its root; a directory channel's "/" is it. */
    CHECK_ST(fs_subview_until(r.fs, dl(), "/a", 2, 0, &s), OK);
    CHECK_ST(od(s, "/sub", 0, &d, NULL), OK);
    CHECK(inside_is(d, "", "sub"));
    CHECK(inside_is(d, "..", ""));
    CHECK(inside_is(d, "/", ""));
    CHECK_ST(mode_of(d, "../..", &mode), ERR_LINK_OUT);
    CHECK(file_is(&(struct fatrun){ .fs = d }, "../f", "f"));

    /* Moved above the subview's root, it is cut off; back, it works. */
    CHECK_ST(t_rename(&r, "/a/sub", "/sub2"), OK);
    CHECK_ST(mode_of(d, "", &mode), ERR_NOT_FOUND);
    CHECK_ST(mk(d, "x"), ERR_NOT_FOUND);
    CHECK_ST(mode_of(d, "/", &mode), ERR_NOT_FOUND);
    CHECK_ST(fs_sync_until(d, dl()), ERR_NOT_FOUND);
    CHECK_ST(t_rename(&r, "/sub2", "/a/sub"), OK);
    CHECK(dir_at(d, ""));
    CHECK(inside_is(d, "", "sub"));

    /* Moved where the view hides it (into etc/ssh, the keys hidden): the
     * same. */
    CHECK_ST(t_mkdir(&r, "/b"), OK);
    CHECK_ST(od(k, "/b", 0, &e, NULL), OK);
    CHECK_ST(t_rename(&r, "/b", "/etc/ssh/b"), OK);
    CHECK_ST(mode_of(e, "", &mode), ERR_NOT_FOUND);
    CHECK_ST(t_rename(&r, "/etc/ssh/b", "/b"), OK);
    CHECK(dir_at(e, ""));
    jam_handle_close(e);

    /* A subview's root moved: judged where it lies now (DE0's renamed
     * root): guarded in etc, hidden in etc/ssh. */
    CHECK_ST(t_mkdir(&r, "/gr"), OK);
    CHECK_ST(fs_subview_until(r.fs, dl(), "/gr", 3, FS_VIEW_GUARD_ETC, &x), OK);
    CHECK_ST(mk(x, "/m"), OK);
    CHECK_ST(t_rename(&r, "/gr", "/etc/gr"), OK);
    CHECK_ST(mk(x, "/n"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&r, "/etc/gr", "/gr"), OK);
    CHECK_ST(mk(x, "/n"), OK);
    CHECK_ST(fs_subview_until(r.fs, dl(), "/b", 2, FS_VIEW_HIDE_KEYS, &y), OK);
    CHECK(dir_at(y, "/"));
    CHECK_ST(t_rename(&r, "/b", "/etc/ssh/b"), OK);
    CHECK_ST(mode_of(y, "/", &mode), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&r, "/etc/ssh/b", "/b"), OK);
    CHECK(dir_at(y, "/"));
    close_all((handle_t[]){ x, y }, 2);

    /* rename_at and link_at: two directories by channel and id. */
    CHECK_ST(t_mkdir(&r, "/p"), OK);
    CHECK_ST(t_mkdir(&r, "/q"), OK);
    CHECK_ST(t_mkdir(&r, "/p/in"), OK);
    CHECK(put_file(&r, "/p/f", "pf"));
    CHECK_ST(od(r.fs, "/p", 0, &dp, &id_p), OK);
    CHECK_ST(od(r.fs, "/q", 0, &dq, &id_q), OK);
    CHECK_ST(od(r.fs, "/p/in", 0, &dpi, NULL), OK);
    CHECK_ST(fs_rename_at_until(dp, dl(), "f", 1, id_q, "g", 1, 0), OK);
    CHECK(file_is(&r, "/q/g", "pf"));
    CHECK_ST(fs_link_at_until(dq, dl(), "g", 1, id_p, "h", 1), OK);
    CHECK_ST(xmode(r.fs, "/p/h", 0, &mode, &nlink), OK);
    CHECK_EQ(nlink, 2u);
    CHECK_ST(fs_rename_at_until(dq, dl(), "g", 1, id_p, "/q/g2", 5, 0), OK);   /* from the root */
    CHECK(file_is(&r, "/q/g2", "pf"));
    CHECK_ST(fs_rename_at_until(dp, dl(), "in", 2, id_q, "in", 2, 0), OK);   /* a directory */
    CHECK(inside_is(dpi, "", "q/in"));

    /* A directory the view doesn't reach (outside the subview), its own
     * root by id, a removed one, a file's id, none, one the view hides. */
    CHECK(put_file(&(struct fatrun){ .fs = d }, "t", "t"));
    CHECK_ST(fs_rename_at_until(d, dl(), "t", 1, id_q, "t", 1, 0), ERR_NOT_FOUND);
    CHECK_ST(fs_link_at_until(d, dl(), "t", 1, id_q, "t", 1), ERR_NOT_FOUND);
    CHECK_ST(t_stat_id(&r, "/a", &id_a), OK);
    CHECK_ST(fs_rename_at_until(d, dl(), "t", 1, id_a, "t2", 2, 0), OK);
    CHECK(file_is(&r, "/a/t2", "t"));
    CHECK_ST(t_mkdir(&r, "/gone"), OK);
    CHECK_ST(t_stat_id(&r, "/gone", &id_gone), OK);
    CHECK_ST(t_unlink(&r, "/gone"), OK);
    CHECK_ST(fs_rename_at_until(dq, dl(), "g2", 2, id_gone, "x", 1, 0), ERR_NOT_FOUND);
    CHECK_ST(t_stat_id(&r, "/q/g2", &id_f), OK);
    CHECK_ST(fs_rename_at_until(dq, dl(), "g2", 2, id_f, "x", 1, 0), ERR_NOT_FOUND);
    CHECK_ST(fs_link_at_until(dq, dl(), "g2", 2, 0, "x", 1), ERR_NOT_FOUND);
    CHECK_ST(fs_link_at_until(dq, dl(), "g2", 2, 1ull << 40, "x", 1), ERR_NOT_FOUND);
    CHECK_ST(t_stat_id(&r, "/etc/ssh", &id_ssh), OK);
    CHECK_ST(od(k, "/p", 0, &kp, NULL), OK);
    CHECK_ST(fs_link_at_until(kp, dl(), "h", 1, id_ssh, "h", 1), ERR_NOT_FOUND);
    CHECK(file_is(&r, "/q/g2", "pf"));
    close_all((handle_t[]){ d, s, k, dp, dq, dpi, kp }, 7);
    return stop(&r, "dirs-views");
}

/* ---- limits -------------------------------------------------------------------------- */

/* One message on ch: its first byte into *b, its handles into hs (hcap). */
static status_t read_msg(handle_t ch, handle_t *hs, uint32_t hcap, uint32_t *nh)
{
    uint8_t b = 0;
    uint32_t n = 0;
    *nh = 0;
    status_t st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, dl(), NULL);
    if (st != OK)
        return st;
    const struct channel_read_args a = {
        .h = ch, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)&b,
        .rd_handles = (uint64_t)(uintptr_t)hs, .rd_hcap = hcap,
        .rd_actual = (uint64_t)(uintptr_t)&n, .rd_hactual = (uint64_t)(uintptr_t)nh,
    };
    return jam_channel_read(&a);
}

/* The second process (mode xdirs-share or xdirs-full, a channel to us as
 * SR_USER, on which the mount's channel comes first): share: SHARE
 * directory channels of the root and the next refused (its share), the
 * SHARE sent back to us; full: one, and the next refused (the volume's
 * full). */
int extfs_dirs_child(int argc, char **argv)
{
    const char *m = argc >= 2 ? argv[1] : "";
    handle_t back = startup_handle(SR_USER), fs = HANDLE_INVALID, more;
    static handle_t hs[SHARE];
    uint32_t nh = 0;
    if (!back || read_msg(back, &fs, 1, &nh) != OK || nh != 1)
        return 1;
    bool share = !strcmp(m, "xdirs-share");
    unsigned want = share ? SHARE : 1;
    for (unsigned i = 0; i < want; i++)
        if (od(fs, "/", 0, &hs[i], NULL) != OK)
            return 2;
    if (od(fs, "/", 0, &more, NULL) != ERR_NO_RESOURCES)
        return 3;
    for (unsigned i = 0; share && i < want; i += 16)
        if (jam_channel_write(back, "d", 1, &hs[i], 16) != OK)
            return 4;
    return 0;
}

/* Run mode with the mount's channel; share: take its SHARE channels (out). */
static bool kid(handle_t fs, const char *mode, handle_t *out)
{
    handle_t mine, theirs, dup, job, proc;
    struct process_info info;
    uint32_t nh = 0;
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child(mode, NULL, job, theirs, &proc), OK);   /* before a handle is queued in it:
                                                             * the kernel's cycle rule */
    CHECK_ST(jam_handle_duplicate(fs, RIGHT_SAME, &dup), OK);
    CHECK_ST(jam_channel_write(mine, "f", 1, &dup, 1), OK);
    for (unsigned i = 0; out && i < SHARE; i += nh) {
        CHECK_ST(read_msg(mine, &out[i], SHARE - i, &nh), OK);
        CHECK(nh);
    }
    CHECK_ST(spawn_wait(proc, FAT_CALL_NS, &info), OK);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(mine);
    if (info.exit_code)
        FAIL("%s: exit %d", mode, (int)info.exit_code);
    return true;
}

bool t_extfs_dirs_limits(void)
{
    struct fatrun r;
    static handle_t own[SHARE], kids[CHANNELS - SHARE];
    handle_t more;
    CHECK(start(&r));
    for (unsigned i = 0; i < SHARE; i++)
        CHECK_ST(od(r.fs, "/", 0, &own[i], NULL), OK);
    CHECK_ST(od(r.fs, "/", 0, &more, NULL), ERR_NO_RESOURCES);   /* our share */
    for (unsigned k = 0; k < 3; k++)
        CHECK(kid(r.fs, "xdirs-share", &kids[k * SHARE]));
    /* All 256 in use: a channel closed lets its slot go at once (its close
     * seen or not), and every channel works, a child's too. */
    jam_handle_close(own[0]);
    CHECK_ST(od(r.fs, "/", 0, &own[0], NULL), OK);
    CHECK_ST(od(r.fs, "/", 0, &more, NULL), ERR_NO_RESOURCES);
    CHECK(dir_at(kids[100], ""));
    CHECK(dir_at(own[0], ""));
    /* 255: a process with one is refused its second, the volume full. */
    jam_handle_close(own[1]);
    own[1] = HANDLE_INVALID;
    CHECK(kid(r.fs, "xdirs-full", NULL));
    close_all(own, SHARE);
    close_all(kids, CHANNELS - SHARE);
    CHECK_ST(od(r.fs, "/", 0, &more, NULL), OK);
    jam_handle_close(more);
    return stop(&r, "dirs-limits");
}

/* ---- removed directories --------------------------------------------------------------- */

/* fs.getattr of path on ch, a directory: its id and link count (either
 * may be NULL). */
static status_t dattr(handle_t ch, const char *path, uint64_t *id, uint32_t *nlink)
{
    uint32_t mode = 0, uid, gid, nl;
    uint64_t size, blocks, i;
    int64_t at, mt, ct;
    status_t st = fs_getattr_until(ch, dl(), path, L(path), 0, &mode, nlink ? nlink : &nl, &uid,
                                   &gid, &size, &blocks, id ? id : &i, &at, &mt, &ct);
    return st == OK && (mode & S_TYPE) != S_DIR ? ERR_NOT_DIR : st;
}

/* ch's directory, id `id`, removed: "" and "." it, with no links, sync and
 * statfs answered; nothing listed, made, opened, changed or resolved, no
 * other name, no '/' path. */
static bool removed_is(handle_t ch, uint64_t id)
{
    struct fatrun r = on(ch);
    struct tfile f;
    uint64_t got_id = 0, total, fr;
    uint32_t nlink = 1, magic, name_max, feat;
    uint8_t ro, label[16], kind;
    CHECK_ST(dattr(ch, "", &got_id, &nlink), OK);
    CHECK(got_id == id && nlink == 0);
    CHECK_ST(dattr(ch, ".", &got_id, NULL), OK);
    CHECK_EQ(got_id, id);
    CHECK_ST(dattr(ch, "./.", NULL, NULL), OK);
    CHECK_ST(t_stat_id(&r, "", &got_id), OK);
    CHECK_EQ(got_id, id);
    CHECK_ST(fs_sync_until(ch, dl()), OK);
    CHECK_ST(fs_statfs_until(ch, dl(), &total, &fr, &ro, label, &magic, &name_max, &feat), OK);
    CHECK_ST(t_batch(&r, "", 0, &batch), ERR_NOT_FOUND);
    CHECK_ST(mk(ch, "x"), ERR_NOT_FOUND);
    CHECK_ST(t_open(&r, "f", FS_READ | FS_WRITE | FS_CREATE, &f), ERR_NOT_FOUND);
    CHECK_ST(fs_setattr_until(ch, dl(), "", 0, 0, FS_ATTR_MODE, 0700, 0, 0, 0, 0), ERR_NOT_FOUND);
    CHECK_ST(dattr(ch, "x", NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(dattr(ch, "/", NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(rs(ch, "", &kind, got), ERR_NOT_FOUND);
    return true;
}

/* ch's ".." is directory `want`; want 0: gone (ERR_NOT_FOUND), at once or
 * once a closed channel's packet is served (asked again until then). */
static bool up_is(handle_t ch, uint64_t want)
{
    uint64_t id = 0, end = dl();
    status_t st;
    while ((st = dattr(ch, "..", &id, NULL)) == OK && !want && now() < end)
        jam_sleep_until(now() + 10 * NS_PER_MS);
    CHECK_ST(st, want ? OK : ERR_NOT_FOUND);
    if (want)
        CHECK_EQ(id, want);
    return true;
}

/* The volume's free bytes rise past `than` (as up_is: asked again). */
static bool freed_past(const struct fatrun *r, uint64_t than)
{
    uint64_t total = 0, fr = 0, end = dl();
    do {
        CHECK_ST(t_free(r, &total, &fr), OK);
        if (fr > than)
            return true;
        jam_sleep_until(now() + 10 * NS_PER_MS);
    } while (now() < end);
    FAIL("%lu bytes free, %lu before: the removed directory is still there", (unsigned long)fr,
         (unsigned long)than);
}

bool t_extfs_dirs_gone(void)
{
    struct fatrun r;
    handle_t g, g2, h, p, q, s, top;
    uint64_t id_a = 0, id_g = 0, id = 0, id_p = 0, id_q = 0, id_s = 0, id_t = 0, id_w = 0;
    uint64_t total = 0, fr = 0;
    uint32_t nlink = 0;
    char path[32];
    CHECK(start(&r));
    CHECK_ST(t_mkdir(&r, "/a"), OK);
    CHECK_ST(t_mkdir(&r, "/a/g"), OK);
    CHECK_ST(t_stat_id(&r, "/a", &id_a), OK);
    CHECK_ST(od(r.fs, "/a/g", FS_READ, &g, &id_g), OK);
    struct fatrun gr = on(g);

    /* Removed while held: its name gone, its parent's link count down; its
     * channel answers "" and "." only, its ".." the parent. */
    CHECK_ST(dattr(r.fs, "/a", NULL, &nlink), OK);
    CHECK_EQ(nlink, 3u);
    CHECK_ST(t_unlink(&r, "/a/g"), OK);
    CHECK_ST(t_stat(&r, "/a/g", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(dattr(r.fs, "/a", NULL, &nlink), OK);
    CHECK_EQ(nlink, 2u);
    CHECK(removed_is(g, id_g));
    CHECK(up_is(g, id_a));
    CHECK(inside_is(g, "..", "a"));
    CHECK(inside_is(g, "./../.", "a"));
    CHECK(put_file(&gr, "../made", "m"));
    CHECK(file_is(&r, "/a/made", "m"));
    CHECK(dir_at(g, "../.."));
    CHECK_ST(mode_of(g, "../../..", &nlink), ERR_LINK_OUT);

    /* Made again at its name: another directory, the channel staying on the
     * removed one; no directory made takes its inode while it is held. */
    CHECK_ST(t_mkdir(&r, "/a/g"), OK);
    CHECK_ST(t_stat_id(&r, "/a/g", &id), OK);
    CHECK(id != id_g);
    CHECK(put_file(&r, "/a/g/in", "in"));
    CHECK(removed_is(g, id_g));
    CHECK(file_is(&gr, "../g/in", "in"));
    for (unsigned i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/a/n%u", i);
        CHECK_ST(t_mkdir(&r, path), OK);
        CHECK_ST(t_stat_id(&r, path, &id), OK);
        CHECK(id != id_g);
    }

    /* opendir of "" on it: a second channel on the removed directory, its
     * ".." the same; nothing else opens. Its last channel closed, it goes. */
    CHECK_ST(od(g, "", 0, &g2, &id), OK);
    CHECK_EQ(id, id_g);
    CHECK_ST(od(g, "x", 0, &h, NULL), ERR_NOT_FOUND);
    CHECK_ST(od(g, "/a", 0, &h, NULL), ERR_NOT_FOUND);
    jam_handle_close(g);
    CHECK(removed_is(g2, id_g));
    CHECK(up_is(g2, id_a));
    CHECK_ST(t_free(&r, &total, &fr), OK);
    jam_handle_close(g2);
    CHECK(freed_past(&r, fr));

    /* ".." from one whose parent went too: a parent no channel held is free
     * (or another inode now): ERR_NOT_FOUND. */
    CHECK_ST(t_mkdir(&r, "/p"), OK);
    CHECK_ST(t_mkdir(&r, "/p/q"), OK);
    CHECK_ST(od(r.fs, "/p/q", 0, &q, &id_q), OK);
    CHECK_ST(t_unlink(&r, "/p/q"), OK);
    CHECK_ST(t_unlink(&r, "/p"), OK);
    CHECK(removed_is(q, id_q));
    CHECK(up_is(q, 0));
    for (unsigned i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "/m%u", i);
        CHECK_ST(t_mkdir(&r, path), OK);
    }
    CHECK(up_is(q, 0));
    CHECK_ST(dattr(q, "../..", NULL, NULL), ERR_NOT_FOUND);
    jam_handle_close(q);

    /* ... and one held, removed too: it answers as a removed one, its ".."
     * on to the root; a channel opened on it by ".." keeps it. */
    CHECK_ST(t_mkdir(&r, "/p"), OK);
    CHECK_ST(t_mkdir(&r, "/p/q"), OK);
    CHECK_ST(od(r.fs, "/p", 0, &p, &id_p), OK);
    CHECK_ST(od(p, "q", 0, &q, &id_q), OK);
    CHECK_ST(fs_unlink_until(p, dl(), "q", 1, FS_UNLINK_DIR), OK);   /* through its parent's */
    CHECK_ST(t_unlink(&r, "/p"), OK);
    CHECK(removed_is(q, id_q));
    CHECK(removed_is(p, id_p));
    CHECK(up_is(q, id_p));
    CHECK_ST(dattr(q, "..", NULL, &nlink), OK);
    CHECK_EQ(nlink, 0u);
    CHECK_ST(dattr(q, "../q", NULL, NULL), ERR_NOT_FOUND);
    CHECK(dir_at(q, "../.."));
    CHECK(inside_is(q, "../../a", "a"));
    CHECK_ST(od(q, "..", 0, &h, &id), OK);
    CHECK_EQ(id, id_p);
    jam_handle_close(p);
    CHECK(removed_is(h, id_p));
    CHECK(up_is(q, id_p));
    jam_handle_close(h);
    CHECK(up_is(q, 0));   /* p's last channel closed: it went */
    jam_handle_close(q);

    /* A rename over a held empty directory: the channel stays on the one
     * renamed over, its ".." where it was. */
    CHECK_ST(t_mkdir(&r, "/s"), OK);
    CHECK_ST(t_mkdir(&r, "/t"), OK);
    CHECK(put_file(&r, "/t/f", "t"));
    CHECK_ST(od(r.fs, "/s", 0, &s, &id_s), OK);
    CHECK_ST(t_stat_id(&r, "/t", &id_t), OK);
    CHECK_ST(t_rename_flags(&r, "/t", "/s", FS_RENAME_REPLACE), OK);
    CHECK(removed_is(s, id_s));
    CHECK(inside_is(s, "..", ""));
    struct fatrun sr = on(s);
    CHECK(file_is(&sr, "../s/f", "t"));
    CHECK_ST(t_stat_id(&r, "/s", &id), OK);
    CHECK_EQ(id, id_t);

    /* rename_at moving a directory below itself (to_dir by its id): refused
     * (ext_rename climbs from the new parent by inode). */
    CHECK_ST(t_mkdir(&r, "/v"), OK);
    CHECK_ST(t_mkdir(&r, "/v/w"), OK);
    CHECK_ST(t_stat_id(&r, "/v/w", &id_w), OK);
    CHECK_ST(od(r.fs, "/", 0, &top, NULL), OK);
    CHECK_ST(fs_rename_at_until(top, dl(), "v", 1, id_w, "v", 1, 0), ERR_INVALID_ARGS);
    jam_handle_close(top);

    /* s still held at the stop: it goes then (the saved disk's e2fsck). */
    bool ok = stop(&r, "dirs-gone");
    jam_handle_close(s);
    return ok;
}

/* ---- restarts ------------------------------------------------------------------------ */

/* extfs ended by its test powers (utest's restart supervisor): instance 0
 * after its 4th answer (an opendir: its reply never went out), instance 1
 * at its 2nd commit (an opendir). The comments count each instance's
 * requests. */
bool t_extfs_restart_dirs(void)
{
    static const char *const dies[] = { "die=answered:4", "die=commit:2" };
    handle_t v, da, dv, dx;
    uint32_t mode = 0;
    CHECK(ramdisk_create(&dd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&dd, dies, 2));
    struct fatrun *s = extfs_sup_run();
    CHECK_ST(t_mkdir(s, "/a"), OK);                                       /* 1 */
    CHECK_ST(fs_view_until(s->fs, dl(), FS_VIEW_READ_ONLY, &v), OK);     /* 2 */
    CHECK_ST(od(s->fs, "/a", 0, &da, NULL), OK);                          /* 3 */
    CHECK_ST(od(v, "/a", 0, &dv, NULL), OK);                              /* 4: made again */
    CHECK_EQ(extfs_sup_ended(), 1u);

    /* Instance 1: the channel carried on, its place found again. */
    CHECK_ST(mk(da, "x"), OK);                                            /* 1 */
    CHECK_ST(od(da, "x", 0, &dx, NULL), OK);                              /* 2: made again */
    CHECK_EQ(extfs_sup_ended(), 2u);

    /* Instance 2: both made again work, the view's flags with them. */
    CHECK_ST(mk(dv, "y"), ERR_ACCESS_DENIED);
    CHECK(dir_at(dv, "x"));
    CHECK(put_file(&(struct fatrun){ .fs = dx }, "f", "f"));
    CHECK(file_is(s, "/a/x/f", "f"));
    CHECK(inside_is(dx, "", "a/x"));
    CHECK_ST(mode_of(dx, "../..", &mode), OK);
    close_all((handle_t[]){ v, da, dv, dx }, 4);
    CHECK_EQ(extfs_sup_ended(), 2u);
    CHECK(extfs_sup_stop());
    CHECK(extfs_save(&dd, "restart-dirs"));
    return ramdisk_destroy(&dd);
}

/* Held directories removed with extfs ended at each point of the removal
 * (test.c's powers; instance n ends at dies[n]); the comments name the
 * instance each request ends. Then a fresh start. */
bool t_extfs_restart_gone(void)
{
    static const char *const dies[] = {
        "die=removed:1", "die=journal:1", "die=commit:1",  "die=answered:1",
        "die=removed:1", "die=removed:1", "die=removed:2",
    };
    handle_t ga, g[4], s;
    uint64_t id_a = 0, id_g[4], id_s = 0, id_t = 0, id = 0, total = 0, fr = 0;
    char name[4] = "g0";
    CHECK(ramdisk_create(&dd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&dd, dies, 7));
    struct fatrun *sr = extfs_sup_run();
    CHECK_ST(t_mkdir(sr, "/a"), OK);
    CHECK_ST(t_stat_id(sr, "/a", &id_a), OK);
    CHECK_ST(od(sr->fs, "/a", 0, &ga, NULL), OK);
    for (unsigned i = 0; i < 4; i++) {
        name[1] = (char)('0' + i);
        CHECK_ST(mk(ga, name), OK);
        CHECK_ST(od(ga, name, 0, &g[i], &id_g[i]), OK);
    }
    CHECK_ST(mk(ga, "s"), OK);
    CHECK_ST(mk(ga, "t"), OK);
    CHECK_ST(od(ga, "s", 0, &s, &id_s), OK);
    CHECK_ST(t_stat_id(sr, "/a/t", &id_t), OK);

    /* Instance 0 ends with g0's ".." recorded, 1 with its rmdir in the
     * journal (run again), 2 answers it from its slot; 2 ends with g1's
     * slot committed, 3 answers; 3 ends with g2's answer waiting, 4 sends
     * it; 4 and 5 end with the rename over s recorded, 6 runs it. */
    CHECK_ST(fs_unlink_until(ga, dl(), "g0", 2, FS_UNLINK_DIR), OK);
    CHECK_EQ(extfs_sup_ended(), 2u);
    CHECK_ST(fs_unlink_until(ga, dl(), "g1", 2, FS_UNLINK_DIR), OK);
    CHECK_EQ(extfs_sup_ended(), 3u);
    CHECK_ST(t_unlink_flags(sr, "/a/g2", FS_UNLINK_DIR), OK);
    CHECK_EQ(extfs_sup_ended(), 4u);
    CHECK_ST(t_rename_flags(sr, "/a/t", "/a/s", FS_RENAME_REPLACE), OK);
    CHECK_EQ(extfs_sup_ended(), 6u);

    /* Instance 6: each kept, its channel and ".." as before; s is t's now. */
    for (unsigned i = 0; i < 3; i++) {
        CHECK(removed_is(g[i], id_g[i]));
        CHECK(up_is(g[i], id_a));
    }
    CHECK(removed_is(s, id_s));
    CHECK(up_is(s, id_a));
    CHECK_ST(t_stat_id(sr, "/a/s", &id), OK);
    CHECK_EQ(id, id_t);
    CHECK_ST(t_stat(sr, "/a/g0", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK(dir_at(g[3], "../g3"));

    /* A fresh start (6 ends with g3's ".." recorded, its second removal
     * after the rename's, 7 is handed a new state): the rmdir in progress
     * and every channel closed, g3 not
     * removed, the removed ones dropped by 7's mount. */
    CHECK_ST(t_free(sr, &total, &fr), OK);
    extfs_sup_fresh();
    CHECK_ST(fs_unlink_until(ga, dl(), "g3", 2, FS_UNLINK_DIR), ERR_PEER_CLOSED);
    CHECK_EQ(extfs_sup_ended(), 7u);
    for (unsigned i = 0; i < 4; i++)
        CHECK_ST(dattr(g[i], "", NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(dattr(s, "", NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(t_stat_id(sr, "/a/g3", &id), OK);
    CHECK_EQ(id, id_g[3]);
    CHECK(freed_past(sr, fr));
    close_all((handle_t[]){ ga, g[0], g[1], g[2], g[3], s }, 6);
    CHECK(extfs_sup_stop());
    CHECK(extfs_save(&dd, "restart-gone"));
    return ramdisk_destroy(&dd);
}
