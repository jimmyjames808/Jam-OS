/* utest: mapped files' stores written back by extfs
 * (user/services/extfs/wback.c; docs/WRITEMAP-PLAN.md's WE2). extfs with a
 * pager over a RAM disk it formats, started again and again over it:
 *   1. stores written by msync (vmo_sync with the wait: overwrites across
 *      pages, the last page's bytes inside the size and a store past the
 *      end that is nobody's), by fsync (file.sync), by fs.sync and by an
 *      orderly stop; a store over a hole (blocks taken then) and in an
 *      unwritten extent (the unwritten-grow test power: made written, no
 *      block taken); the modification time set;
 *   2. an instance with no pager reads the disk: every store is there;
 *   3. with files.writeback = 1 in the volume's etc/settings, a store is
 *      written by age alone; then extfs is killed with stores dirty, and a
 *      fresh instance (no state) serves their keys from the inodes: the
 *      msync is answered OK, and that of a file unlinked under its mapping
 *      (an orphan the fresh mount dropped) ERR_PEER_CLOSED;
 *   4. a full volume: a store over a hole is ERR_NO_SPACE at the next
 *      msync and at the next fsync, and a read shows it as the mapping
 *      does (the page kept, docs/WRITEMAP-REVIEW.md's finding 1); a store
 *      over the file's own block in the same msync is written. The error
 *      once to each mapper and each open (WR2's errseq): our mapping and
 *      a child process's each hear of it once, a mapping made after they
 *      did doesn't; two opens held across it and one made before any
 *      open heard of it each hear of it once, one made after doesn't; an
 *      error met by an fsync's write-back reaches the mappers too;
 *   5. VMOs a fresh instance doesn't know of yet (lost()): a write and a
 *      truncate reach their pages, a map to run is refused.
 * The disk is saved for Linux's e2fsck (tools/extfs-svc-test.sh). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define WF_MAX   (8u * PAGE_SIZE)          /* a test file's bytes at most */
#define AGE_WAIT (15 * NS_PER_S)           /* the age write-back (1 s, checked each second) */
#define FILL     (64u << 10)               /* the full volume's file, written this much a time */

/* A test file: what the disk must hold, our writable mapping of its VMO. */
struct wf {
    const char *path;
    uint64_t    size;
    handle_t    vmo;
    uint8_t    *m;
    uint64_t    len;           /* the mapping's bytes */
    uint8_t     want[WF_MAX];
};

static struct fatrun r;
static struct ramdisk rd;
static handle_t pager;
/* (named at the start: no initializer, so in .bss, not .data: ktest's
 * quota case spawns utest in a 256 KiB job) */
static struct wf fm, ff, fh, fu, fo, fy, fn, fl, ft, fx;
static struct wf *const all[] = { &fm, &ff, &fh, &fu, &fo, &fy, &fn, &fl, &ft, &fx };
static const char *const names[] = { "/m.bin", "/f.bin", "/h.bin", "/u.bin", "/s.bin", "/y.bin",
                                     "/n.bin", "/l.bin", "/t.bin", "/x.bin" };

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static uint64_t up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static void pattern(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed * 41 + i * 13 + (i >> 8) + 1);
}

/* n bytes (a page at most) of seed's pattern written at off through
 * extfs's write, the file made if it isn't there. */
static bool wr(struct wf *w, uint64_t off, uint32_t n, uint32_t seed)
{
    static uint8_t bytes[PAGE_SIZE];
    struct tfile f;
    uint32_t done = 0;
    CHECK(n <= PAGE_SIZE && off + n <= WF_MAX);
    pattern(bytes, n, seed);
    CHECK_ST(t_open(&r, w->path, FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    status_t st = t_write(&f, off, bytes, n, &done);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(done, n);
    memcpy(w->want + off, bytes, n);
    if (off + n > w->size)
        w->size = off + n;
    return true;
}

/* The file's shared VMO to write, its pages mapped writable here. */
static bool wmap(struct wf *w)
{
    struct tfile f;
    uint64_t size = 0, addr = 0;
    CHECK_ST(t_open(&r, w->path, FS_READ | FS_WRITE, &f), OK);
    status_t st = file_map_until(f.ch, dl(), FILE_MAP_WRITE, &w->vmo, &size);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(size, w->size);
    w->len = up(size);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), w->vmo, 0, w->len,
                          VMAR_READ | VMAR_WRITE, &addr), OK);
    w->m = (uint8_t *)(uintptr_t)addr;
    return true;
}

