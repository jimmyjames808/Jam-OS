/* extfs: mapped files' stores written back (docs/WRITEMAP-PLAN.md's WE2,
 * sections 2.3 to 2.7). A store through a writable mapping makes its page
 * dirty in the kernel (the pager VMO, pager.c's slots); extfs writes dirty
 * pages to the file and tells the kernel they are done.
 *
 * A write-back is a journal transaction of its own, no request slot (the
 * kernel holds what a death would lose), a batch of runs:
 *   1. pager_dirty with PAGER_DIRTY_BEGIN: the first run of dirty pages in
 *      a range (or, with PAGER_DIRTY_OLDEST, around the pager's oldest
 *      dirty page), its pages write-protected in every mapping and then
 *      copied into wbuf (up to 1 MiB, ours alone: pager.c's buffer is the
 *      read requests' and the peeks');
 *   2. the run's bytes written to the file (ext_write) as far as the file
 *      goes now, never past its size, which so never changes: overwrites
 *      in place, holes filled and unwritten extents made written in the
 *      transaction, their data on the disk before the commit (ordered
 *      mode, as a write's); the inode's mtime and ctime set (a store has
 *      no earlier moment extfs sees). Again for the next run, as long as
 *      the journal has room (WB_ROOM) and the batch is not full;
 *   3. the commit (tx_commit: the log, its commit block and a flush), then
 *      PAGER_OP_WRITTEN over each run, OK or the error. Only after the
 *      commit: an acknowledged msync is never lost.
 * extfs is one thread and ends each batch before it begins another (and
 * before it reads the next request), so no WRITTEN ever covers a page
 * another write-back began and hasn't ended (WK3's hazard), and each run's
 * WRITTEN covers exactly the pages its BEGIN took.
 *
 * When: a PAGER_REQ_WRITEBACK, between requests like a page read (pager.c's
 * serve: its range); by age, at most once a second whether busy or not
 * (wback_tick from main.c's loop: OLDEST while the oldest dirty page is
 * older than the files.writeback setting, read from the volume's own
 * etc/settings at the start, 10 s by default); file.sync (the file's VMO
 * whole) and fs.sync (every page dirty when it was asked, of every VMO the
 * pager has); an orderly stop (the same, within WB_STOP_NS: volumes waits
 * 5 s for the stop's answer).
 *
 * A death (2.6): before the commit, the successor's PAGER_NEXT_AGAIN makes
 * the writing pages dirty again and the request comes again: written again
 * (data written in place is the same bytes or older ones written over;
 * data in new blocks went to blocks the committed bitmaps call free).
 * After the commit, before WRITTEN: the replay keeps it, AGAIN makes the
 * pages dirty again, and they are written again before the request is
 * answered. A file.sync or fs.sync in progress runs again from its slot
 * with its write-backs. A fresh start's requests name keys the state
 * doesn't know: pager.c's maps_key makes their slots from the inodes the
 * keys name, and detaches a key whose file is gone (its data went with it).
 *
 * Errors (Q1's answer A): a run that can't be written (a full volume for a
 * store over a hole, a disk error, a volume that can't be written) is
 * answered WRITTEN with ERR_NO_SPACE or ERR_IO: its pages become clean
 * (readable until evicted; then the disk's bytes come back), the kernel
 * keeps the error for the VMO's next vmo_sync, and the slot keeps it for
 * the file's next file.sync. Of a run cut short by a full volume, the
 * pages written are OK. A disk error or a failed commit gives the whole
 * batch up. */
#include <settings.h>
#include "extfs.h"

#define WB_BUF      PAGER_SUPPLY_MAX    /* a run's bytes at most */
#define WB_RUNS     32u                 /* runs in one transaction at most */
#define WB_ROOM     64u                 /* journal blocks a run's metadata may take: a run
                                         * needs this much room to begin */
#define WB_AGE_RUNS 4u                  /* runs one age check writes at most (4 MiB: a slow
                                         * stick's second or so between two requests); the
                                         * rest a second later */
#define WB_CHECK_NS NS_PER_S            /* the age check: at most once a second */
#define WB_STOP_NS  (3 * NS_PER_S)      /* an orderly stop's write-back, at most */
#define WB_SETTING  "files.writeback"   /* the age setting: seconds, 1 to WB_AGE_MAX */
#define WB_AGE_MAX  600u

