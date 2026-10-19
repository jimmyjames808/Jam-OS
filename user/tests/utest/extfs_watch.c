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
 *   extfs_watch_nameless (DR) setattr of a path that ends in no name ("" on
 *                       a directory channel, ".", ".."): the parent's watch
 *                       hears the directory's name, after a rename too
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
 *                       pid), a second process's still taken; (IR's 5)
 *                       1,024 watches across this process's channels and
 *                       the next refused, a second process's taken
 *   extfs_watch_hidden  (IE2) no event names what the watcher's view hides
 *                       from a listing (etc/ssh, the root's etc and an
 *                       "ETC"); adds on hidden places refused
 *   extfs_watch_reach   (IE2, Q2) a watch whose inode a rename takes out of
 *                       its view's reach ends, with what is under it: a
 *                       subview's above its root, a file moved out, a
 *                       view hiding the keys' into etc/ssh, one hiding etc's
 *                       renamed "Etc." and moved into etc; the subview's
 *                       root moved and a move into etc that the view still
 *                       sees leave them
 *   extfs_watch_deep    (IR's 3) a watch its client made too deep for the
 *                       reach check to place ends when its tree leaves
 *                       the subview, as any out of reach
 *   extfs_watch_cost    (IR's 4) 256 watches 1,000 directories down in a
 *                       subview: renames elsewhere cost what they cost
 *                       with none, and one out of the subview reads their
 *                       shared path once, every watch kept
 *   extfs_restart_watch (IE2, 2.8) extfs ended by its test powers between a
 *                       change and its event, at an fs.watch committed and
 *                       not answered, before a fresh start: watches back
 *                       with IN_Q_OVERFLOW first (none on a remade
 *                       channel), a bell the keeper can't hold closing its
 *                       channel, a fresh start and the end closing every
 *                       channel with its bell rung
 *   extfs_restart_reach (IR's 1) extfs ended between a rename's commit and
 *                       its events: the successor runs its reach check
 *   extfs_restart_giveup (IR's 7) extfs killed and given up: the keeper
 *                       closes the watcher channel and rings its bell
 *   extfs_watch_storm   (IR2) renames while extfs is killed every 200 ms:
 *                       every rename answered, the watch carried on, only
 *                       whole pairs and overflows read
 *   extfs_watch_threads (IR2) 8 threads opening, writing, closing and
 *                       renaming their files while another reads: each
 *                       thread's records in its order (the close rule)
 *   extfs_watch_abuse   (IR2) protocol abuse aimed at extfs: bad first
 *                       messages, malformed and odd requests, floods of
 *                       gives and of takes never read; extfs serves on and
 *                       its handles come back
 *   extfs_watch_unread  (IR2) a watcher that never reads: extfs's pages
 *                       and handles flat once its queue is full
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

/* A watcher channel whose bell has these rights (libc's: the three of
 * watcher's; one without RIGHT_DUPLICATE can't be kept across a restart). */
static bool watcher_with(handle_t fs, struct wat *x, rights_t rights)
{
    handle_t give;
    CHECK_ST(fs_watch_until(fs, dl(), &x->ch), OK);
    CHECK_ST(jam_event_create(&x->bell), OK);
    CHECK_ST(jam_handle_duplicate(x->bell, rights, &give), OK);
    CHECK_ST(fswatch_bell_give(x->ch, give), OK);
    return true;
}

static bool watcher(handle_t fs, struct wat *x)
{
    return watcher_with(fs, x, RIGHT_SIGNAL | RIGHT_TRANSFER | RIGHT_DUPLICATE);
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

/* As expect, but want[loose] may stand anywhere before the first other
 * event of its name: the order of two files' events that no rule ties
 * together (a write sent ahead to keep extfs busy may, on a loaded
 * machine, still be served after the next file's). */
static bool expect_loose(const struct wat *x, const struct want *want, int n, int loose,
                         const char *what)
{
    if (!take_all(x))
        return false;
    bool ok = ngot == n;
    int j = -1;
    for (int i = 0; ok && i < ngot; i++) {
        bool same = got[i].len == L(want[loose].name) && !strcmp(got[i].name, want[loose].name);
        if (same && got[i].id == want[loose].id && got[i].mask == want[loose].mask) {
            j = i;
            break;
        }
        if (same)
            ok = false;   /* another of its events came before it */
    }
    ok = ok && j >= 0;
    for (int i = 0, w = 0; ok && i < ngot; i++) {
        if (i == j)
            continue;
        if (w == loose)
            w++;
        ok = got[i].id == want[w].id && got[i].mask == want[w].mask &&
             got[i].len == L(want[w].name) && !strcmp(got[i].name, want[w].name);
        w++;
    }
    if (!ok) {
        printf("utest: %s: %s: got %d events, want %d (want %d anywhere before its "
               "name's next):\n", utest_cur, what, ngot, n, loose);
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

static bool start_at(uint64_t sectors)
{
    CHECK(ramdisk_create(&xd, sectors));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    return true;
}

static bool start(void)
{
    return start_at(EXT_DISK_SECTORS);
}

static bool stop(const char *name)
{
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, name));
    return ramdisk_destroy(&xd);
}

/* stop, the disk not saved: a hostile run's, whose journal fills 4 MiB of
 * extfs-svc-test's /data and checks nothing the others' don't. */
static bool stop_unsaved(void)
{
    CHECK(fat_stop(&r));
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

/* ---- a change of a path that ends in no name ----------------------------------- */

static status_t chattr(handle_t ch, const char *path, uint32_t mode)
{
    return fs_setattr_until(ch, dl(), path, L(path), 0, FS_ATTR_MODE, mode, 0, 0, 0, 0);
}

/* (DR) futimens, fchmod and fchown on a directory's descriptor (fs.setattr
 * of "" on its directory channel), and a path ending in "." or "..": the
 * parent's watch hears the directory's name, as Linux's (Chromium's
 * FilePathWatcherTest.RecursiveWatch), after a rename too. */
bool t_extfs_watch_nameless(void)
{
    struct wat a;
    handle_t s = HANDLE_INVALID, in = HANDLE_INVALID;
    uint64_t id = 0, vol = 0;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/d"), OK);
    CHECK_ST(t_mkdir(&r, "/e"), OK);
    CHECK_ST(t_mkdir(&r, "/d/s"), OK);
    CHECK_ST(t_mkdir(&r, "/d/s/in"), OK);
    CHECK_ST(fs_opendir_until(r.fs, dl(), "/d/s", 4, 0, &s, &id, &vol), OK);
    CHECK_ST(fs_opendir_until(r.fs, dl(), "/d/s/in", 7, 0, &in, &id, &vol), OK);
    CHECK(watcher(r.fs, &a));
    CHECK((idD = add(&a, "/d", ALL)) && (idE = add(&a, "/e", ALL)) &&
          (idS = add(&a, "/d/s", ALL)));
    NOTHING(&a, "the adds");

    CHECK_ST(chattr(s, "", 0750), OK);
    CHECK_ST(chattr(r.fs, "/d/s/.", 0755), OK);
    CHECK_ST(chattr(in, "..", 0750), OK);
    EXPECT(&a, "the directory itself", { idD, IN(ATTRIB) | IN(ISDIR), "s" },
           { idS, IN(ATTRIB) | IN(ISDIR), "" }, { idD, IN(ATTRIB) | IN(ISDIR), "s" },
           { idS, IN(ATTRIB) | IN(ISDIR), "" }, { idD, IN(ATTRIB) | IN(ISDIR), "s" },
           { idS, IN(ATTRIB) | IN(ISDIR), "" });

    /* Renamed into another directory: its new parent, by its new name. */
    CHECK_ST(t_rename(&r, "/d/s", "/e/t"), OK);
    CHECK(take_all(&a));
    CHECK_ST(chattr(s, "", 0755), OK);
    EXPECT(&a, "renamed", { idE, IN(ATTRIB) | IN(ISDIR), "t" }, { idS, IN(ATTRIB) | IN(ISDIR), "" });

    /* The volume's root has no parent: its own watch alone. */
    uint32_t idR = add(&a, "/", ALL);
    CHECK(idR);
    CHECK_ST(chattr(r.fs, "", 0755), OK);
    EXPECT(&a, "the root", { idR, IN(ATTRIB) | IN(ISDIR), "" });

    jam_handle_close(in);
    jam_handle_close(s);
    unwatcher(&a);
    return stop_unsaved();   /* (extfs-svc-test's /data is nearly full of disks) */
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

/* The second process (mode xwatch-add, the mount's channel as SR_USER): a
 * watcher channel of its own and a watch on /c/128, while the first
 * process holds its whole share of the volume's watches (IR's 5). */
static int add_child(void)
{
    handle_t fs = startup_handle(SR_USER), give;
    struct wat x;
    uint32_t id = 0;
    uint8_t existed = 0;
    if (!fs || fs_watch_until(fs, dl(), &x.ch) != OK || jam_event_create(&x.bell) != OK ||
        jam_handle_duplicate(x.bell, RIGHT_SIGNAL | RIGHT_TRANSFER, &give) != OK ||
        fswatch_bell_give(x.ch, give) != OK)
        return 1;
    status_t st = fswatch_add_until(x.ch, dl(), "/c/128", 6, IN(CREATE), &id, &existed);
    return st == OK && id ? 0 : st == ERR_NO_SPACE ? 2 : 3;
}

int extfs_watch_child(int argc, char **argv)
{
    const char *m = argc >= 2 ? argv[1] : "";
    if (!strcmp(m, "xwatch-change"))
        return change_child();
    if (!strcmp(m, "xwatch-cap"))
        return cap_child();
    if (!strcmp(m, "xwatch-add"))
        return add_child();
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
    /* (the removed watch's queued events stay, before its IN_IGNORED: Linux's) */
    EXPECT(&s, "IN_ONESHOT and a remove", { idIN, IN(OPEN), "h" }, { idIN, IN(MODIFY), "h" },
           { idH, IN(MODIFY), "" }, { idH, IN(IGNORED), "" }, { idIN, IN(MODIFY), "h" },
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
        /* The rule's promise is f1's close before f2's write, and f2's
         * close before f3's; f3's IN_MODIFY (its busy write) may land
         * anywhere before f3's close. */
        const struct want w_[] = { { idO, IN(MODIFY), "f3" },      { idO, IN(MODIFY), "f1" },
                                   { idO, IN(CLOSE_WRITE), "f1" }, { idO, IN(MODIFY), "f2" },
                                   { idO, IN(CLOSE_WRITE), "f2" }, { idO, IN(CLOSE_WRITE), "f3" } };
        if (!expect_loose(&a, w_, 6, 0, "a close before another file's write"))
            return false;
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

    /* FSW_PROCESS_WATCHES on all of this process's channels (IR's 5: one
     * process can't take the volume's): 128 directories on each of the 7
     * more channels and 127 on a (with its /q), the next refused on any,
     * a second process's taken; one removed makes room. */
    _Static_assert(FSW_PROCESS_WATCHES == 1 + 128 * FSW_PER_PROCESS - 1, "the share filled");
    CHECK_ST(t_mkdir(&r, "/c"), OK);
    for (unsigned j = 0; j <= 128; j++) {
        snprintf(path, sizeof(path), "/c/%u", j);
        CHECK_ST(t_mkdir(&r, path), OK);
    }
    uint32_t first = 0;
    for (unsigned i = 0; i < FSW_PER_PROCESS; i++) {
        const struct wat *x = i + 1 < FSW_PER_PROCESS ? &more[i] : &a;
        for (unsigned j = 0; j < (x == &a ? 127u : 128u); j++) {
            snprintf(path, sizeof(path), "/c/%u", j);
            uint32_t id = add(x, path, IN(CREATE));
            CHECK(id);
            first = first ? first : id;
        }
    }
    uint32_t id = 0;
    CHECK_ST(add_st(&a, "/c/127", IN(CREATE), &id, NULL), ERR_NO_SPACE);
    CHECK_ST(add_st(&more[0], "/c/128", IN(CREATE), &id, NULL), ERR_NO_SPACE);
    CHECK(run("xwatch-add", r.fs));
    CHECK_ST(fswatch_remove_until(more[0].ch, dl(), first), OK);
    CHECK_ST(add_st(&a, "/c/127", IN(CREATE), &id, NULL), OK);
    for (unsigned i = 0; i + 1 < FSW_PER_PROCESS; i++)
        unwatcher(&more[i]);

    unwatcher(&a);
    return stop("watch-limits");
}

/* ---- what a view hides (2.6) --------------------------------------------------- */

bool t_extfs_watch_hidden(void)
{
    struct wat k, e, a;
    handle_t vk, ve;
    uint32_t id = 0;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/etc"), OK);

    /* A view hiding the keys (etc/ssh) and one hiding all of etc, watching
     * the root and etc; the whole volume's watcher on etc. */
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_HIDE_KEYS, &vk), OK);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_NO_ETC, &ve), OK);
    CHECK(watcher(vk, &k) && watcher(ve, &e) && watcher(r.fs, &a));
    uint32_t kRoot = add(&k, "/", ALL), kEtc = add(&k, "/etc", ALL), eRoot = add(&e, "/", ALL);
    uint32_t aEtc = add(&a, "/etc", ALL);
    CHECK(kRoot && kEtc && eRoot && aEtc);

    /* etc/ssh made, a file in etc, etc's mode, a second etc ("ETC" is etc
     * to the views), a name anyone sees, ssh renamed. */
    CHECK_ST(t_mkdir(&r, "/etc/ssh"), OK);
    CHECK(put_file(&r, "/etc/conf", "c"));
    CHECK_ST(setattr("/etc", FS_ATTR_MODE, 0755), OK);
    CHECK_ST(t_mkdir(&r, "/ETC"), OK);
    CHECK_ST(t_mkdir(&r, "/pub"), OK);
    /* Adds on what a view hides: refused, as any request there. */
    CHECK_ST(add_st(&k, "/etc/ssh", ALL, &id, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(add_st(&e, "/etc", ALL, &id, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(add_st(&e, "/ETC", ALL, &id, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(add_st(&e, "/etc/conf", ALL, &id, NULL), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&r, "/etc/ssh", "/etc/keys"), OK);

    /* No name the view hides from a listing: ssh for k (its rename's
     * IN_MOVED_FROM too), etc and ETC in the root for e. */
    EXPECT(&k, "a view hiding the keys", { kEtc, IN(CREATE), "conf" }, { kEtc, IN(OPEN), "conf" },
           { kEtc, IN(MODIFY), "conf" }, { kEtc, IN(CLOSE_WRITE), "conf" },
           { kRoot, IN(ATTRIB) | IN(ISDIR), "etc" }, { kEtc, IN(ATTRIB) | IN(ISDIR), "" },
           { kRoot, IN(CREATE) | IN(ISDIR), "ETC" }, { kRoot, IN(CREATE) | IN(ISDIR), "pub" },
           { kEtc, IN(MOVED_TO) | IN(ISDIR), "keys" });
    EXPECT(&e, "a view hiding etc", { eRoot, IN(CREATE) | IN(ISDIR), "pub" });
    EXPECT(&a, "the whole volume's", { aEtc, IN(CREATE) | IN(ISDIR), "ssh" },
           { aEtc, IN(CREATE), "conf" }, { aEtc, IN(OPEN), "conf" }, { aEtc, IN(MODIFY), "conf" },
           { aEtc, IN(CLOSE_WRITE), "conf" }, { aEtc, IN(ATTRIB) | IN(ISDIR), "" },
           { aEtc, IN(MOVED_FROM) | IN(ISDIR), "ssh" }, { aEtc, IN(MOVED_TO) | IN(ISDIR), "keys" });

    unwatcher(&k);
    unwatcher(&e);
    unwatcher(&a);
    jam_handle_close(vk);
    jam_handle_close(ve);
    return stop("watch-hidden");
}

/* ---- a watch out of its view's reach (Q2) --------------------------------------- */

bool t_extfs_watch_reach(void)
{
    struct wat s, k, e;
    handle_t sub, vk, ve;
    CHECK(start());
    static const char *const dirs[] = { "/v", "/v/a", "/v/a/b", "/v/c", "/etc", "/etc/ssh",
                                        "/w", "/w/d", "/w/d/e", "/p", "/q" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        CHECK_ST(t_mkdir(&r, dirs[i]), OK);
    CHECK(put_file(&r, "/v/a/f", "f") && put_file(&r, "/v/g", "g"));
    CHECK_ST(fs_subview_until(r.fs, dl(), "/v", 2, 0, &sub), OK);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_HIDE_KEYS, &vk), OK);
    CHECK_ST(fs_view_until(r.fs, dl(), FS_VIEW_NO_ETC, &ve), OK);
    CHECK(watcher(sub, &s) && watcher(vk, &k) && watcher(ve, &e));
    uint32_t idA = add(&s, "/a", ALL), idAB = add(&s, "/a/b", ALL), idAF = add(&s, "/a/f", ALL);
    uint32_t idG = add(&s, "/g", ALL), idC = add(&s, "/c", ALL);
    uint32_t idKD = add(&k, "/w/d", ALL), idKE = add(&k, "/w/d/e", ALL);
    uint32_t idP = add(&e, "/p", ALL), idQ = add(&e, "/q", ALL);
    CHECK(idA && idAB && idAF && idG && idC && idKD && idKE && idP && idQ);

    /* A subview at /v: a directory moved above its root ends its watch
     * (the rename's IN_MOVE_SELF, then IN_IGNORED) and those under it
     * (IN_MOVE_SELF, IN_IGNORED); a file moved out, the same; the root
     * itself moved: its watches carry on. */
    CHECK_ST(t_rename(&r, "/v/a", "/a2"), OK);
    EXPECT(&s, "a directory moved above the subview's root", { idA, IN(MOVE_SELF), "" },
           { idA, IN(IGNORED), "" }, { idAB, IN(MOVE_SELF), "" }, { idAB, IN(IGNORED), "" },
           { idAF, IN(MOVE_SELF), "" }, { idAF, IN(IGNORED), "" });
    CHECK_ST(t_rename(&r, "/v/g", "/w/g"), OK);
    EXPECT(&s, "a file moved out", { idG, IN(MOVE_SELF), "" }, { idG, IN(IGNORED), "" });
    CHECK_ST(t_rename(&r, "/v", "/vv"), OK);
    CHECK_ST(t_mkdir(&r, "/vv/c/n"), OK);
    EXPECT(&s, "the root moved", { idC, IN(CREATE) | IN(ISDIR), "n" });

    /* A view hiding the keys: into etc, still seen; into etc/ssh, ended
     * with what is under it. */
    CHECK_ST(t_rename(&r, "/w", "/etc/w"), OK);
    CHECK_ST(t_mkdir(&r, "/etc/w/d/n"), OK);
    EXPECT(&k, "moved into etc", { idKD, IN(CREATE) | IN(ISDIR), "n" });
    CHECK_ST(t_rename(&r, "/etc/w/d", "/etc/ssh/d"), OK);
    EXPECT(&k, "moved into etc/ssh", { idKD, IN(MOVE_SELF), "" }, { idKD, IN(IGNORED), "" },
           { idKE, IN(MOVE_SELF), "" }, { idKE, IN(IGNORED), "" });

    /* A view hiding etc: renamed in the root to a name that is etc to the
     * views ("Etc."), and moved into etc. */
    CHECK_ST(t_rename(&r, "/p", "/Etc."), OK);
    CHECK_ST(t_rename(&r, "/q", "/etc/q"), OK);
    EXPECT(&e, "renamed etc, moved into etc", { idP, IN(MOVE_SELF), "" },
           { idP, IN(IGNORED), "" }, { idQ, IN(MOVE_SELF), "" }, { idQ, IN(IGNORED), "" });
    NOTHING(&s, "the subview's after moves elsewhere");

    unwatcher(&s);
    unwatcher(&k);
    unwatcher(&e);
    jam_handle_close(sub);
    jam_handle_close(vk);
    jam_handle_close(ve);
    return stop("watch-reach");
}

/* ---- watches deep down (IR's 3 and 4) ---------------------------------------------- */

#define DEEP_SECTORS (64u * MIB_SECTORS)   /* 4,096 inodes (32 MiB has 2,048) */

/* dir/top made 32 directories deeper, every path short: a chain c/a/.../a
 * of 32 made in dir, top moved to its bottom, the chain renamed top. */
static bool deepen(const char *dir, const char *top)
{
    char chain[100], from[40], into[120];
    int n = snprintf(chain, sizeof(chain), "%s/c", dir);
    CHECK_ST(t_mkdir(&r, chain), OK);
    for (int i = 1; i < 32; i++) {
        n += snprintf(chain + n, sizeof(chain) - (size_t)n, "/a");
        CHECK_ST(t_mkdir(&r, chain), OK);
    }
    snprintf(from, sizeof(from), "%s/%s", dir, top);
    snprintf(into, sizeof(into), "%s/%s", chain, top);
    CHECK_ST(t_rename(&r, from, into), OK);
    snprintf(chain, sizeof(chain), "%s/c", dir);
    CHECK_ST(t_rename(&r, chain, from), OK);
    return true;
}

/* A subview's client makes its own watch too deep for the reach check to
 * place (2,048 directories): w watched, then its tree deepened inside the
 * subview (no move judged). The tree moved above the subview's root must
 * end the watch all the same, not leave it reporting from outside. */
bool t_extfs_watch_deep(void)
{
    struct wat s;
    struct tfile f;
    handle_t sub;
    uint32_t done = 0;
    CHECK(start_at(DEEP_SECTORS));
    CHECK_ST(t_mkdir(&r, "/h"), OK);
    CHECK_ST(t_mkdir(&r, "/h/t"), OK);
    CHECK_ST(t_mkdir(&r, "/h/t/w"), OK);
    CHECK(put_file(&r, "/h/t/w/f", "f"));
    CHECK_ST(fs_subview_until(r.fs, dl(), "/h", 2, 0, &sub), OK);
    CHECK(watcher(sub, &s));
    uint32_t idW = add(&s, "/t/w", ALL);
    CHECK(idW);
    CHECK_ST(t_open(&r, "/h/t/w/f", FS_WRITE, &f), OK);
    CHECK(take_all(&s));   /* f's open */
    for (int i = 0; i < 64; i++)   /* w: 3 + 64 * 32 below the volume's root */
        CHECK(deepen("/h", "t"));
    NOTHING(&s, "w's tree made deeper inside the subview");
    CHECK_ST(t_rename(&r, "/h/t", "/out"), OK);
    CHECK_ST(t_write(&f, 0, "x", 1, &done), OK);
    t_close(&f);
    EXPECT(&s, "a watch too deep to place, its tree moved out", { idW, IN(MOVE_SELF), "" },
           { idW, IN(IGNORED), "" });
    unwatcher(&s);
    jam_handle_close(sub);
    return stop("watch-deep");
}

/* ns that rounds renames of /x/f to /x/g and back take. */
static uint64_t renames_ns(unsigned rounds)
{
    uint64_t t0 = now();
    for (unsigned i = 0; i < rounds; i++)
        if (t_rename(&r, "/x/f", "/x/g") != OK || t_rename(&r, "/x/g", "/x/f") != OK)
            return UINT64_MAX;
    return now() - t0;
}

/* The reach check's cost: 256 watches some 1,000 directories down in a
 * subview. Renames elsewhere on the volume must cost what they cost with
 * no watch (the subview's channel isn't looked at), and one out of the
 * subview the 256 watches' shared path once, not 256 times (the watches
 * all stay: none was under what moved). */
bool t_extfs_watch_cost(void)
{
    struct wat s;
    handle_t sub;
    char path[40];
    const unsigned many = 256, rounds = 4;
    CHECK(start_at(DEEP_SECTORS));
    CHECK_ST(t_mkdir(&r, "/h"), OK);
    CHECK_ST(t_mkdir(&r, "/h/t"), OK);
    CHECK_ST(t_mkdir(&r, "/h/s"), OK);
    CHECK_ST(t_mkdir(&r, "/x"), OK);
    CHECK(put_file(&r, "/x/f", "f"));
    for (unsigned i = 0; i < many; i++) {
        snprintf(path, sizeof(path), "/h/t/b%03u", i);
        CHECK_ST(t_mkdir(&r, path), OK);
    }
    CHECK_ST(fs_subview_until(r.fs, dl(), "/h", 2, 0, &sub), OK);
    CHECK(watcher(sub, &s));
    uint64_t base = renames_ns(rounds);
    for (unsigned i = 0; i < many; i++) {
        snprintf(path, sizeof(path), "/t/b%03u", i);
        CHECK(add(&s, path, ALL));
    }
    for (int i = 0; i < 31; i++)   /* the b's: 3 + 31 * 32 below the volume's root */
        CHECK(deepen("/h", "t"));
    uint64_t with = renames_ns(rounds);
    printf("utest: %s: %u renames elsewhere: %lu us with no watch, %lu us with %u deep ones\n",
           utest_cur, 2 * rounds, (unsigned long)(base / NS_PER_US),
           (unsigned long)(with / NS_PER_US), many);
    CHECK(base != UINT64_MAX && with <= 3 * base + 50 * NS_PER_MS);
    uint64_t t0 = now();
    CHECK_ST(t_rename(&r, "/h/s", "/s2"), OK);
    uint64_t out = now() - t0;
    printf("utest: %s: a directory moved out of the subview: %lu us\n", utest_cur,
           (unsigned long)(out / NS_PER_US));
    /* The move reads the deep path once: about 25 times what 8 plain renames
     * take (7 ms against 0.28 ms with KVM; 101 ms against 4.8 ms under TCG,
     * 87 ms against 4.6 ms on a loaded PC: the ratio holds, the milliseconds
     * don't). A bound of 100 times the base and 50 ms over passes all three,
     * and fails a check that reads the path for each of the 256 watches
     * (256 times the one read: 1.8 s with KVM, 25 s under TCG). */
    CHECK(out <= 100 * base + 50 * NS_PER_MS);
    NOTHING(&s, "none of the deep watches was under what moved");
    unwatcher(&s);
    jam_handle_close(sub);
    return stop("watch-cost");
}

/* ---- restarts (2.8) ------------------------------------------------------------ */

/* x's channel is closed: a take answers ERR_PEER_CLOSED (once the keeper
 * dropped its copy: the supervisor takes what extfs sends it every 50 ms). */
static bool closed(const struct wat *x)
{
    uint8_t more = 0;
    uint32_t n = 0;
    return fswatch_take_until(x->ch, dl(), FSWATCH_TAKE_BYTES, &more, takebuf, &n) ==
           ERR_PEER_CLOSED;
}

/* extfs ended by its test powers (utest's restart supervisor, extfs_restart.c)
 * with watches on: instance 0 after a change is in the journal and before
 * its event, instance 1 with an fs.watch committed and not answered,
 * instance 2 with a watcher's take committed and not answered, its
 * successor handed a new state (a fresh start). The comments count each
 * instance's requests for its power. */
bool t_extfs_restart_watch(void)
{
    static const char *const dies[] = { "die=journal:3", "die=commit:5", "die=commit:7" };
    struct wat a, b, c, o;
    uint32_t id = 0, idB = 0;
    uint8_t existed = 0;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&xd, dies, 3));
    struct fatrun *s = extfs_sup_run();

    /* Instance 0: a watches /d and /d/s; o (a bell without RIGHT_DUPLICATE)
     * watches /d. The mkdir of /d/x ends it, answered by its successor. */
    CHECK_ST(t_mkdir(s, "/d"), OK);     /* journal 1 */
    CHECK_ST(t_mkdir(s, "/d/s"), OK);   /* journal 2 */
    CHECK(watcher(s->fs, &a) && watcher_with(s->fs, &o, RIGHT_SIGNAL | RIGHT_TRANSFER));
    CHECK((idD = add(&a, "/d", ALL)) && (idS = add(&a, "/d/s", ALL)) && add(&o, "/d", ALL));
    CHECK_ST(t_mkdir(s, "/d/x"), OK);   /* journal 3: ended */
    CHECK_EQ(extfs_sup_ended(), 1u);

    /* Instance 1: a's watches back, its queue starting with the overflow
     * (x's IN_CREATE died with instance 0), its bell rung; then events as
     * before. o's channel closed: its bell couldn't be kept. */
    CHECK(rung(&a));
    EXPECT(&a, "after a death between a change and its event", { 0, IN(Q_OVERFLOW), "" }); /* 1 */
    CHECK_ST(add_st(&a, "/d", ALL, &id, &existed), OK);   /* 2 */
    CHECK(id == idD && existed == 1);
    CHECK_ST(t_mkdir(s, "/d/y"), OK);   /* 3 */
    EXPECT(&a, "a watch carried on", { idD, IN(CREATE) | IN(ISDIR), "y" });   /* 4 */
    CHECK(closed(&o));

    /* fs.watch committed, not answered (5: instance 1 ended): its successor
     * makes the client's end again. It had no bell, so no overflow. */
    CHECK(watcher(s->fs, &b));
    CHECK_EQ(extfs_sup_ended(), 2u);
    CHECK((idB = add(&b, "/d/s", ALL)));   /* instance 2: 1 */
    NOTHING(&b, "a remade channel");       /* 2 */
    CHECK_ST(t_mkdir(s, "/d/s/z"), OK);    /* 3 */
    EXPECT(&b, "a remade channel's watch", { idB, IN(CREATE) | IN(ISDIR), "z" });   /* 4 */
    EXPECT(&a, "a after a second restart", { 0, IN(Q_OVERFLOW), "" },
           { idS, IN(CREATE) | IN(ISDIR), "z" });   /* 5 */

    /* A fresh start (instance 2 ended at a take of a's, committed, not
     * answered; the next is handed a new state): every watcher channel
     * closed (that take's too), its bell rung first. */
    extfs_sup_fresh();
    CHECK_ST(t_stat(s, "/d", NULL, NULL, NULL), OK);   /* 6 */
    CHECK(closed(&a));                                 /* 7: ended */
    CHECK_ST(t_stat(s, "/d", NULL, NULL, NULL), OK);   /* instance 3's */
    CHECK_EQ(extfs_sup_ended(), 3u);
    CHECK(rung(&a) && rung(&b));
    CHECK(closed(&b));

    /* The fresh instance's watches, no overflow; the mount going (extfs
     * stopped) closes them, the bell rung. */
    CHECK(watcher(s->fs, &c));
    CHECK((id = add(&c, "/d", ALL)));
    CHECK_ST(t_mkdir(s, "/d/w"), OK);
    EXPECT(&c, "a fresh instance's watch", { id, IN(CREATE) | IN(ISDIR), "w" });
    CHECK(extfs_sup_stop());
    CHECK(rung(&c) && closed(&c));
    unwatcher(&a);
    unwatcher(&b);
    unwatcher(&c);
    unwatcher(&o);
    CHECK(extfs_save(&xd, "restart-watch"));
    return ramdisk_destroy(&xd);
}

/* IR's 1: extfs ended between a rename's commit in the journal and its
 * events. The successor answers the rename from the journal, and must
 * still run its reach check: the subview's watch on the directory moved
 * above its root ends (after the overflow), and hears nothing from there. */
bool t_extfs_restart_reach(void)
{
    static const char *const dies[] = { "die=journal:3" };
    struct wat s;
    handle_t sub;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&xd, dies, 1));
    struct fatrun *f = extfs_sup_run();
    CHECK_ST(t_mkdir(f, "/v"), OK);     /* journal 1 */
    CHECK_ST(t_mkdir(f, "/v/a"), OK);   /* journal 2 */
    CHECK_ST(fs_subview_until(f->fs, dl(), "/v", 2, 0, &sub), OK);
    CHECK(watcher(sub, &s));
    uint32_t idA = add(&s, "/a", ALL);
    CHECK(idA);
    CHECK_ST(t_rename(f, "/v/a", "/a2"), OK);   /* journal 3: ended */
    CHECK_EQ(extfs_sup_ended(), 1u);
    CHECK(rung(&s));
    EXPECT(&s, "a rename's reach check, its instance dead before it",
           { 0, IN(Q_OVERFLOW), "" }, { idA, IN(IGNORED), "" });
    CHECK_ST(t_mkdir(f, "/a2/n"), OK);
    NOTHING(&s, "the directory moved out, after the restart");
    CHECK(extfs_sup_stop());
    unwatcher(&s);
    jam_handle_close(sub);
    CHECK(extfs_save(&xd, "restart-reach"));
    return ramdisk_destroy(&xd);
}

/* IR's 7: a dead extfs its supervisor gives up (as volumes does past its
 * restart limit) has its kept channels closed by the keeper, which rings
 * each kept bell (keepch.put's ring): a client waiting on its bell alone
 * (poll, epoll) wakes to find its channel closed. */
bool t_extfs_restart_giveup(void)
{
    struct wat a;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&xd, NULL, 0));
    struct fatrun *f = extfs_sup_run();
    CHECK_ST(t_mkdir(f, "/d"), OK);
    CHECK(watcher(f->fs, &a));
    uint32_t id = add(&a, "/d", ALL);
    CHECK(id);
    CHECK_ST(t_mkdir(f, "/d/x"), OK);
    EXPECT(&a, "before", { id, IN(CREATE) | IN(ISDIR), "x" });
    CHECK(!rung(&a));
    CHECK(extfs_sup_give_up(dl()));
    CHECK_ST(jam_object_wait_one(a.bell, FSWATCH_BELL, now() + 5 * NS_PER_S, NULL), OK);
    CHECK(closed(&a));
    CHECK(extfs_sup_stop());
    unwatcher(&a);
    return ramdisk_destroy(&xd);
}

/* ---- hostile runs (IR2: the plan's IR row) ------------------------------------- */

/* A reader thread: takes what watcher x gets as it comes, until told to
 * stop and nothing more came for 20 ms, handing each record to check
 * (false: against the rules). */
struct rd {
    const struct wat *x;
    bool    (*check)(void *ctx, const struct rec *e);
    void     *ctx;
    bool      stop;
    bool      bad;          /* a take failed or a record broke the rules */
    bool      closed;       /* the channel closed under it */
    unsigned  records, overflows;
    handle_t  thread;
    struct rec e;
    uint8_t   buf[FSWATCH_TAKE_BYTES];
    _Alignas(16) uint8_t stack[32 << 10];
};

static bool rd_take(struct rd *d)
{
    uint8_t more = 1;
    CHECK_ST(jam_event_signal(d->x->bell, FSWATCH_BELL, 0), OK);
    while (more) {
        uint32_t n = 0;
        status_t st = fswatch_take_until(d->x->ch, dl(), FSWATCH_TAKE_BYTES, &more, d->buf, &n);
        if (st == ERR_PEER_CLOSED) {
            d->closed = true;
            return false;
        }
        CHECK_ST(st, OK);
        for (uint32_t o = 0; o < n;) {
            struct rec *e = &d->e;
            uint16_t len;
            CHECK(n - o >= FSWATCH_RECORD_FIXED);
            memcpy(&e->id, d->buf + o, 4);
            memcpy(&e->mask, d->buf + o + 4, 4);
            memcpy(&e->cookie, d->buf + o + 8, 4);
            memcpy(&len, d->buf + o + 12, 2);
            o += FSWATCH_RECORD_FIXED;
            CHECK(len < sizeof(e->name) && n - o >= len);
            e->len = len;
            memcpy(e->name, d->buf + o, len);
            e->name[len] = '\0';
            o += len;
            d->records++;
            d->overflows += e->id == 0 && e->mask == IN(Q_OVERFLOW);
            if (!d->check(d->ctx, e))
                FAIL("record %u against the rules: id %u mask %#x cookie %u \"%s\"", d->records,
                     e->id, e->mask, e->cookie, e->name);
        }
    }
    return true;
}

static void reader(void *arg)
{
    struct rd *d = arg;
    for (;;) {
        bool stop = __atomic_load_n(&d->stop, __ATOMIC_ACQUIRE);
        if (jam_object_wait_one(d->x->bell, FSWATCH_BELL, now() + 20 * NS_PER_MS, NULL) != OK) {
            if (stop)
                return;
            continue;
        }
        if (!rd_take(d)) {
            d->bad = !d->closed;
            return;
        }
    }
}

static bool rd_start(struct rd *d, const struct wat *x, bool (*check)(void *, const struct rec *),
                     void *ctx)
{
    d->x = x;
    d->check = check;
    d->ctx = ctx;
    d->stop = d->bad = d->closed = false;
    d->records = d->overflows = 0;
    CHECK_ST(thread_spawn("watch reader", reader, d, d->stack, sizeof(d->stack), &d->thread), OK);
    return true;
}

static bool rd_stop(struct rd *d)
{
    __atomic_store_n(&d->stop, true, __ATOMIC_RELEASE);
    CHECK(wait_threads(&d->thread, 1));
    CHECK(!d->closed);
    CHECK(!d->bad);
    return true;
}

/* The storm's rule: a and b in /s moved back and forth make pairs, each
 * IN_MOVED_FROM followed at once by its IN_MOVED_TO (its cookie, the
 * other name); a restart's IN_Q_OVERFLOW (the dead instance's queue lost)
 * may come between two pairs or cut one after its IN_MOVED_FROM, nothing
 * else. */
struct pairs {
    uint32_t id, cookie;
    char     from;
    unsigned pairs, cut;
};

static bool pair_check(void *ctx, const struct rec *e)
{
    struct pairs *p = ctx;
    if (e->id == 0 && e->mask == IN(Q_OVERFLOW) && !e->len) {
        p->cut += p->cookie != 0;
        p->cookie = 0;
        return true;
    }
    if (e->id != p->id || e->len != 1 || (e->name[0] != 'a' && e->name[0] != 'b') || !e->cookie)
        return false;
    if (e->mask == IN(MOVED_FROM) && !p->cookie) {
        p->cookie = e->cookie;
        p->from = e->name[0];
        return true;
    }
    if (e->mask == IN(MOVED_TO) && e->cookie == p->cookie && e->name[0] != p->from) {
        p->cookie = 0;
        p->pairs++;
        return true;
    }
    return false;
}

/* Is /s/a there (else /s/b is), asked again through a death or two. */
static status_t where_a(const struct fatrun *f, bool *at_a)
{
    status_t st = ERR_IO;
    for (int i = 0; i < 3 && st == ERR_IO; i++)
        st = t_stat(f, "/s/a", NULL, NULL, NULL);
    *at_a = st == OK;
    return st == ERR_NOT_FOUND ? OK : st;
}

#define STORM_EVERY_MS 200u
#define STORM_KILLS    15u
#define STORM_MAX_NS   (60 * NS_PER_S)

/* A rename storm while the supervisor kills extfs every 200 ms (as WR's
 * whostile kills extfs-jam with initctl.kill), a reader taking as it goes:
 * every rename answered (OK, or ERR_IO for one in progress at two deaths
 * in a row: extfs_restart's R2), the watch carried across every restart
 * (its channel never closed), its records only pairs and overflows; after
 * the storm a rename makes exactly one pair. */
bool t_extfs_watch_storm(void)
{
    static struct rd d;
    static struct pairs p;
    struct wat a;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&xd, NULL, 0));
    struct fatrun *f = extfs_sup_run();
    CHECK_ST(t_mkdir(f, "/s"), OK);
    CHECK(put_file(f, "/s/a", "a"));
    CHECK(watcher(f->fs, &a));
    memset(&p, 0, sizeof(p));
    CHECK((p.id = add(&a, "/s", IN(MOVED_FROM) | IN(MOVED_TO))));
    CHECK(rd_start(&d, &a, pair_check, &p));
    extfs_sup_storm(STORM_EVERY_MS * NS_PER_MS);
    bool at_a = true;
    unsigned done = 0, io = 0;
    status_t st = OK;
    uint64_t t0 = now();
    while (st == OK && extfs_sup_crashes() < STORM_KILLS && now() - t0 < STORM_MAX_NS) {
        st = t_rename(f, at_a ? "/s/a" : "/s/b", at_a ? "/s/b" : "/s/a");
        if (st == OK) {
            at_a = !at_a;
            done++;
        } else if (st == ERR_IO) {
            io++;
            st = where_a(f, &at_a);
        }
    }
    extfs_sup_storm(0);
    jam_sleep_until(now() + 100 * NS_PER_MS);   /* a kill the supervisor had timed already */
    status_t up = t_stat(f, "/s", NULL, NULL, NULL);   /* an instance serves: its overflow queued */
    unsigned kills = extfs_sup_crashes();
    bool rd_ok = rd_stop(&d);
    printf("utest: %s: %u renames (%u ERR_IO) in %lu ms, extfs killed %u times; %u records: "
           "%u pairs, %u cut by an overflow, %u overflows\n", utest_cur, done, io,
           (unsigned long)((now() - t0) / NS_PER_MS), kills, d.records, p.pairs, p.cut,
           d.overflows);
    CHECK_ST(st, OK);
    CHECK_ST(up, OK);
    CHECK(rd_ok);
    CHECK(kills >= STORM_KILLS && done >= STORM_KILLS);
    CHECK(p.pairs + p.cut <= done + io && p.pairs > 0);

    /* After the storm: the watch carries on, one pair for one rename. */
    CHECK_ST(where_a(f, &at_a), OK);
    CHECK_ST(t_rename(f, at_a ? "/s/a" : "/s/b", at_a ? "/s/b" : "/s/a"), OK);
    EXPECT(&a, "after the storm", { p.id, IN(MOVED_FROM), at_a ? "a" : "b" },
           { p.id, IN(MOVED_TO), at_a ? "b" : "a" });
    CHECK(paired(0, 1));
    CHECK(extfs_sup_stop());
    unwatcher(&a);
    CHECK(extfs_save(&xd, "watch-storm"));
    return ramdisk_destroy(&xd);
}