/* n bytes (a page at most) of seed's pattern stored at off through our
 * mapping: the file's inside its size (kept: the disk must get them, if
 * `kept`), nobody's past it. */
static void put(struct wf *w, uint64_t off, uint32_t n, uint32_t seed, bool kept)
{
    uint8_t bytes[PAGE_SIZE];
    pattern(bytes, n, seed);
    memcpy(w->m + off, bytes, n);
    if (kept && off < w->size)
        memcpy(w->want + off, bytes, off + n <= w->size ? n : w->size - off);
}

static void store(struct wf *w, uint64_t off, uint32_t n, uint32_t seed)
{
    put(w, off, n, seed, true);
}

/* No page of the pager is dirty: every store written back. */
static bool clean(void)
{
    static uint8_t page[PAGE_SIZE];
    struct pager_run run = { .len = PAGE_SIZE };
    return jam_pager_dirty(pager, 0, PAGER_DIRTY_OLDEST | PAGER_DIRTY_PEEK, &run, page) ==
           ERR_NOT_FOUND;
}

static status_t msync(struct wf *w)
{
    return jam_vmo_sync(w->vmo, 0, w->len, VMO_SYNC_WAIT, NULL);
}

static status_t fsync(struct wf *w)
{
    struct tfile f;
    status_t st = t_open(&r, w->path, FS_READ | FS_WRITE, &f);
    if (st != OK)
        return st;
    st = file_sync_until(f.ch, dl());
    t_close(&f);
    return st;
}

/* msync of w's mapping as a mapper with its own write-back error sample
 * (libc's per mapping: WR2's errseq). */
static status_t msync_as(struct wf *w, uint32_t *seq)
{
    return jam_vmo_sync(w->vmo, 0, w->len, VMO_SYNC_WAIT, seq);
}

/* Another process mapping a file: utest's wback-map child, which samples
 * the VMO sent in the first message and syncs it whole for each message
 * after, answering each with the status (wback_child). */
#define WM_SAMPLE 1u
#define WM_SYNC   2u

struct wm {
    handle_t ch, job, proc;
};

static status_t wm_ask(struct wm *c, uint32_t what, handle_t vmo)
{
    status_t st = jam_channel_write(c->ch, &what, sizeof(what), vmo ? &vmo : NULL, vmo ? 1 : 0);
    signals_t seen = 0;
    if (st == OK)
        st = jam_object_wait_one(c->ch, SIG_READABLE | SIG_PEER_CLOSED, dl(), &seen);
    status_t ans = ERR_INTERNAL;
    uint32_t nb = 0;
    struct channel_read_args a = {
        .h = c->ch, .rd_cap = sizeof(ans), .rd_bytes = (uint64_t)(uintptr_t)&ans,
        .rd_actual = (uint64_t)(uintptr_t)&nb,
    };
    if (st == OK)
        st = jam_channel_read(&a);
    return st != OK ? st : nb == sizeof(ans) ? ans : ERR_INTERNAL;
}

static bool wm_start(struct wm *c, struct wf *w)
{
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID, vmo = HANDLE_INVALID;
    *c = (struct wm){ 0 };
    CHECK_ST(new_job(&c->job), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    CHECK_ST(child("wback-map", NULL, c->job, theirs, &c->proc), OK);
    c->ch = mine;
    CHECK_ST(jam_handle_duplicate(w->vmo, RIGHT_SAME, &vmo), OK);
    CHECK_ST(wm_ask(c, WM_SAMPLE, vmo), OK);
    return true;
}

static bool wm_stop(struct wm *c)
{
    struct process_info info;
    CHECK_ST(jam_handle_close(c->ch), OK);   /* it ends at the peer's close */
    CHECK_ST(spawn_wait(c->proc, 20 * NS_PER_S, &info), OK);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(c->proc), OK);
    CHECK_ST(jam_handle_close(c->job), OK);
    return true;
}

