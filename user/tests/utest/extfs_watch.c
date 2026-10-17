/* utest: file watching on the extfs service (bin/extfs's watch.c,
 * docs/INOTIFY-PLAN.md's IE1) over a RAM disk it formats, through the
 * protocols themselves (fs.watch, fswatch, fswatch_bell): the events a
 * watcher takes must be Linux's, exactly, in Linux's order.
 *
 *   extfs_watch_rows    each row of the plan's 2.2: opens, reads, writes,
 *                       closes, truncates, setattr's masks, a new name of
 *                       each kind, a link, unlinks of a file (named
 *                       twice, once, and open), of a directory; what makes
 *                       no event (readdir, stat, readlink, resolve)
 *   extfs_watch_moves   renames in and across directories with one cookie,
 *                       over a name (its IN_ATTRIB, its IN_DELETE_SELF),
 *                       RENAME_EXCHANGE's two pairs, a directory moved, a
 *                       name onto itself, an open file's events following
 *                       its name, IN_EXCL_UNLINK
 *   extfs_watch_views   a change through one view (and from a second
 *                       process) seen through another, a subview's; add's
 *                       walk: links inside, IN_DONT_FOLLOW, ERR_LINK_OUT,
 *                       IN_ONLYDIR, the read bit; the mask flags
 *                       (IN_MASK_ADD, IN_MASK_CREATE, IN_ONESHOT); remove
 *   extfs_watch_order   the close rule (2.3): a close made before a request
 *                       was sent is handled first, even folded into the
 *                       file's earlier packet (the service kept busy so it
 *                       is): write and close, add, write and close give
 *                       one IN_CLOSE_WRITE; a close then a write on another
 *                       file: the close's event first
 *   extfs_watch_limits  the bell rung once per empty queue; the queue's
 *                       bound (one IN_Q_OVERFLOW, the rest dropped, events
 *                       again after a take); 8 watcher channels for this
 *                       process and the ninth refused (fs.watch's writer
 *                       pid), a second process's still taken
 *
 * Each disk is saved for Linux's e2fsck (tools/extfs-svc-test.sh). The
 * second process's mode is xwatch-<what>. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <fswatch.h>
#include <idl/file.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define IN(x) FSWATCH_IN_##x
#define ALL   FSWATCH_IN_ALL_EVENTS
#define RECS  128

/* A watcher channel, its bell given. */
struct wat {
    handle_t ch, bell;
};

/* A record taken. */
struct rec {
    uint32_t id, mask, cookie, len;
    char     name[256];
};

/* An event wanted: its watch, mask and name. */
struct want {
    uint32_t    id, mask;
    const char *name;
};

static struct ramdisk xd;
static struct fatrun  r;
static struct rec     got[RECS];   /* the last take_all's */
static int            ngot;
static uint8_t        takebuf[FSWATCH_TAKE_BYTES];

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static uint32_t L(const char *s)
{
    return (uint32_t)strlen(s);
}

static bool watcher(handle_t fs, struct wat *x)
{
    handle_t give;
    CHECK_ST(fs_watch_until(fs, dl(), &x->ch), OK);
    CHECK_ST(jam_event_create(&x->bell), OK);
    CHECK_ST(jam_handle_duplicate(x->bell, RIGHT_SIGNAL | RIGHT_TRANSFER, &give), OK);
    CHECK_ST(fswatch_bell_give(x->ch, give), OK);
    return true;
}

static void unwatcher(struct wat *x)
{
    jam_handle_close(x->ch);
    jam_handle_close(x->bell);
}

static status_t add_st(const struct wat *x, const char *path, uint32_t mask, uint32_t *id,
                       uint8_t *existed)
{
    uint8_t e = 0;
    return fswatch_add_until(x->ch, dl(), path, L(path), mask, id, existed ? existed : &e);
}

/* A new watch's id (0: the add failed, said in the log). */
static uint32_t add(const struct wat *x, const char *path, uint32_t mask)
{
    uint32_t id = 0;
    uint8_t existed = 1;
    status_t st = add_st(x, path, mask, &id, &existed);
    if (st != OK || existed || !id) {
        printf("utest: %s: add %s: %s, existed %u\n", utest_cur, path, status_str(st), existed);
        return 0;
    }
    return id;
}

static bool rung(const struct wat *x)
{
    return jam_object_wait_one(x->bell, FSWATCH_BELL, 0, NULL) == OK;
}

/* Everything queued on x (the bell cleared first, then takes until `more`
 * is 0) into got[]. */
