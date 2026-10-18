/* utest: extfs's record locks (file.idl's lock, getlock, lockowner;
 * user/services/extfs/locks.c, <rlock.h>), one process speaking the
 * protocol to an extfs over a RAM disk. Owners from one process differ
 * only as lock owners (OFD and flock: each open's own); POSIX locks,
 * whose owner is the writer's pid, are all ours here, so posixtest's locks
 * section has the cases between processes (EDEADLK among them).
 *
 * extfs_locks: the conflicts (write against anything, read against read
 * none), the bytes an owner's lock replaces (split, joined, a type
 * changed), flock's space apart from fcntl's, getlock's answer (a POSIX
 * lock's pid ours), the open's rights, the close of the open a POSIX lock
 * was set through and of an owner's token, the wake channel, bad
 * requests, a full table (and a split refused there, nothing changed) and
 * the owners' limit (a closed owner's slot taken again).
 *
 * extfs_restart_locks: across restarts (utest's supervisor,
 * extfs_restart.c): a lock request ended after its commit is answered by
 * the successor; the locks, POSIX and OFD, and the owners carry on; a
 * waiter's wake channel closes with the instance; a lockowner whose reply
 * died comes again without its token; a fresh start closes every open,
 * and the new instance has no locks. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define G   FILE_LOCK_GRANTED
#define C   FILE_LOCK_CONFLICT
#define R   FILE_LOCK_READ
#define W   FILE_LOCK_WRITE
#define U   FILE_LOCK_UNLOCK
#define OFD FILE_LOCK_OFD
#define FL  FILE_LOCK_FLOCK
#define PX  FILE_LOCK_POSIX
#define EOF_ FILE_LOCK_EOF
#define BAD 0xffu

static struct ramdisk ld;

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static status_t lk_st(const struct tfile *f, uint8_t type, uint8_t flags, uint64_t s, uint64_t e,
                      uint64_t owner, uint8_t *result, handle_t *wake)
{
    handle_t w[1];
    uint32_t n = 0;
    *result = BAD;
    status_t st = file_lock_until(f->ch, dl(), type, flags, s, e, owner, result, w, &n);
    if (wake)
        *wake = n ? w[0] : HANDLE_INVALID;
    else if (n)
        jam_handle_close(w[0]);
    return st;
}

/* lock's result, or BAD for an error. */
static uint8_t lk(const struct tfile *f, uint8_t type, uint8_t flags, uint64_t s, uint64_t e,
                  uint64_t owner)
{
    uint8_t r;
    return lk_st(f, type, flags, s, e, owner, &r, NULL) == OK ? r : BAD;
}

struct held {
    uint8_t  type;
    uint64_t start, end;
    uint32_t pid;
};

static bool gl(const struct tfile *f, uint8_t type, uint8_t flags, uint64_t s, uint64_t e,
               uint64_t owner, struct held *h)
{
    *h = (struct held){ .type = BAD };
    return file_getlock_until(f->ch, dl(), type, flags, s, e, owner, &h->type, &h->start,
                              &h->end, &h->pid) == OK;
}

/* getlock says the lock in the way is type, start..end (pid: its). */
static bool in_way(const struct tfile *f, uint8_t type, uint8_t flags, uint64_t s, uint64_t e,
                   uint64_t owner, uint8_t htype, uint64_t hs, uint64_t he, uint32_t pid)
{
    struct held h;
    CHECK(gl(f, type, flags, s, e, owner, &h));
    CHECK_EQ(h.type, htype);
    CHECK(h.start == hs && h.end == he);
    CHECK_EQ(h.pid, pid);
    return true;
}

static bool free_of(const struct tfile *f, uint8_t type, uint8_t flags, uint64_t s, uint64_t e,
                    uint64_t owner)
{
    struct held h;
    CHECK(gl(f, type, flags, s, e, owner, &h));
    CHECK_EQ(h.type, U);
    return true;
}

static bool owner(const struct tfile *f, uint64_t *id, handle_t *token)
{
    handle_t t[1];
    uint32_t n = 0;
    CHECK_ST(file_lockowner_until(f->ch, dl(), id, t, &n), OK);
    CHECK(n == 1 && *id);
    *token = t[0];
    return true;
}