#define CR_THREADS 8u
#define CR_ROUNDS  60u
#define CR_CREATE  7     /* a thread's first record: its file's IN_CREATE */
#define CR_LOST    (-1)  /* after an overflow: in step again at its next IN_OPEN */

/* A round's records of one thread, in its order: the close's before the
 * next request's (the close rule, 2.3), each pair whole. */
static const struct {
    uint32_t mask;
    char     name;
} cr_steps[] = {
    { IN(OPEN), 'f' },       { IN(MODIFY), 'f' },   { IN(CLOSE_WRITE), 'f' },
    { IN(MOVED_FROM), 'f' }, { IN(MOVED_TO), 'g' }, { IN(MOVED_FROM), 'g' },
    { IN(MOVED_TO), 'f' },
};

struct crstate {
    uint32_t id;
    uint32_t cookie;            /* an IN_MOVED_FROM's, its IN_MOVED_TO next */
    int      step[CR_THREADS];  /* each thread's next record: cr_steps[], CR_CREATE, CR_LOST */
    unsigned rounds[CR_THREADS];
};

static bool cr_check(void *ctx, const struct rec *e)
{
    struct crstate *s = ctx;
    if (e->id == 0 && e->mask == IN(Q_OVERFLOW) && !e->len) {
        for (unsigned i = 0; i < CR_THREADS; i++)
            s->step[i] = CR_LOST;
        s->cookie = 0;
        return true;
    }
    if (e->id != s->id || e->len != 2 || (e->name[0] != 'f' && e->name[0] != 'g') ||
        e->name[1] < '0' || e->name[1] >= (char)('0' + CR_THREADS))
        return false;
    if (e->mask == IN(MOVED_TO)) {
        if (!s->cookie || e->cookie != s->cookie)
            return false;
        s->cookie = 0;
    } else if (s->cookie) {
        return false;   /* a pair cut by another record */
    } else if (e->mask == IN(MOVED_FROM)) {
        if (!e->cookie)
            return false;
        s->cookie = e->cookie;
    }
    unsigned i = (unsigned)(e->name[1] - '0');
    int k = s->step[i];
    if (k == CR_LOST) {
        if (e->mask == IN(OPEN) && e->name[0] == 'f')
            s->step[i] = 1;
        return true;
    }
    uint32_t mask = k == CR_CREATE ? IN(CREATE) : cr_steps[k].mask;
    char name = k == CR_CREATE ? 'f' : cr_steps[k].name;
    if (e->mask != mask || e->name[0] != name)
        return false;
    s->step[i] = k == CR_CREATE ? 0 : (k + 1) % 7;
    s->rounds[i] += s->step[i] == 0 && k != CR_CREATE;
    return true;
}