/* A run begun in the running batch. */
struct wb_run {
    uint64_t key;
    uint64_t off, len;   /* its pages, bytes */
    uint64_t ok;         /* of a run a full volume cut short: the bytes written */
    status_t st;         /* its own error (ERR_NO_SPACE), else OK */
    int      slot;       /* its file's slot of maps[], or -1 */
    bool     gone;       /* its file went: the key detached, nothing to answer */
};

static _Alignas(PAGE_SIZE) uint8_t wbuf[WB_BUF];   /* a run's bytes, copied out and written */
static struct wb_run runs[WB_RUNS];
static unsigned nruns;
static bool     running;       /* a batch is running (a run begun) */
static bool     began;         /* ... in a transaction it began */
static status_t given_up;      /* ... given up: every run fails with this */
static uint64_t writes;        /* the disk's writes when it began */
static status_t first;         /* the first error a sync's write-backs met */
static uint64_t age_ns = PAGER_WRITEBACK_AGE_NS;   /* the setting */
static uint64_t next_check;    /* when the age check is due */
static bool     failed_said, kind_said;

static uint64_t page_down(uint64_t n)
{
    return n & ~(uint64_t)(PAGE_SIZE - 1);
}

/* ---- a batch ----------------------------------------------------------------------------- */

static void batch_begin(void)
{
    if (running)
        return;
    running = true;
    began = false;
    given_up = OK;
    reply_flush();   /* the last request's answer goes before the disk is written */
    writes = disk_writes();
    if (!writable()) {
        given_up = ERR_IO;   /* the runs are begun and failed: their pages don't pin the
                              * dirty limit (Q1) */
        return;
    }
    status_t st = tx_begin();
    if (st != OK)
        given_up = ERR_IO;
    began = st == OK;
}

/* The batch given up (a disk error, a commit that failed): every run fails. */
static void give_up(status_t st)
{
    if (given_up == OK)
        given_up = st == ERR_NO_SPACE ? ERR_NO_SPACE : ERR_IO;
}

static status_t stamp(struct ext_inode *in)
{
    struct ext_time t;
    time_now(&t);
    status_t st = ext_itime_set(in, EXT_MTIME, &t);
    if (st == OK)
        st = ext_itime_set(in, EXT_CTIME, &t);
    return st == OK ? ext_iput(&vol.fs, in) : st;
}

/* Run r's bytes (at data) written to its file, up to the file's size. */
static void put_run(struct wb_run *r, const uint8_t *data)
{
    struct ext_inode in;
    uint32_t ro = vol.fs.ro;
    status_t st = maps_key(r->key, &in, &r->slot);
    if (st == ERR_NOT_FOUND && r->slot < 0) {   /* the file is gone (a fresh start's key):
                                                 * its data with it */
        maps_detach(r->key);
        r->gone = true;
        return;
    }
    if (st == OK && given_up != OK)
        return;
    uint64_t size = ext_isize(&in), n = 0;
    if (st == OK && r->off < size)
        n = size - r->off < r->len ? size - r->off : r->len;
    size_t got = 0;
    if (st == OK && n)
        st = ext_write(&vol.fs, &in, r->off, data, n, &got);
    if (st == ERR_NO_SPACE) {   /* what was written stays */
        r->st = ERR_NO_SPACE;
        r->ok = page_down(got);
        st = got ? OK : ERR_NO_SPACE;
    }
    if (st == OK && got)
        st = stamp(&in);
    if (st != OK && r->st == OK) {
        give_up(st);
        if ((vol.fs.ro & EXT_RO_DAMAGED) && !(ro & EXT_RO_DAMAGED))
            went_bad("a mapped file's write-back");
    }
}

/* Run r ended: WRITTEN over its pages, with the batch's error (bst) or
 * its own (the pages a full volume left out), an error kept for its file. */
static void written(struct wb_run *r, status_t bst)
{
    if (r->gone)
        return;
    status_t st = bst != OK ? bst : r->st;
    uint64_t ok = bst != OK ? 0 : r->st != OK ? r->ok : r->len;
    status_t a = OK, b = OK;
    if (ok)
        a = jam_pager_op(vol.pager, r->key, PAGER_OP_WRITTEN, r->off, ok, OK);
    if (ok < r->len)
        b = jam_pager_op(vol.pager, r->key, PAGER_OP_WRITTEN, r->off + ok, r->len - ok, st);
    if ((a != OK && a != ERR_NOT_FOUND) || (b != OK && b != ERR_NOT_FOUND))
        printf("extfs %s: mapped file %lx: can't end its write-back: %s\n", vol.name,
               (unsigned long)r->key, status_str(a != OK ? a : b));
    test_die(XF_DIE_WBACK);   /* answered */
    if (st == OK)
        return;
    if (first == OK)
        first = st;
    if (r->slot >= 0 && kept->maps[r->slot].used && kept->maps[r->slot].key == r->key &&
        kept->maps[r->slot].error == OK)
        kept->maps[r->slot].error = st;   /* for the file's next file.sync */
    if (!failed_said)
        printf("extfs %s: mapped file %lx: stores at %lu (%lu bytes) not written: %s\n",
               vol.name, (unsigned long)r->key, (unsigned long)(r->off + ok),
               (unsigned long)(r->len - ok), status_str(st));
    failed_said = true;
}