static uint32_t my_pid(void)
{
    struct process_info pi;
    CHECK_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    return (uint32_t)pi.pid;
}

static bool woken(handle_t wake, uint64_t within)
{
    return jam_object_wait_one(wake, SIG_PEER_CLOSED, now() + within, NULL) == OK;
}

bool t_extfs_locks(void)
{
    struct fatrun r;
    struct tfile a, b, c, d;
    uint64_t oa, ob, oa2;
    handle_t ta, tb, ta2, wake;
    uint8_t res;
    uint32_t pid = my_pid();
    CHECK(ramdisk_create(&ld, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &ld, false, NULL, 0));
    CHECK_ST(t_open(&r, "/f", FS_READ | FS_WRITE | FS_CREATE, &a), OK);
    CHECK_ST(t_open(&r, "/f", FS_READ | FS_WRITE, &b), OK);
    CHECK_ST(t_open(&r, "/f", FS_READ, &c), OK);
    CHECK(owner(&a, &oa, &ta) && owner(&b, &ob, &tb));
    CHECK(oa != ob);

    /* Conflicts: a write lock against any lock, reads together. */
    CHECK_EQ(lk(&a, W, OFD, 10, 19, oa), G);
    CHECK_EQ(lk(&b, W, OFD, 15, 25, ob), C);
    CHECK_EQ(lk(&b, R, OFD, 19, 19, ob), C);
    CHECK_EQ(lk(&b, R, OFD, 0, 9, ob), G);   /* touching, not overlapping */
    CHECK(in_way(&b, W, OFD, 0, EOF_, ob, W, 10, 19, 0));   /* its own [0, 9] isn't */
    CHECK(free_of(&a, R, OFD, 0, 9, oa));
    CHECK(in_way(&a, W, OFD, 0, 9, oa, R, 0, 9, 0));

    /* Split: a's middle unlocked is b's to take, its two sides still a's. */
    CHECK_EQ(lk(&a, U, OFD, 12, 13, oa), G);
    CHECK_EQ(lk(&b, W, OFD, 12, 13, ob), G);
    CHECK_EQ(lk(&b, W, OFD, 11, 11, ob), C);
    CHECK_EQ(lk(&b, W, OFD, 14, 14, ob), C);
    CHECK(in_way(&b, W, OFD, 14, EOF_, ob, W, 14, 19, 0));
    /* Joined: b's locks gone, a's middle back is one lock with its sides,
     * and one touching it joins too. */
    CHECK_EQ(lk(&b, U, OFD, 0, EOF_, ob), G);
    CHECK_EQ(lk(&a, W, OFD, 12, 13, oa), G);
    CHECK_EQ(lk(&a, W, OFD, 20, 29, oa), G);
    CHECK(in_way(&b, R, OFD, 0, EOF_, ob, W, 10, 29, 0));
    CHECK_EQ(lk(&a, W, OFD, 10, 29, oa), G);   /* held already: no change */
    CHECK(in_way(&b, R, OFD, 0, EOF_, ob, W, 10, 29, 0));
    /* A type changed in the middle: three locks. */
    CHECK_EQ(lk(&a, R, OFD, 15, 15, oa), G);
    CHECK_EQ(lk(&b, R, OFD, 15, 15, ob), G);
    CHECK_EQ(lk(&b, R, OFD, 14, 14, ob), C);
    CHECK_EQ(lk(&b, R, OFD, 16, 16, ob), C);
    CHECK_EQ(lk(&a, U, OFD, 0, EOF_, oa), G);
    CHECK_EQ(lk(&b, U, OFD, 0, EOF_, ob), G);
    CHECK(free_of(&b, W, OFD, 0, EOF_, ob));

    /* flock's space: never meets fcntl's; the whole file only. */
    CHECK_EQ(lk(&a, W, FL, 0, EOF_, oa), G);
    CHECK_EQ(lk(&b, W, OFD, 0, EOF_, ob), G);
    CHECK_EQ(lk(&b, R, FL, 0, EOF_, ob), C);
    CHECK_ST(lk_st(&b, W, FL, 0, 5, ob, &res, NULL), ERR_INVALID_ARGS);
    CHECK_EQ(lk(&a, U, FL, 0, EOF_, oa), G);
    CHECK_EQ(lk(&b, R, FL, 0, EOF_, ob), G);
    CHECK_EQ(lk(&b, U, FL, 0, EOF_, ob), G);
    CHECK_EQ(lk(&b, U, OFD, 0, EOF_, ob), G);

    /* POSIX: ours (the writer's pid), through a read-only open: a read
     * lock may, a write lock not; a flock asks no rights. It meets OFD
     * locks, and goes when the open it was set through closes. */
    CHECK_EQ(lk(&c, R, PX, 100, 199, 0), G);
    CHECK_ST(lk_st(&c, W, PX, 0, 0, 0, &res, NULL), ERR_ACCESS_DENIED);
    CHECK(in_way(&b, W, OFD, 0, EOF_, ob, R, 100, 199, pid));
    CHECK_EQ(lk(&b, W, OFD, 150, 150, ob), C);
    CHECK_EQ(lk(&a, R, PX, 300, 300, 0), G);   /* the same owner through another open */
    CHECK(in_way(&b, W, OFD, 200, EOF_, ob, R, 300, 300, pid));
    t_close(&c);
    CHECK_EQ(lk(&b, W, OFD, 150, 150, ob), G);   /* every POSIX lock of ours on the file */
    CHECK(free_of(&b, W, OFD, 200, EOF_, ob));
    CHECK_EQ(lk(&b, U, OFD, 0, EOF_, ob), G);

    /* An owner's token closed: its locks go, the owner is unknown. */
    CHECK_EQ(lk(&a, W, OFD, 0, 0, oa), G);
    CHECK_EQ(lk(&b, W, OFD, 0, 0, ob), C);
    jam_handle_close(ta);
    CHECK_EQ(lk(&b, W, OFD, 0, 0, ob), G);
    CHECK_ST(lk_st(&a, R, OFD, 5, 5, oa, &res, NULL), ERR_NOT_FOUND);
    CHECK_EQ(lk(&b, U, OFD, 0, EOF_, ob), G);

    /* A wait: the wake channel's peer closes when the lock in the way goes. */
    CHECK(owner(&a, &oa2, &ta2));
    CHECK_EQ(lk(&a, W, OFD, 0, 9, oa2), G);
    CHECK_ST(lk_st(&b, W, OFD | FILE_LOCK_WAIT, 5, 5, ob, &res, &wake), OK);
    CHECK(res == C && wake != HANDLE_INVALID);
    CHECK(!woken(wake, 100 * NS_PER_MS));
    CHECK_EQ(lk(&a, R, OFD, 20, 20, oa2), G);   /* a lock added frees nothing: no wake */
    CHECK(!woken(wake, 0));
    CHECK_EQ(lk(&a, U, OFD, 0, 9, oa2), G);
    CHECK(woken(wake, FAT_CALL_NS));
    jam_handle_close(wake);
    CHECK_EQ(lk(&b, W, OFD, 5, 5, ob), G);
    CHECK_ST(lk_st(&b, W, OFD, 5, 5, ob, &res, &wake), OK);   /* no wait asked: no wake */
    CHECK(res == G && wake == HANDLE_INVALID);

    /* Bad requests. */
    CHECK_ST(lk_st(&a, R, OFD, 5, 4, oa2, &res, NULL), ERR_INVALID_ARGS);
    CHECK_ST(lk_st(&a, R, OFD, 5, EOF_ + 1, oa2, &res, NULL), ERR_INVALID_ARGS);
    CHECK_ST(lk_st(&a, R, 3, 0, 0, oa2, &res, NULL), ERR_INVALID_ARGS);
    CHECK_ST(lk_st(&a, R, OFD | 8, 0, 0, oa2, &res, NULL), ERR_INVALID_ARGS);
    CHECK_ST(lk_st(&a, 3, OFD, 0, 0, oa2, &res, NULL), ERR_INVALID_ARGS);
    CHECK_ST(lk_st(&a, R, OFD, 0, 0, oa2 ^ 1, &res, NULL), ERR_NOT_FOUND);
    struct held h;
    CHECK_ST(file_getlock_until(a.ch, dl(), U, OFD, 0, 0, oa2, &h.type, &h.start, &h.end, &h.pid),
             ERR_INVALID_ARGS);
    CHECK_ST(file_getlock_until(a.ch, dl(), R, OFD | FILE_LOCK_WAIT, 0, 0, oa2, &h.type, &h.start,
                                &h.end, &h.pid), ERR_INVALID_ARGS);

    /* A full table: ERR_NO_RESOURCES (libc's ENOLCK), and a split that
     * would need one more lock refused with nothing changed. Locks held now:
     * a's [20, 20] and b's [5, 5]. */
    CHECK_ST(t_open(&r, "/g", FS_READ | FS_WRITE | FS_CREATE, &d), OK);
    CHECK_EQ(lk(&d, W, OFD, 1000, 1002, oa2), G);
    unsigned made = 3;
    for (uint64_t off = 0; lk(&d, W, OFD, off * 2, off * 2, oa2) == G; off++)
        made++;
    CHECK_EQ(made, 256u);
    CHECK_ST(lk_st(&d, W, OFD, 600, 600, oa2, &res, NULL), ERR_NO_RESOURCES);
    CHECK_ST(lk_st(&d, U, OFD, 1001, 1001, oa2, &res, NULL), ERR_NO_RESOURCES);
    CHECK(in_way(&d, W, OFD, 1001, 1001, ob, W, 1000, 1002, 0));
    CHECK_EQ(lk(&d, U, OFD, 0, EOF_, oa2), G);
    CHECK_EQ(lk(&d, W, OFD, 600, 600, oa2), G);
    CHECK_EQ(lk(&d, U, OFD, 0, EOF_, oa2), G);

    /* The owners' limit (64); a closed owner's slot is taken again. */
    handle_t toks[64];
    unsigned n = 0;
    uint64_t id;
    while (n < 64 && file_lockowner_until(a.ch, dl(), &id, &toks[n], &(uint32_t){ 0 }) == OK)
        n++;
    CHECK_EQ(n, 64u - 2u);   /* oa2 and ob hold two */
    CHECK_ST(file_lockowner_until(a.ch, dl(), &id, &wake, &(uint32_t){ 0 }), ERR_NO_RESOURCES);
    jam_handle_close(toks[--n]);
    CHECK(owner(&a, &id, &toks[n++]));
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(toks[i]);

    jam_handle_close(ta2);
    jam_handle_close(tb);
    t_close(&a);
    t_close(&b);
    t_close(&d);
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&ld, "locks"));
    return ramdisk_destroy(&ld);
}

