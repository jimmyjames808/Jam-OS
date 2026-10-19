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
 * removed directories dropped by its mount.
 *
 * The review's hostile runs (docs/DIRHANDLE-REVIEW.md):
 * extfs_dirs_storm: 8 threads move directories in and out of a subview
 * while 4 walk through channels held on them: never anything outside.
 * extfs_dirs_killed: a process holding its 64 channels killed after half
 * their directories were removed: the removed dropped, every slot back.
 * extfs_restart_deep: DeleteDeep's tree made and removed through channels
 * while extfs is killed every 200 ms: every call answered, every channel
 * carried on. */
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
 * full); hold (DR): SHARE channels on /h/k00 to /h/k63, "r" sent back,
 * then held until it is killed. */
int extfs_dirs_child(int argc, char **argv)
{
    const char *m = argc >= 2 ? argv[1] : "";
    handle_t back = startup_handle(SR_USER), fs = HANDLE_INVALID, more;
    static handle_t hs[SHARE];
    uint32_t nh = 0;
    if (!back || read_msg(back, &fs, 1, &nh) != OK || nh != 1)
        return 1;
    if (!strcmp(m, "xdirs-hold")) {
        char p[16];
        for (unsigned i = 0; i < SHARE; i++) {
            snprintf(p, sizeof(p), "/h/k%02u", i);
            if (od(fs, p, FS_READ, &hs[i], NULL) != OK)
                return 5;
        }
        if (jam_channel_write(back, "r", 1, NULL, 0) != OK)
            return 6;
        (void)jam_object_wait_one(back, SIG_PEER_CLOSED, DEADLINE_NEVER, NULL);
        return 7;   /* never: the test kills it */
    }
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

/* ---- hostile runs (DR) ---------------------------------------------------------------- */

#define ST_MOVERS  8u
#define ST_WALKERS 4u
#define ST_NS      (3 * NS_PER_S)
#define ST_PROBES  16u   /* requests on each channel while it is held */

struct st_thread {
    handle_t    thread;
    unsigned    i;
    status_t    st;        /* an answer it mustn't get (its first) ... */
    const char *what;      /* ... and to what */
    unsigned    ok, cut, moves;
    unsigned    escapes;   /* reached outside the subview */
    char        out[FS_PATH_LEN + 1];
    _Alignas(16) uint8_t stack[32 << 10];
};
static struct st_thread st_mv[ST_MOVERS], st_wk[ST_WALKERS];
static handle_t st_fs, st_sub[ST_WALKERS];
static uint64_t st_end, st_out_id;

static void st_bad(struct st_thread *t, status_t st, const char *what)
{
    if (t->st == OK) {
        t->st = st == OK ? ERR_BAD_STATE : st;
        t->what = what;
    }
}

static void st_escape(struct st_thread *t, const char *what)
{
    t->escapes++;
    st_bad(t, ERR_BAD_STATE, what);
}

/* Directory d<i> moved between /v/in and /out until the storm's end, and
 * left in /v/in. */
static void st_mover(void *arg)
{
    struct st_thread *t = arg;
    char in[16], out[16];
    snprintf(in, sizeof(in), "/v/in/d%u", t->i);
    snprintf(out, sizeof(out), "/out/d%u", t->i);
    struct fatrun r = on(st_fs);
    bool inside = true;
    while (t->st == OK && now() < st_end) {
        status_t st = t_rename(&r, inside ? in : out, inside ? out : in);
        if (st != OK)
            st_bad(t, st, "a move");
        inside = !inside;
        t->moves++;
    }
    if (!inside && t->st == OK) {
        status_t st = t_rename(&r, out, in);
        if (st != OK)
            st_bad(t, st, "the move back");
    }
}

/* Probe j of channel ch (the subview's in/d<k>): counted inside (OK and
 * the answer from inside) or cut off (ERR_NOT_FOUND); anything from
 * outside the subview is an escape. */
static void st_probe(struct st_thread *t, handle_t ch, unsigned k, unsigned j)
{
    struct fatrun c = on(ch);
    char got[FS_NAME_MAX], want[16];
    uint8_t kind = 0;
    unsigned n = 0;
    status_t st;
    switch (j % 5) {
    case 0:   /* its own subdirectory */
        st = t_stat(&c, "s", NULL, NULL, NULL);
        break;
    case 1:   /* /v/in/secret is no name; /out/secret is outside */
        st = t_stat(&c, "../secret", NULL, NULL, NULL);
        if (st == OK)
            st_escape(t, "../secret reached");
        if (st == ERR_NOT_FOUND)
            st = OK;   /* (the answer from inside too: not counted as cut off) */
        break;
    case 2:   /* its place: in/d<k>, nothing else */
        snprintf(want, sizeof(want), "in/d%u", k);
        st = rs(ch, "", &kind, t->out);
        if (st == OK && (kind != FS_RESOLVE_INSIDE || strcmp(t->out, want)))
            st_escape(t, "a place outside");
        break;
    case 3:   /* "..": /v/in's listing, never /out's */
        while ((st = t_readdir(&c, "..", n, got, NULL)) == OK && n < 64) {
            if (!strcmp(got, "secret"))
                st_escape(t, "/out listed");
            n++;
        }
        if (st == ERR_NOT_FOUND && n)
            st = OK;   /* the listing's end */
        break;
    default:  /* above the subview's root: out of the mount, never /out */
        st = t_stat(&c, "../../../out/secret", NULL, NULL, NULL);
        if (st == OK)
            st_escape(t, "../../../out/secret reached");
        if (st == ERR_LINK_OUT)
            st = OK;   /* the caller's to follow in its own namespace */
        break;
    }
    if (st == OK)
        t->ok++;
    else if (st == ERR_NOT_FOUND)
        t->cut++;
    else
        st_bad(t, st, "a probe");
}

/* Channels on the moving directories, opened in the subview and held
 * across their moves; a rename_at into /out by its id. */
static void st_walker(void *arg)
{
    struct st_thread *t = arg;
    handle_t sub = st_sub[t->i];
    char p[16], w[8];
    snprintf(w, sizeof(w), "w%u", t->i);
    for (unsigned n = 0; t->st == OK && now() < st_end; n++) {
        unsigned k = (t->i * 3 + n) % ST_MOVERS;
        snprintf(p, sizeof(p), "in/d%u", k);
        handle_t ch;
        status_t st = od(sub, p, FS_READ, &ch, NULL);
        if (st == ERR_NOT_FOUND)
            continue;   /* outside now */
        if (st != OK) {
            st_bad(t, st, "an opendir");
            break;
        }
        for (unsigned j = 0; j < ST_PROBES && t->st == OK; j++)
            st_probe(t, ch, k, j + n);
        jam_handle_close(ch);
        st = fs_rename_at_until(sub, dl(), w, L(w), st_out_id, w, L(w), 0);
        if (st == OK)
            st_escape(t, "rename_at into /out");
        else if (st != ERR_NOT_FOUND)
            st_bad(t, st, "rename_at into /out");
    }
}

/* (DR) 8 threads move directories in and out of a subview (/v/in/d<k>
 * and /out/d<k>, through the volume's whole channel) while 4 others walk
 * through channels on them, opened in the subview and held across the
 * moves: each answer is from inside while the directory lies there, and
 * ERR_NOT_FOUND while it lies outside (the cut-off); nothing outside the
 * subview is ever reached (/out/secret through the channel's own "..",
 * /out's listing, a place but in/d<k>, a rename_at into /out by its id). */
bool t_extfs_dirs_storm(void)
{
    struct fatrun r;
    handle_t th[ST_MOVERS + ST_WALKERS];
    char p[24];
    CHECK(start(&r));
    CHECK_ST(t_mkdir(&r, "/v"), OK);
    CHECK_ST(t_mkdir(&r, "/v/in"), OK);
    CHECK_ST(t_mkdir(&r, "/out"), OK);
    CHECK(put_file(&r, "/out/secret", "s"));
    CHECK_ST(t_stat_id(&r, "/out", &st_out_id), OK);
    for (unsigned k = 0; k < ST_MOVERS; k++) {
        snprintf(p, sizeof(p), "/v/in/d%u", k);
        CHECK_ST(t_mkdir(&r, p), OK);
        snprintf(p, sizeof(p), "/v/in/d%u/s", k);
        CHECK_ST(t_mkdir(&r, p), OK);
    }
    for (unsigned i = 0; i < ST_WALKERS; i++) {
        snprintf(p, sizeof(p), "/v/w%u", i);
        CHECK(put_file(&r, p, "w"));
        CHECK_ST(fs_subview_until(r.fs, dl(), "/v", 2, 0, &st_sub[i]), OK);
    }
    st_fs = r.fs;
    st_end = now() + ST_NS;
    for (unsigned i = 0; i < ST_MOVERS + ST_WALKERS; i++) {
        bool mover = i < ST_MOVERS;
        struct st_thread *t = mover ? &st_mv[i] : &st_wk[i - ST_MOVERS];
        memset(t, 0, offsetof(struct st_thread, stack));
        t->i = mover ? i : i - ST_MOVERS;
        CHECK_ST(thread_spawn(mover ? "dirs mover" : "dirs walker", mover ? st_mover : st_walker,
                              t, t->stack, sizeof(t->stack), &t->thread), OK);
        th[i] = t->thread;
    }
    CHECK(wait_threads(th, ST_MOVERS + ST_WALKERS));
    unsigned ok = 0, cut = 0, moves = 0, escapes = 0;
    bool all = true;
    for (unsigned i = 0; i < ST_MOVERS + ST_WALKERS; i++) {
        const struct st_thread *t = i < ST_MOVERS ? &st_mv[i] : &st_wk[i - ST_MOVERS];
        if (t->st != OK)
            printf("utest: %s: %s %u: %s: %s\n", utest_cur, i < ST_MOVERS ? "mover" : "walker",
                   t->i, t->what, status_str(t->st));
        all &= t->st == OK;
        ok += t->ok;
        cut += t->cut;
        moves += t->moves;
        escapes += t->escapes;
    }
    printf("utest: %s: %u moves; %u answers from inside, %u cut off, %u from outside\n",
           utest_cur, moves, ok, cut, escapes);
    CHECK(all);
    CHECK_EQ(escapes, 0u);
    CHECK(ok > 0 && cut > 0 && moves >= ST_MOVERS);
    close_all(st_sub, ST_WALKERS);
    return stop(&r, "dirs-storm");
}

/* (DR) A process holding its 64 channels (on /h/k00 to /h/k63) killed
 * after half of those directories were removed under it: the removed
 * ones dropped (their blocks free again) and every slot back (our 64 and
 * three children's 64: the volume's 256). */
bool t_extfs_dirs_killed(void)
{
    struct fatrun r;
    static handle_t own[SHARE], kids[CHANNELS - SHARE];
    handle_t mine, theirs, dup, job, proc;
    struct process_info info;
    uint64_t total = 0, before = 0, held = 0;
    uint32_t nh = 0;
    char p[16];
    CHECK(start(&r));
    CHECK_ST(t_mkdir(&r, "/h"), OK);
    for (unsigned i = 0; i < SHARE; i++) {
        snprintf(p, sizeof(p), "/h/k%02u", i);
        CHECK_ST(t_mkdir(&r, p), OK);
    }
    CHECK_ST(t_free(&r, &total, &before), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("xdirs-hold", NULL, job, theirs, &proc), OK);
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &dup), OK);
    CHECK_ST(jam_channel_write(mine, "f", 1, &dup, 1), OK);
    CHECK_ST(read_msg(mine, NULL, 0, &nh), OK);   /* "r": it holds them */
    for (unsigned i = 0; i < SHARE / 2; i++) {
        snprintf(p, sizeof(p), "/h/k%02u", i);
        CHECK_ST(t_unlink_flags(&r, p, FS_UNLINK_DIR), OK);   /* held: orphans */
    }
    CHECK_ST(t_free(&r, &total, &held), OK);
    CHECK(held <= before);   /* their blocks kept while they are held */
    CHECK_ST(jam_job_kill(job), OK);
    CHECK_ST(spawn_wait(proc, FAT_CALL_NS, &info), OK);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(mine);
    CHECK(freed_past(&r, before));
    for (unsigned i = 0; i < SHARE; i++)
        CHECK_ST(od(r.fs, "/", 0, &own[i], NULL), OK);
    for (unsigned k = 0; k < 3; k++)
        CHECK(kid(r.fs, "xdirs-share", &kids[k * SHARE]));
    close_all(own, SHARE);
    close_all(kids, CHANNELS - SHARE);
    return stop(&r, "dirs-killed");
}