static bool take_all(const struct wat *x)
{
    uint8_t more = 1;
    ngot = 0;
    CHECK_ST(jam_event_signal(x->bell, FSWATCH_BELL, 0), OK);
    while (more) {
        uint32_t n = 0;
        CHECK_ST(fswatch_take_until(x->ch, dl(), FSWATCH_TAKE_BYTES, &more, takebuf, &n), OK);
        for (uint32_t o = 0; o < n;) {
            struct rec *e = &got[ngot];
            uint16_t len;
            CHECK(ngot < RECS && n - o >= FSWATCH_RECORD_FIXED);
            memcpy(&e->id, takebuf + o, 4);
            memcpy(&e->mask, takebuf + o + 4, 4);
            memcpy(&e->cookie, takebuf + o + 8, 4);
            memcpy(&len, takebuf + o + 12, 2);
            o += FSWATCH_RECORD_FIXED;
            CHECK(len < sizeof(e->name) && n - o >= len);
            e->len = len;
            memcpy(e->name, takebuf + o, len);
            e->name[len] = '\0';
            o += len;
            ngot++;
        }
        CHECK(n || !more);
    }
    return true;
}

static void say_got(void)
{
    for (int i = 0; i < ngot; i++)
        printf("utest:   %d: id %u mask %#x cookie %u \"%s\"\n", i, got[i].id, got[i].mask,
               got[i].cookie, got[i].name);
}

/* x's queue is exactly want[0..n), in that order. */
static bool expect(const struct wat *x, const struct want *want, int n, const char *what)
{
    if (!take_all(x))
        return false;
    bool ok = ngot == n;
    for (int i = 0; ok && i < n; i++)
        ok = got[i].id == want[i].id && got[i].mask == want[i].mask &&
             got[i].len == L(want[i].name) && !strcmp(got[i].name, want[i].name);
    if (!ok) {
        printf("utest: %s: %s: got %d events, want %d:\n", utest_cur, what, ngot, n);
        say_got();
        for (int i = 0; i < n; i++)
            printf("utest:   want %d: id %u mask %#x \"%s\"\n", i, want[i].id, want[i].mask,
                   want[i].name);
        FAIL("%s: not the events wanted", what);
    }
    return true;
}

#define EXPECT(x, what, ...)                                                    \
    do {                                                                        \
        const struct want w_[] = { __VA_ARGS__ };                               \
        if (!expect(x, w_, (int)(sizeof(w_) / sizeof(w_[0])), what))           \
            return false;                                                       \
    } while (0)
#define NOTHING(x, what)                                                        \
    do {                                                                        \
        if (!expect(x, NULL, 0, what))                                          \
            return false;                                                       \
    } while (0)

/* The cases' watch ids. */
static uint32_t idD, idF, idS, idO, idA, idB, idX, idY, idZ, idSD, idE, idIN, idH;

static bool start(void)
{
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    return true;
}

static bool stop(const char *name)
{
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, name));
    return ramdisk_destroy(&xd);
}

static status_t setattr(const char *path, uint32_t mask, uint32_t mode)
{
    return fs_setattr_until(r.fs, dl(), path, L(path), 0, mask, mode, 0, 0, 1000000000,
                            2000000000);
}

/* ---- each row of 2.2 ----------------------------------------------------------- */