/* The services that keep no locks answer lock, getlock and lockowner
 * ERR_NOT_SUPPORTED (libc then grants a lock and keeps none, as before). */
static bool no_locks(handle_t file)
{
    uint8_t res;
    handle_t w[1];
    uint32_t n = 0;
    uint64_t id;
    struct held h;
    CHECK_ST(file_lock_until(file, dl(), W, PX, 0, 0, 0, &res, w, &n), ERR_NOT_SUPPORTED);
    CHECK_ST(file_getlock_until(file, dl(), W, PX, 0, 0, 0, &h.type, &h.start, &h.end, &h.pid),
             ERR_NOT_SUPPORTED);
    CHECK_ST(file_lockowner_until(file, dl(), &id, w, &n), ERR_NOT_SUPPORTED);
    return true;
}

bool t_fs_no_locks(void)
{
    struct fatrun r;
    struct tfile f;
    CHECK(ramdisk_create(&ld, 16 * MIB_SECTORS));
    CHECK(fat_start(&r, &ld, false));
    CHECK_ST(t_open(&r, "/f", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    bool ok = no_locks(f.ch);
    t_close(&f);
    CHECK(fat_stop(&r) && ramdisk_destroy(&ld));
    handle_t boot, ch, buf;
    uint64_t size;
    if (ns_channel("/boot", &boot) != OK) {
        printf("utest: %s: no /boot in our namespace: its part skipped\n", utest_cur);
        return ok;
    }
    CHECK_ST(fs_open_until(boot, dl(), "bin/utest", 9, FS_READ, 0, &ch, &buf, &size), OK);
    ok = no_locks(ch) && ok;
    jam_handle_close(ch);
    jam_handle_close(buf);
    jam_handle_close(boot);
    return ok;
}

/* extfs ended by its test powers (utest's restart supervisor): instance 0
 * after a lock request's commit, instance 1 after a lockowner's, the
 * third handed a new state. The comments count each instance's requests. */
bool t_extfs_restart_locks(void)
{
    static const char *const dies[] = { "die=commit:8", "die=commit:4", "die=commit:5" };
    struct tfile a, b, c;
    uint64_t oa, ob, ob2, id;
    handle_t ta, tb, tb2, wake, t[1];
    uint32_t n = 9, pid = my_pid();
    uint8_t res;
    CHECK(ramdisk_create(&ld, EXT_DISK_SECTORS));
    CHECK(extfs_sup_start(&ld, dies, 3));
    struct fatrun *s = extfs_sup_run();

    CHECK_ST(t_open(s, "/f", FS_READ | FS_WRITE | FS_CREATE, &a), OK);   /* 1 */
    CHECK_ST(t_open(s, "/f", FS_READ | FS_WRITE, &b), OK);               /* 2 */
    CHECK(owner(&a, &oa, &ta) && owner(&b, &ob, &tb));                    /* 3, 4 */
    CHECK_EQ(lk(&a, W, OFD, 0, 9, oa), G);                                /* 5 */
    CHECK_EQ(lk(&a, R, PX, 20, 29, 0), G);                                /* 6 */
    CHECK_ST(lk_st(&b, W, OFD | FILE_LOCK_WAIT, 5, 5, ob, &res, &wake), OK);   /* 7 */
    CHECK(res == C && wake != HANDLE_INVALID);
    CHECK_EQ(lk(&a, W, OFD, 40, 49, oa), G);   /* 8: ended, answered by the successor */
    CHECK_EQ(extfs_sup_ended(), 1u);
    CHECK(woken(wake, FAT_CALL_NS));            /* the wait's end died with instance 0 */
    jam_handle_close(wake);

    /* Instance 1: every lock and owner carried on. */
    CHECK(in_way(&b, W, OFD, 0, 9, ob, W, 0, 9, 0));        /* 1 */
    CHECK(in_way(&b, W, OFD, 20, 29, ob, R, 20, 29, pid));  /* 2 */
    CHECK(in_way(&b, W, OFD, 40, 49, ob, W, 40, 49, 0));    /* 3 */
    /* 4: a lockowner ended after its commit: answered again without its
     * token (the owner it made goes with its peer). */
    CHECK_ST(file_lockowner_until(b.ch, dl(), &id, t, &n), OK);
    CHECK_EQ(n, 0u);
    CHECK_EQ(extfs_sup_ended(), 2u);

    /* Instance 2: a new owner; a's token closed takes its OFD locks, a's
     * close its POSIX ones. */
    CHECK(owner(&b, &ob2, &tb2));                     /* 1 */
    jam_handle_close(ta);
    CHECK(in_way(&b, W, OFD, 0, EOF_, ob2, R, 20, 29, pid));   /* 2 */
    CHECK_EQ(lk(&b, W, OFD, 0, 9, ob2), G);           /* 3 */
    t_close(&a);
    CHECK(free_of(&b, W, OFD, 10, EOF_, ob2));        /* 4 */

    /* A fresh start (instance 2 ended at a lock's commit, the next handed
     * a new state): every open closed, that lock's too, and no lock or
     * owner left. */
    extfs_sup_fresh();
    CHECK_ST(lk_st(&b, W, OFD, 50, 59, ob2, &res, NULL), ERR_PEER_CLOSED);   /* 5: ended */
    CHECK_EQ(extfs_sup_ended(), 3u);
    CHECK_ST(t_open(s, "/f", FS_READ | FS_WRITE, &c), OK);
    CHECK(free_of(&c, W, PX, 0, EOF_, 0));
    CHECK_ST(lk_st(&c, W, OFD, 0, 9, ob2, &res, NULL), ERR_NOT_FOUND);
    t_close(&b);
    t_close(&c);
    jam_handle_close(tb);
    jam_handle_close(tb2);
    CHECK(extfs_sup_stop());
    CHECK(extfs_save(&ld, "restart-locks"));
    return ramdisk_destroy(&ld);
}