int wback_child(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t ch = startup_handle(SR_USER), vmo = HANDLE_INVALID;
    uint32_t seq = 0;
    uint64_t size = 0;
    for (;;) {
        uint32_t what = 0, nb = 0, nh = 0;
        handle_t h = HANDLE_INVALID;
        struct channel_read_args a = {
            .h = ch, .rd_cap = sizeof(what), .rd_bytes = (uint64_t)(uintptr_t)&what,
            .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_handles = (uint64_t)(uintptr_t)&h,
            .rd_hcap = 1, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            if (jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen))
                return 2;
            continue;
        }
        if (st == ERR_PEER_CLOSED)
            return 0;
        if (st != OK || nb != sizeof(what))
            return 3;
        if (nh)
            vmo = h;
        status_t ans = ERR_INVALID_ARGS;
        if (what == WM_SAMPLE && (ans = jam_vmo_get_size(vmo, &size)) == OK)
            ans = jam_vmo_sync(vmo, 0, 0, VMO_SYNC_SAMPLE, &seq);
        else if (what == WM_SYNC)
            ans = jam_vmo_sync(vmo, 0, size, VMO_SYNC_WAIT, &seq);
        if (jam_channel_write(ch, &ans, sizeof(ans), NULL, 0) != OK)
            return 4;
    }
}

static int64_t mtime_of(const char *path)
{
    uint32_t mode, nlink, uid, gid;
    uint64_t size, blocks, id;
    int64_t at = 0, mt = 0, ct = 0;
    (void)fs_getattr_until(r.fs, dl(), path, (uint32_t)strlen(path), 0, &mode, &nlink, &uid,
                           &gid, &size, &blocks, &id, &at, &mt, &ct);
    return mt;
}

static uint64_t free_now(void)
{
    uint64_t total = 0, free_bytes = 0;
    return t_free(&r, &total, &free_bytes) == OK ? free_bytes : 0;
}

/* The file as extfs reads it is what the disk must hold (on an instance
 * with no pager: the disk's bytes alone). */
static bool on_disk(const struct wf *w)
{
    static uint8_t got[WF_MAX];
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(&r, w->path, FS_READ, &f), OK);
    status_t st = t_read(&f, 0, got, (uint32_t)w->size, &done);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(done, w->size);
    if (memcmp(got, w->want, w->size))
        FAIL("%s: the disk doesn't hold its stores", w->path);
    return true;
}

/* One cache: the file as extfs reads it is what our mapping shows (its
 * stores, written back or not, failed or not). */
static bool read_as_mapped(const struct wf *w)
{
    static uint8_t got[WF_MAX];
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(&r, w->path, FS_READ, &f), OK);
    status_t st = t_read(&f, 0, got, (uint32_t)w->size, &done);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(done, w->size);
    if (memcmp(got, w->m, w->size))
        FAIL("%s: a read doesn't show what its mapping shows", w->path);
    return true;
}