/* One of the 8: its file opened, written, closed, renamed away and back,
 * CR_ROUNDS times. */
struct crw {
    unsigned i;
    status_t st;
    handle_t thread;
    _Alignas(16) uint8_t stack[32 << 10];
};

static void cr_worker(void *arg)
{
    struct crw *w = arg;
    char f[8], g[8];
    snprintf(f, sizeof(f), "/w/f%u", w->i);
    snprintf(g, sizeof(g), "/w/g%u", w->i);
    for (unsigned round = 0; round < CR_ROUNDS && w->st == OK; round++) {
        struct tfile t;
        uint32_t done = 0;
        w->st = t_open(&r, f, FS_WRITE | FS_CREATE, &t);
        if (w->st != OK)
            break;
        w->st = t_write(&t, 0, "x", 1, &done);
        t_close(&t);
        if (w->st == OK)
            w->st = t_rename(&r, f, g);
        if (w->st == OK)
            w->st = t_rename(&r, g, f);
    }
}

/* 8 threads each opening, writing and closing its file, then renaming it
 * away and back, while a reader takes as it goes: each thread's records
 * in its order (an IN_CLOSE_WRITE before the IN_MOVED_FROM its thread's
 * next request makes: the close rule under load), each pair whole, and
 * with no overflow every record there. */