bool t_extfs_watch_rows(void)
{
    struct wat a;
    struct tfile f;
    uint32_t done = 0;
    char buf[8];
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK(put_file(&r, "/d/f", "x"));
    CHECK(watcher(r.fs, &a));
    CHECK((idD = add(&a, "/d", ALL)) && (idF = add(&a, "/d/f", ALL)) && idD != idF);
    NOTHING(&a, "the adds");
    CHECK(!rung(&a));

    /* Read: the open, a read, a read of nothing (none), the close. */
    CHECK_ST(t_open(&r, "/d/f", FS_READ, &f), OK);
    CHECK(rung(&a));
    CHECK_ST(t_read(&f, 0, buf, 1, &done), OK);
    CHECK_ST(t_read(&f, 100, buf, 1, &done), OK);
    CHECK_EQ(done, 0);
    t_close(&f);
    EXPECT(&a, "a read", { idD, IN(OPEN), "f" }, { idF, IN(OPEN), "" }, { idD, IN(ACCESS), "f" },
           { idF, IN(ACCESS), "" }, { idD, IN(CLOSE_NOWRITE), "f" }, { idF, IN(CLOSE_NOWRITE), "" });
    CHECK(!rung(&a));

    /* Write: a write of nothing makes none. */
    CHECK_ST(t_open(&r, "/d/f", FS_WRITE, &f), OK);
    CHECK_ST(t_write(&f, 0, "abc", 3, &done), OK);
    CHECK_ST(t_write(&f, 0, "", 0, &done), OK);
    t_close(&f);
    EXPECT(&a, "a write", { idD, IN(OPEN), "f" }, { idF, IN(OPEN), "" }, { idD, IN(MODIFY), "f" },
           { idF, IN(MODIFY), "" }, { idD, IN(CLOSE_WRITE), "f" }, { idF, IN(CLOSE_WRITE), "" });

    /* fs.open with FS_TRUNCATE: the open, then the truncation; file.truncate. */
    CHECK_ST(t_open(&r, "/d/f", FS_WRITE | FS_TRUNCATE, &f), OK);
    CHECK_ST(file_truncate_until(f.ch, dl(), 10), OK);
    t_close(&f);
    EXPECT(&a, "truncates", { idD, IN(OPEN), "f" }, { idF, IN(OPEN), "" },
           { idD, IN(MODIFY), "f" }, { idF, IN(MODIFY), "" }, { idD, IN(MODIFY), "f" },
           { idF, IN(MODIFY), "" }, { idD, IN(CLOSE_WRITE), "f" }, { idF, IN(CLOSE_WRITE), "" });

    /* setattr: mode, one time, the other, both, mode and a time; and
     * file.setattr through an open. */
    CHECK_ST(setattr("/d/f", FS_ATTR_MODE, 0640), OK);
    CHECK_ST(setattr("/d/f", FS_ATTR_ATIME, 0), OK);
    CHECK_ST(setattr("/d/f", FS_ATTR_MTIME, 0), OK);
    CHECK_ST(setattr("/d/f", FS_ATTR_ATIME | FS_ATTR_MTIME, 0), OK);
    CHECK_ST(setattr("/d/f", FS_ATTR_MODE | FS_ATTR_MTIME, 0644), OK);
    CHECK_ST(setattr("/d/f", 0, 0), OK);   /* nothing set: nothing said */
    EXPECT(&a, "setattr", { idD, IN(ATTRIB), "f" }, { idF, IN(ATTRIB), "" },
           { idD, IN(ACCESS), "f" }, { idF, IN(ACCESS), "" }, { idD, IN(MODIFY), "f" },
           { idF, IN(MODIFY), "" }, { idD, IN(ATTRIB), "f" }, { idF, IN(ATTRIB), "" },
           { idD, IN(ATTRIB) | IN(MODIFY), "f" }, { idF, IN(ATTRIB) | IN(MODIFY), "" });
    CHECK_ST(t_open(&r, "/d/f", FS_READ | FS_WRITE, &f), OK);
    CHECK_ST(file_setattr_until(f.ch, dl(), FS_ATTR_MODE, 0600, 0, 0, 0, 0), OK);
    t_close(&f);
    EXPECT(&a, "file.setattr", { idD, IN(OPEN), "f" }, { idF, IN(OPEN), "" },
           { idD, IN(ATTRIB), "f" }, { idF, IN(ATTRIB), "" }, { idD, IN(CLOSE_WRITE), "f" },
           { idF, IN(CLOSE_WRITE), "" });

    /* New names: a file made by fs.open (then its open and close), a
     * directory, a symbolic link; a hard link (the inode's count first). */
    CHECK_ST(t_open(&r, "/d/g", FS_WRITE | FS_CREATE, &f), OK);
    t_close(&f);
    CHECK_ST(t_mkdir(&r, "/d/sub"), OK);
    CHECK_ST(fs_symlink_until(r.fs, dl(), "f", 1, "/d/ln", 5), OK);
    CHECK_ST(fs_link_until(r.fs, dl(), "/d/f", 4, "/d/h", 4), OK);
    EXPECT(&a, "new names", { idD, IN(CREATE), "g" }, { idD, IN(OPEN), "g" },
           { idD, IN(CLOSE_WRITE), "g" }, { idD, IN(CREATE) | IN(ISDIR), "sub" },
           { idD, IN(CREATE), "ln" }, { idF, IN(ATTRIB), "" }, { idD, IN(CREATE), "h" });

    /* What changes nothing says nothing. */
    struct tbatch b;
    char target[FS_PATH_LEN + 1];
    uint32_t tlen = 0;
    uint8_t kind;
    uint16_t used;
    uint32_t mode = 0;
    CHECK_ST(fs_readdir_until(r.fs, dl(), "/d", 2, 0, &b.next, &b.count, b.entries, &b.len), OK);
    CHECK_ST(xmode(r.fs, "/d/f", 0, &mode, NULL), OK);
    CHECK_ST(t_stat(&r, "/d/f", NULL, NULL, NULL), OK);
    CHECK_ST(fs_readlink_until(r.fs, dl(), "/d/ln", 5, target, &tlen), OK);
    CHECK_ST(fs_resolve_until(r.fs, dl(), "/d/ln", 5, 0, &kind, &used, target, &tlen), OK);
    NOTHING(&a, "reads of names and attributes");

    /* Unlinks: one of two names (IN_ATTRIB, then IN_DELETE), a directory
     * (its own watch: IN_DELETE_SELF, IN_IGNORED), the last name. */
    CHECK((idS = add(&a, "/d/sub", ALL)));
    CHECK_ST(t_unlink_flags(&r, "/d/h", 0), OK);
    CHECK_ST(t_unlink_flags(&r, "/d/sub", FS_UNLINK_DIR), OK);
    CHECK_ST(t_unlink_flags(&r, "/d/f", 0), OK);
    EXPECT(&a, "unlinks", { idF, IN(ATTRIB), "" }, { idD, IN(DELETE), "h" },
           { idD, IN(DELETE) | IN(ISDIR), "sub" }, { idS, IN(DELETE_SELF), "" },
           { idS, IN(IGNORED), "" }, { idF, IN(ATTRIB), "" }, { idD, IN(DELETE), "f" },
           { idF, IN(DELETE_SELF), "" }, { idF, IN(IGNORED), "" });

    /* An open file unlinked: its events go on, in its old directory too
     * (no IN_EXCL_UNLINK), and it goes at its last close. */
    CHECK(put_file(&r, "/d/o", "o"));
    CHECK((idO = add(&a, "/d/o", ALL)));
    CHECK_ST(t_open(&r, "/d/o", FS_WRITE, &f), OK);
    CHECK(take_all(&a));
    CHECK_ST(t_unlink_flags(&r, "/d/o", 0), OK);
    CHECK_ST(t_write(&f, 0, "p", 1, &done), OK);
    t_close(&f);
    EXPECT(&a, "an open file unlinked", { idO, IN(ATTRIB), "" }, { idD, IN(DELETE), "o" },
           { idD, IN(MODIFY), "o" }, { idO, IN(MODIFY), "" }, { idD, IN(CLOSE_WRITE), "o" },
           { idO, IN(CLOSE_WRITE), "" }, { idO, IN(DELETE_SELF), "" }, { idO, IN(IGNORED), "" });

    unwatcher(&a);
    return stop("watch-rows");
}