/* The running batch committed (or given up) and its runs answered. */
static void batch_end(void)
{
    if (!running)
        return;
    status_t st = given_up;
    if (st == OK && began) {
        uint32_t tid = vol.jnl.tid;
        st = tx_commit();
        /* Its data on the medium: a commit flushed it; overwrites in place
         * alone (nothing committed) flush here. */
        if (st == OK && vol.jnl.tid == tid && disk_writes() != writes)
            st = disk_flush();
        if (st != OK)
            st = ERR_IO;
        else
            test_die(XF_DIE_WBACK);   /* committed, not answered */
    } else if (began) {
        tx_abort();
    }
    for (unsigned i = 0; i < nruns; i++)
        written(&runs[i], st);
    nruns = 0;
    running = false;
}

/* One run begun and written: pager_dirty with BEGIN and `flags` (0 or
 * PAGER_DIRTY_OLDEST) over *run (in: the range; out: the run, len 0 if
 * none), its bytes at dst (OLDEST: dst's start; else their place from the
 * range's start). Its errors: pager_dirty's. */
static status_t one_run(uint64_t key, uint32_t flags, struct pager_run *run, uint8_t *dst)
{
    if (nruns && (nruns == WB_RUNS || (began && given_up == OK &&
                                       ext_jnl_room(&vol.fs) < WB_ROOM)))
        batch_end();
    uint64_t from = run->offset;
    status_t st = jam_pager_dirty(vol.pager, key, flags | PAGER_DIRTY_BEGIN, run, dst);
    if (st == ERR_INVALID_ARGS && !kind_said) {   /* a VMO that isn't writable: a bug */
        printf("extfs %s: mapped file %lx: its pages can't be written back: %s\n", vol.name,
               (unsigned long)key, status_str(st));
        kind_said = true;
    }
    if (st != OK || !run->len)
        return st;
    batch_begin();
    test_die(XF_DIE_WBACK);   /* begun, nothing written */
    struct wb_run *r = &runs[nruns++];
    *r = (struct wb_run){
        .key = flags & PAGER_DIRTY_OLDEST ? run->key : key, .off = run->offset, .len = run->len,
        .slot = -1,
    };
    put_run(r, flags & PAGER_DIRTY_OLDEST ? dst : dst + (run->offset - from));
    test_die(XF_DIE_WBACK);   /* written, not committed */
    return OK;
}

/* The dirty pages of key's VMO in [at, end), written back. */
static void write_range(uint64_t key, uint64_t at, uint64_t end)
{
    while (at < end) {
        uint64_t len = end - at < WB_BUF ? end - at : WB_BUF;
        struct pager_run run = { .offset = at, .len = len };
        if (one_run(key, 0, &run, wbuf) != OK)
            return;
        at = run.len ? run.offset + run.len : at + len;
    }
}

/* Every page dirty at t0, of every VMO the pager has, until `until`: the
 * oldest first, each run of an age since t0 or more (a page dirtied after
 * t0 is younger: a writer that never stops can't keep it going). */
static void write_old(uint64_t t0, uint64_t until)
{
    for (;;) {
        uint64_t t = now();
        if (t >= until) {
            if (first == OK)
                first = ERR_TIMED_OUT;
            return;
        }
        struct pager_run run = { .len = WB_BUF, .age_ns = t - t0 };
        if (one_run(0, PAGER_DIRTY_OLDEST, &run, wbuf) != OK || !run.len)
            return;
    }
}

/* ---- the age setting ---------------------------------------------------------------------- */

/* files.writeback in the volume's own etc/settings (the system volume's
 * /data/etc/settings, <settings.h>), read with the ext library: extfs
 * can't open its own files. */