bool t_extfs_watch_threads(void)
{
    static struct rd d;
    static struct crstate s;
    static struct crw w[CR_THREADS];
    handle_t th[CR_THREADS];
    struct wat a;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/w"), OK);
    CHECK(watcher(r.fs, &a));
    memset(&s, 0, sizeof(s));
    CHECK((s.id = add(&a, "/w", ALL)));
    for (unsigned i = 0; i < CR_THREADS; i++)
        s.step[i] = CR_CREATE;
    CHECK(rd_start(&d, &a, cr_check, &s));
    for (unsigned i = 0; i < CR_THREADS; i++) {
        w[i].i = i;
        w[i].st = OK;
        CHECK_ST(thread_spawn("watch writer", cr_worker, &w[i], w[i].stack, sizeof(w[i].stack),
                              &w[i].thread), OK);
        th[i] = w[i].thread;
    }
    CHECK(wait_threads(th, CR_THREADS));
    bool rd_ok = rd_stop(&d);
    printf("utest: %s: %u threads x %u rounds: %u records, %u overflows\n", utest_cur,
           CR_THREADS, CR_ROUNDS, d.records, d.overflows);
    for (unsigned i = 0; i < CR_THREADS; i++)
        CHECK_ST(w[i].st, OK);
    CHECK(rd_ok);
    if (!d.overflows) {
        CHECK_EQ(d.records, CR_THREADS * (1 + 7 * CR_ROUNDS));
        for (unsigned i = 0; i < CR_THREADS; i++)
            CHECK(s.step[i] == 0 && s.rounds[i] == CR_ROUNDS);
    }
    unwatcher(&a);
    return stop_unsaved();
}