#define DK_EVERY_MS 200u
#define DK_KILLS    10u
#define DK_MAX_NS   (120 * NS_PER_S)
#define DK_FILES    4u

static unsigned dk_io;   /* calls answered ERR_IO (in progress at two deaths), made again */

static status_t dk_mk(handle_t ch, const char *n)
{
    status_t st;
    bool again = false;
    while ((st = mk(ch, n)) == ERR_IO) {
        dk_io++;
        again = true;
    }
    return again && st == ERR_ALREADY_EXISTS ? OK : st;
}

static status_t dk_od(handle_t ch, const char *n, handle_t *out)
{
    status_t st;
    while ((st = od(ch, n, FS_READ, out, NULL)) == ERR_IO)
        dk_io++;
    return st;
}

static status_t dk_rm(handle_t ch, const char *n, uint32_t flags)
{
    status_t st;
    bool again = false;
    while ((st = fs_unlink_until(ch, dl(), n, L(n), flags)) == ERR_IO) {
        dk_io++;
        again = true;
    }
    return again && st == ERR_NOT_FOUND ? OK : st;
}

static status_t dk_file(handle_t ch, const char *n)
{
    struct fatrun c = on(ch);
    struct tfile f;
    status_t st;
    while ((st = t_open(&c, n, FS_WRITE | FS_CREATE, &f)) == ERR_IO)
        dk_io++;
    if (st == OK)
        t_close(&f);
    return st;
}