/* Killed: no orderly stop, nothing written back. */
static bool kill_extfs(void)
{
    struct process_info info;
    jam_job_kill(r.job);
    CHECK_ST(spawn_wait(r.proc, FAT_CALL_NS, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_handle_close(r.proc), OK);
    CHECK_ST(jam_handle_close(r.job), OK);
    CHECK_ST(jam_handle_close(r.fs), OK);
    CHECK_ST(jam_handle_close(r.ctl), OK);
    return ramdisk_join(r.rd);
}

/* 1: written by msync, fsync, fs.sync; over a hole, in an unwritten
 * extent; the stop's last. */
static bool synced(void)
{
    for (uint64_t at = 0; at < 3 * PAGE_SIZE; at += PAGE_SIZE)
        if (!wr(&fm, at, PAGE_SIZE, 1))
            return false;
    if (!wr(&fm, 3 * PAGE_SIZE, 500, 1))
        return false;
    int64_t t0 = mtime_of(fm.path);
    if (!wmap(&fm))
        return false;
    store(&fm, 1000, PAGE_SIZE, 2);
    store(&fm, 1000 + PAGE_SIZE, PAGE_SIZE, 2);
    store(&fm, 3 * PAGE_SIZE + 100, 300, 2);
    store(&fm, 3 * PAGE_SIZE + 600, 50, 3);   /* past the end: nobody's */
    CHECK(!clean());
    CHECK_ST(msync(&fm), OK);
    CHECK(clean());
    CHECK(mtime_of(fm.path) != t0);

    if (!wr(&ff, 0, PAGE_SIZE, 4) || !wr(&ff, PAGE_SIZE, PAGE_SIZE, 4) || !wmap(&ff))
        return false;
    store(&ff, 10, PAGE_SIZE, 5);
    CHECK_ST(fsync(&ff), OK);
    CHECK(clean());
    store(&ff, 2 * PAGE_SIZE - 50, 50, 6);
    CHECK_ST(t_sync(&r), OK);
    CHECK(clean());

    /* A hole: pages 0 to 6 have no blocks; two of them get theirs. */
    if (!wr(&fh, 7 * PAGE_SIZE, 100, 7))
        return false;
    uint64_t before = free_now();
    if (!wmap(&fh))
        return false;
    store(&fh, 2 * PAGE_SIZE, PAGE_SIZE, 8);
    store(&fh, 3 * PAGE_SIZE, PAGE_SIZE, 8);
    CHECK_ST(msync(&fh), OK);
    CHECK(clean());
    CHECK(free_now() + 2 * PAGE_SIZE <= before);

    /* An unwritten extent: pages 1 to 5 have blocks reading as zeros; a
     * store makes page 3 written, taking none. */
    struct tfile f;
    if (!wr(&fu, 0, PAGE_SIZE, 9))
        return false;
    CHECK_ST(t_open(&r, fu.path, FS_READ | FS_WRITE, &f), OK);
    status_t st = file_truncate_until(f.ch, dl(), 6 * PAGE_SIZE);
    t_close(&f);
    CHECK_ST(st, OK);
    fu.size = 6 * PAGE_SIZE;
    before = free_now();
    if (!wmap(&fu))
        return false;
    store(&fu, 3 * PAGE_SIZE, PAGE_SIZE, 10);
    CHECK_ST(msync(&fu), OK);
    CHECK(clean());
    CHECK_EQ(free_now(), before);

    /* Left dirty: the orderly stop writes it. */
    if (!wr(&fo, 0, PAGE_SIZE, 11) || !wmap(&fo))
        return false;
    store(&fo, 0, 200, 12);
    return true;
}

/* 3: by age, and a fresh start's keys. */
static bool aged_and_fresh(void)
{
    store(&fm, 50, 100, 13);
    CHECK(!clean());   /* (dirty a second before the age check takes it) */
    uint64_t end = now() + AGE_WAIT;
    while (!clean() && now() < end)
        jam_sleep_until(now() + 50 * NS_PER_MS);
    if (!clean())
        FAIL("a store wasn't written back by age (files.writeback = 1) in %lu s",
             (unsigned long)(AGE_WAIT / NS_PER_S));
    if (!wr(&fy, 0, PAGE_SIZE, 14) || !wmap(&fy))
        return false;
    put(&fy, 0, 100, 16, false);   /* its page fetched (nobody will answer a fault soon) */
    CHECK_ST(t_unlink(&r, fy.path), OK);   /* mapped: an orphan */
    if (!kill_extfs())
        return false;
    /* Stores with no extfs: present pages, made dirty in the kernel alone. */
    store(&fm, 200, 100, 15);
    put(&fy, 0, 100, 17, false);
    /* A fresh instance: no state, the same pager. */
    if (!extfs_start_pager(&r, &rd, pager))
        return false;
    CHECK_ST(msync(&fm), OK);
    CHECK_ST(msync(&fy), ERR_PEER_CLOSED);
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)fy.m, fy.len), OK);
    fy.m = NULL;
    CHECK(clean());
    return true;
}