/* What extfs did with an abusive message on ch: ended the channel (0),
 * answered it (1, the reply's status in *st), neither within 2 s (2). */
static int outcome(handle_t ch, int32_t *st)
{
    static uint8_t rep[FSWATCH_REP_MAX];
    signals_t seen = 0;
    if (jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, now() + 2 * NS_PER_S, &seen) != OK)
        return 2;
    if (!(seen & SIG_READABLE))
        return 0;
    uint32_t nb = 0, nh = 0;
    handle_t hs[4];
    struct channel_read_args a = {
        .h = ch, .rd_cap = sizeof(rep), .rd_bytes = (uint64_t)(uintptr_t)rep,
        .rd_handles = (uint64_t)(uintptr_t)hs, .rd_hcap = 4,
        .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    *st = ERR_IO;
    if (jam_channel_read(&a) != OK)
        return 0;   /* closed since */
    for (uint32_t i = 0; i < nh; i++)
        jam_handle_close(hs[i]);
    if (nb >= 8)
        memcpy(st, rep + 4, 4);
    return 1;
}

static const char *said(int o, int32_t st)
{
    static char text[64];
    if (o == 1)
        snprintf(text, sizeof(text), "answered %s", status_str(st));
    return o == 0 ? "ended" : o == 1 ? text : "neither ended nor answered";
}