static status_t dk_list(handle_t ch)
{
    struct fatrun c = on(ch);
    status_t st;
    while ((st = t_batch(&c, "", 0, &batch)) == ERR_IO)
        dk_io++;
    return st;
}

/* One round under top: DeleteDeep's tree made through channels, 4 files
 * in each level, then removed through channels as DeletePathRecursively
 * removes one (each level listed and its files unlinked; each directory
 * removed while its channel is held, which then answers as a removed
 * one). The step that failed, or NULL. */
static const char *dk_round(handle_t top)
{
    static handle_t ch[LEVELS + 1];
    char f[4] = "f0";
    ch[0] = top;
    for (unsigned i = 0; i < LEVELS; i++) {
        if (dk_mk(ch[i], name) != OK)
            return "a mkdir";
        for (unsigned k = 0; k < DK_FILES; k++) {
            f[1] = (char)('0' + k);
            if (dk_file(ch[i], f) != OK)
                return "a file made";
        }
        if (dk_od(ch[i], name, &ch[i + 1]) != OK)
            return "an opendir";
    }
    for (unsigned i = 0; i < LEVELS; i++) {
        if (dk_list(ch[i]) != OK)
            return "a listing";
        for (unsigned k = 0; k < DK_FILES; k++) {
            f[1] = (char)('0' + k);
            if (dk_rm(ch[i], f, 0) != OK)
                return "a file unlinked";
        }
    }
    for (unsigned i = LEVELS; i > 0; i--) {
        uint32_t nlink = 1;
        if (dk_rm(ch[i - 1], name, FS_UNLINK_DIR) != OK)
            return "an rmdir";
        status_t st;
        while ((st = dattr(ch[i], "", NULL, &nlink)) == ERR_IO)
            dk_io++;
        if (st != OK || nlink)
            return "a removed directory's channel";
        jam_handle_close(ch[i]);
    }
    return NULL;
}