/* ---- moves --------------------------------------------------------------------- */

/* got[i] and got[j] carry one cookie, not 0. */
static bool paired(int i, int j)
{
    return got[i].cookie && got[i].cookie == got[j].cookie;
}

bool t_extfs_watch_moves(void)
{
    struct wat a, e;
    struct tfile f;
    uint32_t done = 0, c1;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/a"), OK);
    CHECK_ST(t_mkdir(&r, "/b"), OK);
    CHECK(put_file(&r, "/a/x", "x"));
    CHECK(put_file(&r, "/a/y", "y"));
    CHECK(put_file(&r, "/b/z", "z"));
    CHECK(watcher(r.fs, &a));
    CHECK((idA = add(&a, "/a", ALL)) && (idB = add(&a, "/b", ALL)) && (idX = add(&a, "/a/x", ALL)));
    CHECK((idY = add(&a, "/a/y", ALL)) && (idZ = add(&a, "/b/z", ALL)));

    /* In one directory, then across two: a cookie each, the same for the
     * pair. */
    CHECK_ST(t_rename(&r, "/a/x", "/a/x2"), OK);
    EXPECT(&a, "a rename", { idA, IN(MOVED_FROM), "x" }, { idA, IN(MOVED_TO), "x2" },
           { idX, IN(MOVE_SELF), "" });
    CHECK(paired(0, 1));
    c1 = got[0].cookie;
    CHECK_ST(t_rename(&r, "/a/x2", "/b/x"), OK);
    EXPECT(&a, "a move", { idA, IN(MOVED_FROM), "x2" }, { idB, IN(MOVED_TO), "x" },
           { idX, IN(MOVE_SELF), "" });
    CHECK(paired(0, 1) && got[0].cookie != c1);

    /* Over a name: its inode's link count, then the move, then it goes. */
    CHECK_ST(t_rename_flags(&r, "/a/y", "/b/z", FS_RENAME_REPLACE), OK);
    EXPECT(&a, "a rename over a name", { idA, IN(MOVED_FROM), "y" }, { idB, IN(MOVED_TO), "z" },
           { idZ, IN(ATTRIB), "" }, { idY, IN(MOVE_SELF), "" }, { idZ, IN(DELETE_SELF), "" },
           { idZ, IN(IGNORED), "" });
    CHECK(paired(0, 1));

    /* RENAME_EXCHANGE: each name's move, a cookie each. */
    CHECK_ST(t_rename_flags(&r, "/b/x", "/b/z", FS_RENAME_EXCHANGE), OK);
    EXPECT(&a, "an exchange", { idB, IN(MOVED_FROM), "x" }, { idB, IN(MOVED_TO), "z" },
           { idX, IN(MOVE_SELF), "" }, { idB, IN(MOVED_FROM), "z" }, { idB, IN(MOVED_TO), "x" },
           { idY, IN(MOVE_SELF), "" });
    CHECK(paired(0, 1) && paired(3, 4) && got[0].cookie != got[3].cookie);
    CHECK(file_is(&r, "/b/z", "x") && file_is(&r, "/b/x", "y"));
    CHECK(take_all(&a));   /* (file_is's opens and reads) */

    /* A directory: IN_ISDIR on the pair, never on IN_MOVE_SELF. */
    CHECK_ST(t_mkdir(&r, "/a/sd"), OK);
    CHECK((idSD = add(&a, "/a/sd", ALL)));
    CHECK_ST(t_rename(&r, "/a/sd", "/b/sd"), OK);
    EXPECT(&a, "a directory moved", { idA, IN(CREATE) | IN(ISDIR), "sd" },
           { idA, IN(MOVED_FROM) | IN(ISDIR), "sd" }, { idB, IN(MOVED_TO) | IN(ISDIR), "sd" },
           { idSD, IN(MOVE_SELF), "" });

    /* A name onto another name of the same inode: nothing done, nothing said. */
    CHECK_ST(fs_link_until(r.fs, dl(), "/b/z", 4, "/b/zl", 5), OK);
    CHECK(take_all(&a));
    CHECK_ST(t_rename_flags(&r, "/b/z", "/b/zl", FS_RENAME_REPLACE), OK);
    NOTHING(&a, "a rename onto the same inode");

    /* An open file's events follow its name. */
    CHECK_ST(t_open(&r, "/b/x", FS_WRITE, &f), OK);
    CHECK_ST(t_rename(&r, "/b/x", "/a/w"), OK);
    CHECK_ST(t_write(&f, 0, "w", 1, &done), OK);
    t_close(&f);
    EXPECT(&a, "an open file renamed", { idB, IN(OPEN), "x" }, { idY, IN(OPEN), "" },
           { idB, IN(MOVED_FROM), "x" }, { idA, IN(MOVED_TO), "w" }, { idY, IN(MOVE_SELF), "" },
           { idA, IN(MODIFY), "w" }, { idY, IN(MODIFY), "" }, { idA, IN(CLOSE_WRITE), "w" },
           { idY, IN(CLOSE_WRITE), "" });

    /* IN_EXCL_UNLINK: a watch with it hears nothing of a file unlinked
     * while open; one without does. */
    CHECK(watcher(r.fs, &e));
    CHECK((idE = add(&e, "/a", IN(MODIFY) | IN(EXCL_UNLINK))));
    CHECK_ST(t_open(&r, "/a/w", FS_WRITE, &f), OK);
    CHECK_ST(t_write(&f, 0, "1", 1, &done), OK);
    CHECK_ST(t_unlink_flags(&r, "/a/w", 0), OK);
    CHECK_ST(t_write(&f, 0, "2", 1, &done), OK);
    t_close(&f);
    EXPECT(&e, "IN_EXCL_UNLINK", { idE, IN(MODIFY), "w" });
    EXPECT(&a, "without it", { idA, IN(OPEN), "w" }, { idY, IN(OPEN), "" },
           { idA, IN(MODIFY), "w" }, { idY, IN(MODIFY), "" }, { idY, IN(ATTRIB), "" },
           { idA, IN(DELETE), "w" }, { idA, IN(MODIFY), "w" }, { idY, IN(MODIFY), "" },
           { idA, IN(CLOSE_WRITE), "w" }, { idY, IN(CLOSE_WRITE), "" },
           { idY, IN(DELETE_SELF), "" }, { idY, IN(IGNORED), "" });

    unwatcher(&e);
    unwatcher(&a);
    return stop("watch-moves");
}