/* A fresh watcher channel's first message, as a hostile client may send
 * it (case c; false: no more cases). Anything but one give of one event
 * with RIGHT_SIGNAL must end the channel (fswatch_bell's rule). */
static bool first_abuse(unsigned c, handle_t ch, const char **what)
{
    struct fswatch_bell_give_msg g = { 0, FSWATCH_BELL_GIVE };
    struct fswatch_take_req t = { 1, FSWATCH_TAKE, 64 };
    handle_t h[2] = { HANDLE_INVALID, HANDLE_INVALID }, x = HANDLE_INVALID;
    (void)jam_event_create(&h[0]);
    switch (c) {
    case 0:
        *what = "a take before the bell";
        jam_handle_close(h[0]);
        (void)jam_channel_write(ch, &t, sizeof(t), NULL, 0);
        return true;
    case 1:
        *what = "the give as a call (a txid)";
        g.txid = 7;
        (void)jam_channel_write(ch, &g, sizeof(g), h, 1);
        return true;
    case 2:
        *what = "a channel for the bell";
        jam_handle_close(h[0]);
        (void)jam_channel_create(&h[0], &x);
        jam_handle_close(x);
        (void)jam_channel_write(ch, &g, sizeof(g), h, 1);
        return true;
    case 3:
        *what = "a bell without RIGHT_SIGNAL";
        (void)jam_handle_replace(h[0], RIGHT_TRANSFER | RIGHT_DUPLICATE | RIGHT_WAIT, &x);
        (void)jam_channel_write(ch, &g, sizeof(g), &x, 1);
        return true;
    case 4:
        *what = "a give of no handle";
        jam_handle_close(h[0]);
        (void)jam_channel_write(ch, &g, sizeof(g), NULL, 0);
        return true;
    case 5:
        *what = "a give of two events";
        (void)jam_event_create(&h[1]);
        (void)jam_channel_write(ch, &g, sizeof(g), h, 2);
        return true;
    case 6:
        *what = "a short give";
        (void)jam_channel_write(ch, &g, 4, h, 1);
        return true;
    case 7:
        *what = "an unknown ordinal, as an event";
        g.ordinal = FSWATCH_BELL_GIVE + 9;
        (void)jam_channel_write(ch, &g, sizeof(g), h, 1);
        return true;
    case 8:
        /* A flood of gives: the first is the bell; each after it is a
         * request of no fswatch ordinal, refused (ERR_INVALID_ARGS, its
         * event closed), the channel kept. */
        *what = "a flood of gives";
        jam_handle_close(h[0]);
        for (unsigned i = 0; i < 1000; i++)
            if (jam_event_create(&x) != OK || jam_channel_write(ch, &g, sizeof(g), &x, 1) != OK)
                break;
        return true;
    default:
        jam_handle_close(h[0]);
        return false;
    }
}