static void read_setting(void)
{
    struct ext_fs *fs = &vol.fs;
    struct ext_inode dir, f;
    struct ext_dentry d;
    if (ext_iget(fs, EXT_ROOT_INO, 0, &dir) != OK ||
        ext_dir_lookup(fs, &dir, "etc", 3, &d) != OK || ext_iget(fs, d.ino, 0, &dir) != OK ||
        ext_dir_lookup(fs, &dir, "settings", 8, &d) != OK || ext_iget(fs, d.ino, 0, &f) != OK ||
        (f.d.mode & EXT_S_IFMT) != EXT_S_IFREG)
        return;
    uint64_t n = ext_isize(&f) < SETTINGS_MAX ? ext_isize(&f) : SETTINGS_MAX;
    size_t got = 0;
    char v[SETTINGS_VALUE_MAX];
    if (ext_read(fs, &f, 0, wbuf, n, &got) != OK ||
        !settings_lookup((const char *)wbuf, got, WB_SETTING, v, sizeof(v)))
        return;
    uint32_t s = 0;
    const char *p = v;
    for (; *p >= '0' && *p <= '9' && s <= WB_AGE_MAX; p++)
        s = s * 10 + (uint32_t)(*p - '0');
    if (*p || !s || s > WB_AGE_MAX) {
        printf("extfs %s: %s = %s: not 1 to %u seconds; %lu it is\n", vol.name, WB_SETTING, v,
               WB_AGE_MAX, (unsigned long)(age_ns / NS_PER_S));
        return;
    }
    age_ns = (uint64_t)s * NS_PER_S;
    printf("extfs %s: mapped files' stores written back after %u s (%s)\n", vol.name, s,
           WB_SETTING);
}

/* ---- the callers -------------------------------------------------------------------------- */

void wback_start(void)
{
    if (!vol.pager)
        return;
    read_setting();
    next_check = now() + WB_CHECK_NS;
}

void wback_request(const struct pager_request *r)
{
    if (vol.disk_gone) {   /* cut off, as volumes' close of the pager will */
        maps_detach(r->key);
        return;
    }
    write_range(r->key, r->offset, r->offset + r->len);
    batch_end();
}

uint64_t wback_due(void)
{
    return vol.pager && writable() ? next_check : DEADLINE_NEVER;
}

void wback_tick(void)
{
    uint64_t t = now();
    if (!vol.pager || !writable() || t < next_check)
        return;
    uint64_t wait = WB_CHECK_NS;   /* the most a check writes, written: more may be due */
    for (unsigned n = 0; n < WB_AGE_RUNS; n++) {
        struct pager_run run = { .len = WB_BUF, .age_ns = age_ns };
        status_t st = one_run(0, PAGER_DIRTY_OLDEST, &run, wbuf);
        if (st == OK && run.len)
            continue;
        if (st == OK)   /* the oldest is younger: due when the window is out */
            wait = age_ns - (run.age_ns < age_ns ? run.age_ns : age_ns);
        else            /* none dirty: none can be due before a whole window */
            wait = age_ns;
        break;
    }
    batch_end();
    next_check = now() + (wait > WB_CHECK_NS ? wait : WB_CHECK_NS);
}

status_t wback_file(uint32_t ino, uint32_t igen)
{
    if (!vol.pager || ino > XF_SHARED_INO_MAX)
        return OK;
    uint64_t key = XF_SHARED_KEY(ino, igen);
    struct ext_inode in;
    if (ext_iget(&vol.fs, ino, EXT_IGET_UNLINKED, &in) != OK)
        return OK;   /* (the caller's inode_of says why) */
    uint64_t end = ext_isize(&in) < VMO_SMALL_MAX_SIZE ? ext_isize(&in) : VMO_SMALL_MAX_SIZE;
    first = OK;
    maps_again();   /* (run again from its slot by adopt.c: a cut write-back's pages dirty) */
    write_range(key, 0, (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
    batch_end();
    int s = maps_slot_of(key);
    if (s < 0)
        return first;
    status_t st = kept->maps[s].error;   /* the first since its last sync, ours with it */
    kept->maps[s].error = OK;
    return st;
}

status_t wback_all(void)
{
    if (!vol.pager)
        return OK;
    first = OK;
    maps_again();
    write_old(now(), DEADLINE_NEVER);
    batch_end();
    return first;
}

void wback_stop(void)
{
    if (!vol.pager || !writable())   /* (stopped already: the journal closed) */
        return;
    uint64_t t = now();
    first = OK;
    maps_again();
    write_old(t, t + WB_STOP_NS);
    batch_end();
    if (first == ERR_TIMED_OUT)
        printf("extfs %s: stopping: mapped files' stores left unwritten after %lu s\n", vol.name,
               (unsigned long)(WB_STOP_NS / NS_PER_S));
}