/* ---- views, a second process, add's rules --------------------------------------- */

/* The second process (mode xwatch-change, a view as SR_USER): a file made,
 * written, closed and renamed. */
static int change_child(void)
{
    struct fatrun v = { .fs = startup_handle(SR_USER) };
    struct tfile f;
    uint32_t done = 0;
    if (!v.fs || t_open(&v, "/v/in/c", FS_WRITE | FS_CREATE, &f) != OK)
        return 1;
    status_t st = t_write(&f, 0, "kid", 3, &done);
    t_close(&f);
    if (st != OK || done != 3)
        return 2;
    return t_rename(&v, "/v/in/c", "/v/in/d") == OK ? 0 : 3;
}

/* The second process (mode xwatch-cap, the mount's channel as SR_USER): a
 * watcher channel of its own. */
static int cap_child(void)
{
    handle_t fs = startup_handle(SR_USER), ch = HANDLE_INVALID;
    if (!fs)
        return 1;
    status_t st = fs_watch_until(fs, dl(), &ch);
    return st == OK ? 0 : st == ERR_NO_RESOURCES ? 2 : 3;
}

int extfs_watch_child(int argc, char **argv)
{
    const char *m = argc >= 2 ? argv[1] : "";
    if (!strcmp(m, "xwatch-change"))
        return change_child();
    if (!strcmp(m, "xwatch-cap"))
        return cap_child();
    return 127;
}