/* 4: a full volume. */
static bool full(void)
{
    static uint8_t zeros[FILL];
    struct tfile f;
    uint32_t done = 0;
    if (!wr(&fn, 3 * PAGE_SIZE, 100, 17))   /* pages 0 to 2 a hole */
        return false;
    CHECK_ST(t_open(&r, "/big", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    status_t st = OK;
    for (uint64_t at = 0; st == OK; at += done)
        st = t_write(&f, at, zeros, FILL, &done);
    t_close(&f);
    CHECK_ST(st, ERR_NO_SPACE);
    if (!wmap(&fn))
        return false;
    /* Two mappers sample the VMO's errors: our mapping (a) and a child
     * process's (b). */
    struct wm b;
    uint32_t sa = 7, sc = 7;
    CHECK_ST(jam_vmo_sync(fn.vmo, 0, 0, VMO_SYNC_SAMPLE, &sa), OK);
    if (!wm_start(&b, &fn))
        return false;
    put(&fn, PAGE_SIZE, 100, 18, false);   /* over the hole: no block for it */
    store(&fn, 3 * PAGE_SIZE, 50, 18);     /* over its own block: written */
    CHECK_ST(msync_as(&fn, &sa), ERR_NO_SPACE);
    CHECK_ST(msync_as(&fn, &sa), OK);   /* once */
    CHECK(clean());   /* given up: the pages clean, Q1's answer */
    /* ... but a read shows the store the mapping shows, until the page is
     * evicted (the disk's bytes then, for both: an instance with no pager
     * reads the disk alone after this). */
    if (!read_as_mapped(&fn))
        return false;
    /* Mapped again while the VMO lives (extfs let go of its handle at the
     * open's close): the error stays for the opens' next fsync. This
     * mapping (c), made after a heard of the error, doesn't hear of it;
     * b, which mapped before, does, once. */
    handle_t again = HANDLE_INVALID;
    uint64_t size = 0;
    CHECK_ST(t_open(&r, fn.path, FS_READ | FS_WRITE, &f), OK);
    st = file_map_until(f.ch, dl(), FILE_MAP_WRITE, &again, &size);
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_ST(jam_vmo_sync(again, 0, 0, VMO_SYNC_SAMPLE, &sc), OK);
    CHECK_ST(jam_vmo_sync(again, 0, fn.len, VMO_SYNC_WAIT, &sc), OK);
    CHECK_ST(jam_handle_close(again), OK);
    CHECK_ST(wm_ask(&b, WM_SYNC, HANDLE_INVALID), ERR_NO_SPACE);
    CHECK_ST(wm_ask(&b, WM_SYNC, HANDLE_INVALID), OK);
    CHECK_ST(msync(&fn), OK);   /* (no sample: heard of already) */
    /* Opens: two made now, before any open heard of the error, hear of it
     * once each (Linux's open after a failed write-back); one made after
     * the first heard doesn't. */
    struct tfile o1, o2;
    CHECK_ST(t_open(&r, fn.path, FS_READ | FS_WRITE, &o1), OK);
    CHECK_ST(t_open(&r, fn.path, FS_READ | FS_WRITE, &o2), OK);
    CHECK_ST(file_sync_until(o1.ch, dl()), ERR_NO_SPACE);
    CHECK_ST(file_sync_until(o1.ch, dl()), OK);
    CHECK_ST(fsync(&fn), OK);
    CHECK_ST(file_sync_until(o2.ch, dl()), ERR_NO_SPACE);
    CHECK_ST(file_sync_until(o2.ch, dl()), OK);
    /* An error met by o1's fsync (a store over another page of the hole):
     * o1 and o2, open across it, hear of it once each, a new open doesn't;
     * the mappers a and b hear of it too, once. */
    put(&fn, 2 * PAGE_SIZE, 100, 19, false);
    CHECK_ST(file_sync_until(o1.ch, dl()), ERR_NO_SPACE);
    CHECK_ST(file_sync_until(o1.ch, dl()), OK);
    CHECK_ST(file_sync_until(o2.ch, dl()), ERR_NO_SPACE);
    CHECK_ST(file_sync_until(o2.ch, dl()), OK);
    CHECK_ST(fsync(&fn), OK);
    CHECK(clean());
    CHECK_ST(msync_as(&fn, &sa), ERR_NO_SPACE);
    CHECK_ST(msync_as(&fn, &sa), OK);
    CHECK_ST(wm_ask(&b, WM_SYNC, HANDLE_INVALID), ERR_NO_SPACE);
    CHECK_ST(wm_ask(&b, WM_SYNC, HANDLE_INVALID), OK);
    CHECK_ST(msync(&fn), OK);
    t_close(&o1);
    t_close(&o2);
    if (!wm_stop(&b))
        return false;
    CHECK_ST(t_unlink(&r, "/big"), OK);
    return true;
}

/* 5: VMOs an instance before made, which a fresh instance (no state)
 * knows of only once something finds them (docs/WRITEMAP-REVIEW.md's
 * finding 2): a write over a stored page reaches the page (the mapping
 * shows it, and the write-back writes the store and the write, not the
 * old bytes over the write); a truncate zeroes the tail past the new end
 * (zero after a regrow, there and on the disk); a map to run is refused
 * while such a VMO may be written. files.writeback is 600 s here: nothing
 * is written back by age meanwhile. */
static bool lost(void)
{
    struct wf *const ws[] = { &fl, &ft, &fx };
    for (unsigned i = 0; i < 3; i++)
        if (!wr(ws[i], 0, PAGE_SIZE, 30 + i) || !wr(ws[i], PAGE_SIZE, PAGE_SIZE, 30 + i) ||
            !wmap(ws[i]))
            return false;
    store(&fl, 100, 200, 33);
    store(&ft, PAGE_SIZE + 500, 1000, 34);
    store(&fx, 10, 10, 35);
    if (!kill_extfs() || !extfs_start_pager(&r, &rd, pager))
        return false;

    struct tfile f;
    handle_t xv = HANDLE_INVALID;
    uint64_t size = 0;
    CHECK_ST(t_open(&r, fx.path, FS_READ, &f), OK);
    status_t st = file_map_until(f.ch, dl(), FILE_MAP_EXEC, &xv, &size);
    t_close(&f);
    if (xv)
        jam_handle_close(xv);
    CHECK_ST(st, ERR_BUSY);

    if (!wr(&fl, 1000, 500, 36))
        return false;
    if (memcmp(fl.m, fl.want, fl.size))
        FAIL("%s: its mapping doesn't show a write over a stored page", fl.path);

    CHECK_ST(t_open(&r, ft.path, FS_READ | FS_WRITE, &f), OK);
    st = file_truncate_until(f.ch, dl(), PAGE_SIZE + 600);
    if (st == OK)
        st = file_truncate_until(f.ch, dl(), 2 * PAGE_SIZE);
    t_close(&f);
    CHECK_ST(st, OK);
    memset(ft.want + PAGE_SIZE + 600, 0, PAGE_SIZE - 600);
    if (memcmp(ft.m, ft.want, ft.size))
        FAIL("%s: its mapping shows stores past an end it was cut to", ft.path);

    for (unsigned i = 0; i < 3; i++)
        CHECK_ST(msync(ws[i]), OK);
    CHECK(clean());
    return on_disk(&fl) && on_disk(&ft) && on_disk(&fx);
}

/* Stop an instance that had the pager: the VMOs it made that are still
 * mapped here keep their links and structs on its job (2 handle units
 * each) until they go. */
static bool stop_kept(void)
{
    r.kept = 2 * (sizeof(all) / sizeof(all[0]));
    return fat_stop(&r);
}

bool t_extfs_wback(void)
{
    static const char *const unwritten[] = { "unwritten-grow" };
    handle_t root = startup_handle(SR_RESOURCE);
    pager = HANDLE_INVALID;
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        memset(all[i], 0, sizeof(*all[i]));
        all[i]->path = names[i];
    }
    if (!root || jam_pager_create(root, 4096, FAT_CALL_NS, 0, &pager) != OK) {
        printf("utest: extfs_wback: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    bool ok = ramdisk_create(&rd, EXT_DISK_SECTORS) &&
              extfs_start_pager_words(&r, &rd, pager, unwritten, 1) && synced() && stop_kept();
    /* 2: the disk read by an instance with no pager; the setting made. */
    ok = ok && extfs_start(&r, &rd, false, NULL, 0) && on_disk(&fm) && on_disk(&ff) &&
         on_disk(&fh) && on_disk(&fu) && on_disk(&fo) && t_mkdir(&r, "/etc") == OK &&
         put_file(&r, "/etc/settings", "# utest's\nfiles.writeback = 1\n") && fat_stop(&r);
    ok = ok && extfs_start_pager(&r, &rd, pager) && aged_and_fresh() && stop_kept();
    ok = ok && extfs_start(&r, &rd, false, NULL, 0) && on_disk(&fm) &&
         t_stat(&r, fy.path, NULL, NULL, NULL) == ERR_NOT_FOUND && fat_stop(&r);
    ok = ok && extfs_start_pager(&r, &rd, pager) && full() && stop_kept();
    ok = ok && extfs_start(&r, &rd, false, NULL, 0) && on_disk(&fn) &&
         put_file(&r, "/etc/settings", "# utest's\nfiles.writeback = 600\n") && fat_stop(&r);
    ok = ok && extfs_start_pager(&r, &rd, pager) && lost() && stop_kept() &&
         extfs_save(&rd, "wback");
    handle_t self = startup_handle(SR_SELF_VMAR);
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i]->m)
            (void)jam_vmar_unmap(self, (uint64_t)(uintptr_t)all[i]->m, all[i]->len);
        if (all[i]->vmo)
            jam_handle_close(all[i]->vmo);
    }
    CHECK_ST(jam_handle_close(pager), OK);
    return ramdisk_destroy(&rd) && ok;
}