/* (DR) DeleteDeep's tree made and removed through channels, round after
 * round, while the supervisor kills extfs every 200 ms: every call
 * answered (OK, or ERR_IO for one in progress at two deaths, made again),
 * every channel carried across every restart (never ERR_PEER_CLOSED),
 * each directory removed while held answering as a removed one; the disk
 * clean after (extfs-svc-test's e2fsck). */
bool t_extfs_restart_deep(void)
{
    handle_t top;
    unsigned count = 0, rounds = 0;
    memset(name, 'k', NM);
    name[NM] = '\0';
    dk_io = 0;
    CHECK(ramdisk_create(&dd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&dd, NULL, 0));
    struct fatrun *f = extfs_sup_run();
    CHECK_ST(t_mkdir(f, "/dk"), OK);
    CHECK_ST(od(f->fs, "/dk", FS_READ, &top, NULL), OK);
    extfs_sup_storm(DK_EVERY_MS * NS_PER_MS);
    const char *bad = NULL;
    uint64_t t0 = now();
    while (!bad && extfs_sup_crashes() < DK_KILLS && now() - t0 < DK_MAX_NS) {
        bad = dk_round(top);
        rounds += !bad;
    }
    extfs_sup_storm(0);
    jam_sleep_until(now() + 100 * NS_PER_MS);   /* a kill the supervisor had timed already */
    unsigned kills = extfs_sup_crashes();
    printf("utest: %s: %u rounds of %u levels in %lu ms, extfs killed %u times, %u calls made "
           "again (ERR_IO)%s%s\n", utest_cur, rounds, LEVELS,
           (unsigned long)((now() - t0) / NS_PER_MS), kills, dk_io, bad ? "; failed at " : "",
           bad ? bad : "");
    CHECK(!bad);
    CHECK(kills >= DK_KILLS);
    struct fatrun t = on(top);
    CHECK(dir_count(&t, "", NULL, &count, NULL));
    CHECK_EQ(count, 0u);
    jam_handle_close(top);
    CHECK_ST(t_unlink_flags(f, "/dk", FS_UNLINK_DIR), OK);
    CHECK(extfs_sup_stop());
    CHECK(extfs_save(&dd, "restart-deep"));
    return ramdisk_destroy(&dd);
}