/* A request on a belled channel, malformed as a hostile client may send
 * it (case c; false: no more cases). */
static bool request_abuse(unsigned c, handle_t ch, const char **what)
{
    static uint8_t big[FSWATCH_REQ_MAX + 8192];
    struct fswatch_take_req t = { 3, FSWATCH_TAKE, 64 };
    struct fswatch_bell_give_msg g = { 0, FSWATCH_BELL_GIVE };
    handle_t h = HANDLE_INVALID;
    switch (c) {
    case 0:
        *what = "an unknown ordinal";
        t.ordinal = FSWATCH_TAKE + 9;
        (void)jam_channel_write(ch, &t, sizeof(t), NULL, 0);
        return true;
    case 1:
        *what = "a short take";
        (void)jam_channel_write(ch, &t, 8, NULL, 0);
        return true;
    case 2:
        *what = "a take 8 KiB too long";
        memcpy(big, &t, sizeof(t));
        (void)jam_channel_write(ch, big, sizeof(big), NULL, 0);
        return true;
    case 3:
        *what = "a take carrying an event";
        (void)jam_event_create(&h);
        (void)jam_channel_write(ch, &t, sizeof(t), &h, 1);
        return true;
    case 4:
        *what = "a take with txid 0 (an event's form)";
        t.txid = 0;
        (void)jam_channel_write(ch, &t, sizeof(t), NULL, 0);
        return true;
    case 5:
        *what = "a second bell";
        (void)jam_event_create(&h);
        (void)jam_channel_write(ch, &g, sizeof(g), &h, 1);
        return true;
    case 6:
        *what = "a take of another protocol's ordinal";
        t.ordinal = FSWATCH_TAKE ^ (1u << 16);
        (void)jam_channel_write(ch, &t, sizeof(t), NULL, 0);
        return true;
    default:
        return false;
    }
}

/* The extfs job's handles, once every close made so far is handled (a
 * request served after them: 2.3). */
static bool job_handles(uint64_t *out)
{
    struct job_info ji;
    CHECK_ST(t_stat(&r, "/", NULL, NULL, NULL), OK);
    CHECK_ST(info_of(r.job, &ji), OK);
    *out = ji.used[JOB_LIMIT_HANDLES];
    return true;
}

#define ABUSE_PATH 4095u   /* fswatch.add's longest path */

/* Protocol abuse aimed at extfs itself (the library's own cases are
 * utest fswatch_*'s, over a fake): every first message but the bell ends
 * its channel; malformed requests after the bell are answered with an
 * error or end the channel (each said); odd but well-formed ones (a take
 * of no room or of too much, removes of ids not the channel's, adds of no
 * path, of the longest, with a NUL, of every mask bit) answered; a flood
 * of takes never read. Through all of it extfs serves on, a second
 * watcher's watch is whole, and the job's handles come back to what they
 * were. */