static bool run(const char *mode, handle_t fs)
{
    handle_t job, proc, dup;
    struct process_info info;
    CHECK_ST(jam_handle_duplicate(fs, RIGHT_SAME, &dup), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child(mode, NULL, job, dup, &proc), OK);
    CHECK_ST(spawn_wait(proc, FAT_CALL_NS, &info), OK);
    jam_handle_close(proc);
    jam_handle_close(job);
    if (info.exit_code)
        FAIL("%s: exit %d", mode, (int)info.exit_code);
    return true;
}

bool t_extfs_watch_views(void)
{
    struct wat s, b;
    struct fatrun v;
    handle_t sub, view;
    uint32_t id = 0, done = 0;
    uint8_t existed = 0;
    struct tfile f;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/v"), OK);
    CHECK_ST(t_mkdir(&r, "/v/in"), OK);
    CHECK(put_file(&r, "/v/in/f", "f"));
    CHECK(put_file(&r, "/v/in/h", "h"));
    CHECK_ST(fs_symlink_until(r.fs, dl(), "in", 2, "/v/ln", 5), OK);
    CHECK_ST(fs_symlink_until(r.fs, dl(), "/elsewhere", 10, "/v/out", 6), OK);

    /* A watcher through a subview rooted at /v; changes through a view of
     * the whole volume, through the mount's channel, and from a second
     * process. */
    CHECK_ST(fs_subview_until(r.fs, dl(), "/v", 2, 0, &sub), OK);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_GUARD_ETC, &view), OK);
    v = r;
    v.fs = view;
    CHECK(watcher(sub, &s));
    CHECK((idIN = add(&s, "/in", ALL)));
    CHECK(put_file(&v, "/v/in/g", "g"));
    CHECK_ST(t_unlink_flags(&r, "/v/in/g", 0), OK);
    EXPECT(&s, "changes through two other views", { idIN, IN(CREATE), "g" },
           { idIN, IN(OPEN), "g" }, { idIN, IN(MODIFY), "g" }, { idIN, IN(CLOSE_WRITE), "g" },
           { idIN, IN(DELETE), "g" });
    CHECK(run("xwatch-change", view));
    EXPECT(&s, "a second process's", { idIN, IN(CREATE), "c" }, { idIN, IN(OPEN), "c" },
           { idIN, IN(MODIFY), "c" }, { idIN, IN(CLOSE_WRITE), "c" },
           { idIN, IN(MOVED_FROM), "c" }, { idIN, IN(MOVED_TO), "d" });
    CHECK(paired(4, 5));

    /* add's walk, in the subview: a link inside followed (the same watch,
     * its mask replaced), or not (IN_DONT_FOLLOW: the link's own); one out
     * of the view; IN_ONLYDIR; a name not there; the read bit. */
    CHECK_ST(add_st(&s, "/ln", ALL, &id, &existed), OK);
    CHECK(id == idIN && existed == 1);
    CHECK_ST(add_st(&s, "/ln", ALL | IN(DONT_FOLLOW), &id, &existed), OK);
    CHECK(id != idIN && existed == 0);
    CHECK_ST(add_st(&s, "/out", ALL, &id, NULL), ERR_LINK_OUT);
    CHECK_ST(add_st(&s, "/..", ALL, &id, NULL), ERR_LINK_OUT);
    CHECK_ST(add_st(&s, "/in/f", ALL | IN(ONLYDIR), &id, NULL), ERR_NOT_DIR);
    CHECK_ST(add_st(&s, "/in/f/", ALL, &id, NULL), ERR_NOT_DIR);
    CHECK_ST(add_st(&s, "/in/none", ALL, &id, NULL), ERR_NOT_FOUND);
    CHECK_ST(setattr("/v/in/f", FS_ATTR_MODE, 0200), OK);
    CHECK_ST(add_st(&s, "/in/f", ALL, &id, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(setattr("/v/in/f", FS_ATTR_MODE, 0600), OK);

    /* The mask's rules: no event at all, both of MASK_ADD and MASK_CREATE;
     * MASK_CREATE on a watch there; IN_ONESHOT. */
    CHECK_ST(add_st(&s, "/in", 0, &id, NULL), ERR_INVALID_ARGS);
    CHECK_ST(add_st(&s, "/in", IN(MODIFY) | IN(MASK_ADD) | IN(MASK_CREATE), &id, NULL),
             ERR_INVALID_ARGS);
    CHECK_ST(add_st(&s, "/in", ALL | IN(MASK_CREATE), &id, NULL), ERR_ALREADY_EXISTS);
    CHECK((idH = add(&s, "/in/h", IN(MODIFY) | IN(ONESHOT))));
    CHECK(take_all(&s));
    CHECK_ST(t_open(&r, "/v/in/h", FS_WRITE, &f), OK);
    CHECK_ST(t_write(&f, 0, "1", 1, &done), OK);
    CHECK_ST(t_write(&f, 0, "2", 1, &done), OK);
    CHECK_ST(fswatch_remove_until(s.ch, dl(), idIN), OK);
    CHECK_ST(fswatch_remove_until(s.ch, dl(), idIN), OK);   /* gone already: OK, nothing */
    t_close(&f);
    /* (the removed watch's queued events go with it: the library's rule) */
    EXPECT(&s, "IN_ONESHOT and a remove", { idH, IN(MODIFY), "" }, { idH, IN(IGNORED), "" },
           { idIN, IN(IGNORED), "" });

    /* IN_MASK_ADD adds to a watch's mask; an add without it replaces it. */
    CHECK(watcher(r.fs, &b));
    CHECK((id = add(&b, "/v/in", IN(CREATE))));
    CHECK_ST(add_st(&b, "/v/in", IN(DELETE) | IN(MASK_ADD), &idD, &existed), OK);
    CHECK(idD == id && existed == 1);
    CHECK_ST(t_mkdir(&r, "/v/in/m"), OK);
    CHECK_ST(t_unlink_flags(&r, "/v/in/m", FS_UNLINK_DIR), OK);
    idD = id;
    EXPECT(&b, "IN_MASK_ADD", { idD, IN(CREATE) | IN(ISDIR), "m" },
           { idD, IN(DELETE) | IN(ISDIR), "m" });
    CHECK_ST(add_st(&b, "/v/in", IN(DELETE), &id, &existed), OK);
    CHECK_ST(t_mkdir(&r, "/v/in/m"), OK);
    CHECK_ST(t_unlink_flags(&r, "/v/in/m", FS_UNLINK_DIR), OK);
    EXPECT(&b, "a mask replaced", { idD, IN(DELETE) | IN(ISDIR), "m" });

    unwatcher(&b);
    unwatcher(&s);
    jam_handle_close(sub);
    jam_handle_close(view);
    return stop("watch-views");
}

/* ---- the close rule ------------------------------------------------------------ */

/* Keep extfs busy: a 64 KiB write sent on f, its answer never waited for. */
static bool busy(struct tfile *f, uint32_t txid)
{
    memset(f->buf, 'b', FAT_BUF);
    CHECK_ST(file_write_send(f->ch, txid, 0, FAT_BUF), OK);
    return true;
}

bool t_extfs_watch_order(void)
{
    struct wat w, a;
    struct tfile f1, f2, f3;
    uint32_t done = 0, id = 0;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/o"), OK);
    CHECK(put_file(&r, "/o/f1", "1"));
    CHECK(put_file(&r, "/o/f2", "2"));
    CHECK(put_file(&r, "/o/f3", "3"));

    /* Write and close, add, write and close: one IN_CLOSE_WRITE. extfs is
     * busy with f3's write while f1's write, its close and the add are
     * sent, so the close is folded into f1's packet and comes to the port
     * again only after the add's. */
    CHECK(watcher(r.fs, &w));
    for (int round = 0; round < 3; round++) {
        CHECK_ST(t_open(&r, "/o/f3", FS_WRITE, &f3), OK);
        CHECK_ST(t_open(&r, "/o/f1", FS_WRITE, &f1), OK);
        CHECK(busy(&f3, 3));
        CHECK(busy(&f1, 1));
        t_close(&f1);
        CHECK((id = add(&w, "/o/f1", IN(CLOSE_WRITE))));
        t_close(&f3);
        NOTHING(&w, "a close made before the add");
        CHECK(put_file(&r, "/o/f1", "again"));
        idD = id;
        EXPECT(&w, "a close after the add", { idD, IN(CLOSE_WRITE), "" });
        CHECK_ST(fswatch_remove_until(w.ch, dl(), id), OK);
        EXPECT(&w, "removed", { idD, IN(IGNORED), "" });
    }

    /* A close, then a write on another file: the close's event first. */
    CHECK(watcher(r.fs, &a));
    CHECK((idO = add(&a, "/o", IN(MODIFY) | IN(CLOSE_WRITE))));
    for (int round = 0; round < 3; round++) {
        CHECK_ST(t_open(&r, "/o/f3", FS_WRITE, &f3), OK);
        CHECK_ST(t_open(&r, "/o/f1", FS_WRITE, &f1), OK);
        CHECK_ST(t_open(&r, "/o/f2", FS_WRITE, &f2), OK);
        CHECK(busy(&f3, 3));
        CHECK(busy(&f1, 1));
        t_close(&f1);
        CHECK_ST(t_write(&f2, 0, "x", 1, &done), OK);
        t_close(&f2);
        t_close(&f3);
        EXPECT(&a, "a close before another file's write", { idO, IN(MODIFY), "f3" },
               { idO, IN(MODIFY), "f1" }, { idO, IN(CLOSE_WRITE), "f1" }, { idO, IN(MODIFY), "f2" },
               { idO, IN(CLOSE_WRITE), "f2" }, { idO, IN(CLOSE_WRITE), "f3" });
    }

    unwatcher(&a);
    unwatcher(&w);
    return stop("watch-order");
}

/* ---- the bell, the queue's bound, the caps ------------------------------------- */

bool t_extfs_watch_limits(void)
{
    struct wat a, more[FSW_PER_PROCESS];
    char path[300];
    char name[201];
    const unsigned many = 100;   /* records of 14 + 200 bytes: 16 KiB holds 76 */
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/q"), OK);
    CHECK(watcher(r.fs, &a));
    CHECK((idD = add(&a, "/q", IN(CREATE))));

    /* The bell: rung when the queue gets its first event, not again until
     * it was taken empty. */
    CHECK(!rung(&a));
    CHECK_ST(t_mkdir(&r, "/q/one"), OK);
    CHECK(rung(&a));
    CHECK_ST(jam_event_signal(a.bell, FSWATCH_BELL, 0), OK);
    CHECK_ST(t_mkdir(&r, "/q/two"), OK);
    CHECK(!rung(&a));
    EXPECT(&a, "two", { idD, IN(CREATE) | IN(ISDIR), "one" }, { idD, IN(CREATE) | IN(ISDIR), "two" });

    /* The bound: what fits, then one IN_Q_OVERFLOW (id 0), the rest dropped. */
    memset(name, 'n', 200);
    name[200] = '\0';
    for (unsigned i = 0; i < many; i++) {
        snprintf(path, sizeof(path), "/q/%03u%.197s", i, name);
        CHECK_ST(t_mkdir(&r, path), OK);
    }
    CHECK(take_all(&a));
    unsigned fit = FSWATCH_QUEUE_BYTES / (FSWATCH_RECORD_FIXED + 200);
    if ((unsigned)ngot != fit + 1)
        say_got();
    CHECK_EQ(ngot, fit + 1);
    for (unsigned i = 0; i < fit; i++)
        CHECK(got[i].id == idD && got[i].mask == (IN(CREATE) | IN(ISDIR)) && got[i].len == 200);
    CHECK(got[fit].id == 0 && got[fit].mask == IN(Q_OVERFLOW) && got[fit].len == 0);
    CHECK_ST(t_mkdir(&r, "/q/after"), OK);
    EXPECT(&a, "room again", { idD, IN(CREATE) | IN(ISDIR), "after" });

    /* FSW_PER_PROCESS watcher channels for this process (a with them), the
     * next refused, a second process's taken; one closed frees its share. */
    for (unsigned i = 0; i + 1 < FSW_PER_PROCESS; i++)
        CHECK(watcher(r.fs, &more[i]));
    handle_t ch = HANDLE_INVALID;
    CHECK_ST(fs_watch_until(r.fs, dl(), &ch), ERR_NO_RESOURCES);
    CHECK(run("xwatch-cap", r.fs));
    unwatcher(&more[0]);
    CHECK(watcher(r.fs, &more[0]));
    for (unsigned i = 0; i + 1 < FSW_PER_PROCESS; i++)
        unwatcher(&more[i]);

    unwatcher(&a);
    return stop("watch-limits");
}