bool t_extfs_watch_abuse(void)
{
    static uint8_t out[FSWATCH_TAKE_BYTES];
    static char longp[ABUSE_PATH];
    struct wat good, y;
    const char *what = "";
    int32_t st = 0;
    uint64_t h0 = 0, h1 = 0;
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/g"), OK);
    CHECK(watcher(r.fs, &good));
    uint32_t id = add(&good, "/g", IN(CREATE));
    CHECK(id);
    CHECK(job_handles(&h0));

    for (unsigned c = 0;; c++) {
        CHECK_ST(fs_watch_until(r.fs, dl(), &y.ch), OK);
        if (!first_abuse(c, y.ch, &what)) {
            jam_handle_close(y.ch);
            break;
        }
        int o = outcome(y.ch, &st);
        printf("utest: %s: first message %s: %s\n", utest_cur, what, said(o, st));
        CHECK(o == 0 || (c == 8 && o == 1 && st == ERR_INVALID_ARGS));
        jam_handle_close(y.ch);
    }
    for (unsigned c = 0;; c++) {
        CHECK(watcher(r.fs, &y));
        CHECK(add(&y, "/g", IN(CREATE)));
        if (!request_abuse(c, y.ch, &what)) {
            unwatcher(&y);
            break;
        }
        int o = outcome(y.ch, &st);
        printf("utest: %s: after the bell, %s: %s\n", utest_cur, what, said(o, st));
        CHECK(o != 1 || st != OK);
        if (c == 4) {   /* a call with txid 0 (INOTIFY-REVIEW L14): dropped, not served into the void */
            uint8_t more = 0;
            uint32_t n = 0;
            CHECK_EQ(o, 2);   /* no answer, and the channel stays */
            CHECK_ST(fswatch_take_until(y.ch, dl(), FSWATCH_TAKE_BYTES, &more, out, &n), OK);
        }
        unwatcher(&y);
        CHECK_ST(t_stat(&r, "/g", NULL, NULL, NULL), OK);
    }

    /* Well-formed and odd: answered, nothing of the good watcher's touched. */
    uint8_t more = 0, existed = 0;
    uint32_t n = 0, got_id = 0;
    CHECK(watcher(r.fs, &y));
    CHECK_ST(fswatch_take_until(y.ch, dl(), 0, &more, out, &n), OK);
    CHECK_ST(fswatch_take_until(y.ch, dl(), UINT32_MAX, &more, out, &n), OK);
    CHECK_ST(fswatch_remove_until(y.ch, dl(), 0), OK);
    CHECK_ST(fswatch_remove_until(y.ch, dl(), UINT32_MAX), OK);
    CHECK_ST(fswatch_remove_until(y.ch, dl(), id), OK);   /* the good watcher's: not y's */
    status_t s1 = fswatch_add_until(y.ch, dl(), "", 0, IN(CREATE), &got_id, &existed);
    memset(longp, 'a', sizeof(longp));
    longp[0] = '/';
    status_t s2 = fswatch_add_until(y.ch, dl(), longp, ABUSE_PATH, IN(CREATE), &got_id, &existed);
    status_t s3 = fswatch_add_until(y.ch, dl(), "/g\0x", 4, IN(CREATE), &got_id, &existed);
    status_t s4 = fswatch_add_until(y.ch, dl(), "/g", 2, UINT32_MAX, &got_id, &existed);
    status_t s5 = fswatch_add_until(y.ch, dl(), "/g", 2, UINT32_MAX & ~IN(MASK_CREATE), &got_id,
                                    &existed);
    printf("utest: %s: add of no path %s, of %u bytes %s, with a NUL %s, every bit %s, every bit "
           "but IN_MASK_CREATE %s\n", utest_cur, status_str(s1), ABUSE_PATH, status_str(s2),
           status_str(s3), status_str(s4), status_str(s5));
    CHECK(s2 != OK && s3 != OK && s4 == ERR_INVALID_ARGS);
    unwatcher(&y);

    /* A flood of takes, their answers never read: the client's queue
     * fills, its answers are lost; the good watcher served all the while. */
    CHECK(watcher(r.fs, &y));
    struct fswatch_take_req t = { 0, FSWATCH_TAKE, FSWATCH_TAKE_BYTES };
    unsigned sent = 0;
    for (; sent < 4000; sent++) {
        t.txid = sent + 1;
        if (jam_channel_write(y.ch, &t, sizeof(t), NULL, 0) != OK)
            break;
    }
    CHECK_ST(t_mkdir(&r, "/g/after"), OK);
    EXPECT(&good, "through the abuse", { id, IN(CREATE) | IN(ISDIR), "after" });
    printf("utest: %s: %u takes sent, never read\n", utest_cur, sent);
    unwatcher(&y);
    /* Its queued takes are served (their answers lost) before extfs finds
     * it closed: a few ms. */
    uint64_t t0 = now();
    do
        CHECK(job_handles(&h1));
    while (h1 != h0 && now() - t0 < 5 * NS_PER_S && jam_sleep_until(now() + NS_PER_MS) == OK);
    printf("utest: %s: extfs's job: %lu handles before, %lu after (%lu ms)\n", utest_cur,
           (unsigned long)h0, (unsigned long)h1, (unsigned long)((now() - t0) / NS_PER_MS));
    CHECK_EQ(h1, h0);
    unwatcher(&good);
    return stop_unsaved();
}

#define MEM_ROUNDS 500u

/* rounds of a directory named `name` made and removed in /m. */
static bool churn(unsigned rounds, const char *name)
{
    char path[300];
    snprintf(path, sizeof(path), "/m/%s", name);
    for (unsigned i = 0; i < rounds; i++) {
        CHECK_ST(t_mkdir(&r, path), OK);
        CHECK_ST(t_unlink_flags(&r, path, FS_UNLINK_DIR), OK);
    }
    return true;
}

/* A watcher that never reads costs extfs its queue's bound and no more:
 * the job's pages and handles after 2,000 more changes are what they were
 * once its queue was full (1,000 changes after the watch), and what it
 * reads at last is the bound's records and one IN_Q_OVERFLOW. */
bool t_extfs_watch_unread(void)
{
    struct wat a;
    struct job_info j0, j1, j2;
    char name[201];
    memset(name, 'm', 200);
    name[200] = '\0';
    CHECK(start());
    CHECK_ST(t_mkdir(&r, "/m"), OK);
    CHECK(churn(MEM_ROUNDS, name));   /* warm: the caches, the journal round once */
    CHECK(watcher(r.fs, &a));
    uint32_t id = add(&a, "/m", IN(CREATE) | IN(DELETE));
    CHECK(id);
    CHECK_ST(info_of(r.job, &j0), OK);
    CHECK(churn(MEM_ROUNDS, name));
    CHECK_ST(info_of(r.job, &j1), OK);
    CHECK(churn(2 * MEM_ROUNDS, name));
    CHECK_ST(info_of(r.job, &j2), OK);
    printf("utest: %s: extfs's job, the watch made / its queue full / 2,000 changes on: pages "
           "%lu / %lu / %lu, handles %lu / %lu / %lu, message bytes %lu / %lu / %lu\n", utest_cur,
           (unsigned long)j0.used[JOB_LIMIT_PAGES], (unsigned long)j1.used[JOB_LIMIT_PAGES],
           (unsigned long)j2.used[JOB_LIMIT_PAGES], (unsigned long)j0.used[JOB_LIMIT_HANDLES],
           (unsigned long)j1.used[JOB_LIMIT_HANDLES], (unsigned long)j2.used[JOB_LIMIT_HANDLES],
           (unsigned long)j0.used[JOB_LIMIT_MSG_BYTES], (unsigned long)j1.used[JOB_LIMIT_MSG_BYTES],
           (unsigned long)j2.used[JOB_LIMIT_MSG_BYTES]);
    CHECK(rung(&a));
    CHECK(j2.used[JOB_LIMIT_PAGES] <= j1.used[JOB_LIMIT_PAGES] + 2);
    CHECK_EQ(j2.used[JOB_LIMIT_HANDLES], j1.used[JOB_LIMIT_HANDLES]);
    /* Some messages of slack: the sample can land while requests or their
     * replies are still queued. Seen: 3,648 then 3,720 B (one message), and
     * 3,648 then 4,864 B under TCG (the watch made read 4,864 too: two levels
     * the queues settle at). A queue that grew with the changes would add
     * 2,000 messages (150 KB and more). */
    CHECK(j2.used[JOB_LIMIT_MSG_BYTES] <= j1.used[JOB_LIMIT_MSG_BYTES] + 4096);
    CHECK(j1.used[JOB_LIMIT_PAGES] <= j0.used[JOB_LIMIT_PAGES] + 32);   /* 16 KiB of records */
    CHECK(take_all(&a));
    CHECK_EQ(ngot, (int)(FSWATCH_QUEUE_BYTES / (FSWATCH_RECORD_FIXED + 200) + 1));
    CHECK(got[ngot - 1].id == 0 && got[ngot - 1].mask == IN(Q_OVERFLOW));
    unwatcher(&a);
    return stop_unsaved();
}
