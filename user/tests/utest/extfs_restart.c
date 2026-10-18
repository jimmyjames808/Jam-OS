/* utest: extfs carries on from a dead instance (user/services/extfs/adopt.c),
 * as fat_restart.c tests fat: the test plays the spawner (the state VMO,
 * the keeper, the `fs` channel's server end kept and a copy handed to each
 * instance) and restarts extfs over a new `block` session of the same RAM
 * disk whenever it dies. extfs's test powers end an instance at an exact
 * step of a request: a device write landing (file data, the log, its commit
 * block, a block written home), the transaction committed in the journal
 * with its slot not yet, the slot committed, the reply sent.
 *
 * The check: a script of every kind of request (files written and left
 * open, a symbolic and a hard link, a rename and an exchange, a file
 * unlinked while open, a truncate, a chmod) runs once undisturbed and
 * again with extfs ended at steps of it, several times a run; the client
 * sees no error and the same answers, and afterwards the volume holds
 * the same files with the same free space as the undisturbed run's (the
 * journal replays, so the blocks differ; the files may not). Then: an
 * fs.open's and an fs.view's reply that died with the process are made
 * again; a request in progress at two crashes is answered ERR_IO; mapped
 * files carried on (with a pager: maps, page requests and a write under a
 * mapping ended at their steps, an unlinked mapped file kept across a
 * kill); and a warm spare promoted in a dead instance's place carries on
 * its files. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <keep.h>
#include <os.h>
#include <svcstate.h>
#include "extfstest.h"
#include "utest.h"

#define DISK_SECTORS (32u * MIB_SECTORS)
#define CHUNK        (64u << 10)
#define BIG_CHUNKS   5u
#define MAX_DEATHS   8u
#define SUP_POLL     (50 * NS_PER_MS)

struct sup {
    struct fatrun   r;
    handle_t        serve;
    handle_t        state;
    handle_t        pager;    /* handed to each instance (EXTFS_SR_PAGER), or 0 */
    struct keeper   keeper;
    const char     *dies[MAX_DEATHS];
    const char     *crash;
    unsigned        instance;
    unsigned        tests, crashes;
    bool            bad;
    bool            stop;
    bool            fresh;    /* the next start gets a new state: it starts fresh */
    handle_t        thread;
    _Alignas(16) uint8_t stack[32 << 10];
};

static struct sup sv;
static struct ramdisk blank_disk, test_disk;
static uint8_t buf[CHUNK];
static uint64_t ref_free;
static bool lazy_tail;   /* instances started with tail-lazy (extfs/test.c) */

static uint64_t dl(void)
{
    return now() + FAT_CALL_NS;
}

static status_t start(struct sup *s, const char *ended)
{
    handle_t block, serve, state, keep;
    if (!ramdisk_serve(s->r.rd, false, &block))
        return ERR_INTERNAL;
    status_t st = OK;
    if (__atomic_exchange_n(&s->fresh, false, __ATOMIC_ACQ_REL)) {
        jam_handle_close(s->state);
        st = svcstate_create(EXTFS_STATE_SIZE, &s->state);
    }
    if (st == OK)
        st = jam_handle_duplicate(s->serve, RIGHT_SAME, &serve);
    if (st == OK)
        st = svcstate_give(s->state, &state);
    if (st == OK)
        st = keeper_attach(&s->keeper, &keep);
    if (st == OK)
        st = new_job(&s->r.job);
    if (st != OK)
        return st;
    const char *argv[8] = { "extfs", "utest", EXTFS_ARG_FORMAT, "fixed-time" };
    int argc = 4;
    if (ended)
        argv[argc++] = ended;
    if (s->instance < MAX_DEATHS && s->dies[s->instance])
        argv[argc++] = s->dies[s->instance];
    if (s->crash)
        argv[argc++] = s->crash;
    if (lazy_tail)
        argv[argc++] = "tail-lazy";
    struct spawn_handle x[5] = { { EXTFS_SR_BLOCK, block }, { EXTFS_SR_SERVE, serve },
                                 { SR_STATE, state }, { SR_KEEP, keep } };
    rights_t xr[5] = { RIGHT_SAME, RIGHT_SAME, SVCSTATE_SERVICE_RIGHTS, RIGHT_SAME,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT };
    unsigned nx = 4;
    if (s->pager && jam_handle_duplicate(s->pager, RIGHT_SAME, &x[nx].h) == OK)
        x[nx++].role = EXTFS_SR_PAGER;
    struct spawn_args a = { .path = "bin/extfs", .argc = argc, .argv = argv, .job = s->r.job,
                            .extra = x, .nextra = nx, .extra_rights = xr };
    st = spawn(&a, &s->r.proc);
    s->instance++;
    if (st != OK)
        return st;
    /* An instance a test power ends at its first write (die=write:1, in its
     * mount) may be gone before the keeper writes to it: the supervisor
     * sees it end and starts the next, as for any other death. */
    st = keeper_restore(&s->keeper);
    return st == ERR_PEER_CLOSED ? OK : st;
}

static bool reap(struct sup *s, struct process_info *info)
{
    CHECK_ST(jam_process_get_info(s->r.proc, info), OK);
    jam_job_kill(s->r.job);
    CHECK_ST(jam_handle_close(s->r.proc), OK);
    CHECK_ST(jam_handle_close(s->r.job), OK);
    return ramdisk_join(s->r.rd);
}

static void supervise(void *arg)
{
    struct sup *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        signals_t seen;
        if (jam_object_wait_one(s->r.proc, SIG_TERMINATED, now() + SUP_POLL, &seen) != OK) {
            /* What the instance sends its keeper taken as it comes, as
             * volumes takes it: a drop then closes the keeper's copy (a
             * watcher channel a successor refused closes for its client). */
            (void)keeper_drain_upto(&s->keeper, 64);
            continue;
        }
        struct process_info info;
        if (!reap(s, &info)) {
            s->bad = true;
            return;
        }
        bool test = !info.killed && info.exit_code == EXTFS_EXIT_TEST;
        if (!test && !info.killed) {
            printf("utest: extfs ended with %ld, not by a test power or a crash\n",
                   (long)info.exit_code);
            s->bad = true;
            return;
        }
        s->tests += test;
        s->crashes += info.killed;
        status_t st = start(s, test ? EXTFS_ARG_KILLED : EXTFS_ARG_CRASHED);
        if (st != OK) {
            printf("utest: extfs couldn't be started again (%s)\n", status_str(st));
            s->bad = true;
            return;
        }
    }
}

/* pager: handed to every instance (a copy each), or 0. */
static bool sup_begin(struct ramdisk *rd, const char *const *dies, unsigned n, const char *crash,
                      handle_t pager)
{
    memset(&sv, 0, offsetof(struct sup, stack));
    sv.r.rd = rd;
    sv.pager = pager;
    for (unsigned i = 0; i < n && i < MAX_DEATHS; i++)
        sv.dies[i] = dies[i];
    sv.crash = crash;
    keeper_init(&sv.keeper);
    CHECK_ST(jam_channel_create(&sv.r.fs, &sv.serve), OK);
    CHECK_ST(svcstate_create(EXTFS_STATE_SIZE, &sv.state), OK);
    CHECK_ST(start(&sv, NULL), OK);
    CHECK_ST(thread_spawn("extfs supervisor", supervise, &sv, sv.stack, sizeof(sv.stack),
                          &sv.thread), OK);
    return true;
}

static bool sup_start(struct ramdisk *rd, const char *const *dies, unsigned n, const char *crash)
{
    return sup_begin(rd, dies, n, crash, HANDLE_INVALID);
}

static bool sup_stop(void)
{
    __atomic_store_n(&sv.stop, true, __ATOMIC_RELEASE);
    CHECK(wait_threads(&sv.thread, 1));
    CHECK(!sv.bad);
    CHECK_ST(jam_handle_close(sv.r.fs), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(sv.r.proc, FAT_CALL_NS, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(sv.r.proc), OK);
    jam_job_kill(sv.r.job);
    CHECK_ST(jam_handle_close(sv.r.job), OK);
    keeper_release(&sv.keeper);
    CHECK_ST(jam_handle_close(sv.serve), OK);
    CHECK_ST(jam_handle_close(sv.state), OK);
    return ramdisk_join(sv.r.rd);
}

/* The supervisor for the other files' restart cases (extfstest.h). */
bool extfs_sup_start(struct ramdisk *rd, const char *const *dies, unsigned n)
{
    return sup_start(rd, dies, n, NULL);
}

bool extfs_sup_stop(void)
{
    return sup_stop();
}

struct fatrun *extfs_sup_run(void)
{
    return &sv.r;
}

unsigned extfs_sup_ended(void)
{
    return sv.tests;
}

void extfs_sup_fresh(void)
{
    __atomic_store_n(&sv.fresh, true, __ATOMIC_RELEASE);
}

/* ---- the script ---------------------------------------------------------------------- */

static void fill(uint32_t seed)
{
    for (uint32_t j = 0; j < CHUNK; j++)
        buf[j] = (uint8_t)(seed * 131 + j * 7 + j / 4096);
}

static bool write_chunks(struct tfile *f, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t done = 0;
        fill(seed + i);
        CHECK_ST(t_write(f, (uint64_t)i * CHUNK, buf, CHUNK, &done), OK);
        CHECK_EQ(done, CHUNK);
    }
    return true;
}

static bool read_chunks(struct tfile *f, uint32_t n, uint32_t seed)
{
    static uint8_t got[CHUNK];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t done = 0;
        fill(seed + i);
        CHECK_ST(t_read(f, (uint64_t)i * CHUNK, got, CHUNK, &done), OK);
        CHECK_EQ(done, CHUNK);
        if (memcmp(got, buf, CHUNK))
            FAIL("chunk %u of a file written with seed %u reads back wrong", i, seed);
    }
    return true;
}

static status_t lnk(const struct fatrun *r, const char *target, const char *path, bool sym)
{
    uint32_t a = (uint32_t)strlen(target), b = (uint32_t)strlen(path);
    return sym ? fs_symlink_until(r->fs, dl(), target, a, path, b)
               : fs_link_until(r->fs, dl(), target, a, path, b);
}

/* What the script leaves, checked (after it, and on a new instance). */
static bool verify(const struct fatrun *r)
{
    struct tfile big;
    uint32_t mode = 0, nlink = 0;
    uint64_t size = 0;
    CHECK_ST(t_stat(r, "/u", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_stat(r, "/d/e", NULL, NULL, NULL), ERR_NOT_FOUND);
    if (!file_is(r, "/d/b.txt", "alpha") || !file_is(r, "/d/la", "alpha") ||
        !file_is(r, "/d/ha", "alpha") || !file_is(r, "/d/x", "y") || !file_is(r, "/d/y", "x"))
        return false;
    CHECK_ST(xmode(r->fs, "/d/b.txt", 0, &mode, &nlink), OK);
    CHECK(mode == (FS_MODE_REG | 0600u) && nlink == 2);
    CHECK_ST(xmode(r->fs, "/d/la", FS_NOFOLLOW, &mode, NULL), OK);
    CHECK_EQ(mode & FS_MODE_FMT, FS_MODE_LNK);
    CHECK_ST(t_stat(r, "/big.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, 2ull * CHUNK);
    CHECK_ST(t_open(r, "/big.bin", FS_READ, &big), OK);
    bool ok = read_chunks(&big, 2, 1);
    t_close(&big);
    return ok;
}

static bool script(const struct fatrun *r)
{
    struct tfile big, u;
    uint32_t done = 0;
    char got[16];
    CHECK_ST(t_mkdir(r, "/d"), OK);
    if (!put_file(r, "/d/a.txt", "alpha"))
        return false;
    CHECK_ST(t_open(r, "/big.bin", FS_READ | FS_WRITE | FS_CREATE, &big), OK);
    if (!write_chunks(&big, BIG_CHUNKS, 1))
        return false;
    CHECK_ST(lnk(r, "/d/a.txt", "/d/ha", false), OK);
    CHECK_ST(t_rename(r, "/d/a.txt", "/d/b.txt"), OK);
    CHECK_ST(lnk(r, "b.txt", "/d/la", true), OK);
    CHECK_ST(t_open(r, "/u", FS_READ | FS_WRITE | FS_CREATE, &u), OK);
    CHECK_ST(t_write(&u, 0, "unlinked", 8, &done), OK);
    CHECK_ST(t_unlink(r, "/u"), OK);
    CHECK_ST(t_read(&u, 0, got, 8, &done), OK);
    CHECK(done == 8 && !memcmp(got, "unlinked", 8));
    t_close(&u);
    CHECK_ST(file_truncate_until(big.ch, dl(), 2ull * CHUNK), OK);
    if (!read_chunks(&big, 2, 1))
        return false;
    t_close(&big);
    CHECK_ST(fs_setattr_until(r->fs, dl(), "/d/b.txt", 8, 0, FS_ATTR_MODE, 0600, 0, 0, 0, 0), OK);
    if (!put_file(r, "/d/x", "x") || !put_file(r, "/d/y", "y"))
        return false;
    CHECK_ST(t_rename_flags(r, "/d/x", "/d/y", FS_RENAME_EXCHANGE), OK);
    CHECK_ST(t_mkdir(r, "/d/e"), OK);
    if (!put_file(r, "/d/e/c.txt", "gamma"))
        return false;
    CHECK_ST(t_unlink(r, "/d/e/c.txt"), OK);
    CHECK_ST(t_unlink(r, "/d/e"), OK);
    CHECK_ST(t_sync(r), OK);
    return verify(r);
}

/* A new instance (no deaths) reads what the script left: the same files and
 * the same free space as the undisturbed run's. */
static bool after(struct ramdisk *rd, const char *save)
{
    struct fatrun r;
    uint64_t total = 0, fr = 0;
    CHECK(extfs_start(&r, rd, false, NULL, 0));
    bool ok = verify(&r);
    CHECK_ST(t_free(&r, &total, &fr), OK);
    if (!ref_free)
        ref_free = fr;
    else if (fr != ref_free)
        FAIL("%lu bytes free, the undisturbed run left %lu", (unsigned long)fr,
             (unsigned long)ref_free);
    CHECK(fat_stop(&r));
    return ok && (!save || extfs_save(rd, save));
}

static bool storm(const char *const *dies, unsigned n, bool from_blank, const char *save)
{
    if (from_blank)
        memset(test_disk.mem, 0, (size_t)DISK_SECTORS * RAMDISK_SECTOR);
    else
        memcpy(test_disk.mem, blank_disk.mem, (size_t)DISK_SECTORS * RAMDISK_SECTOR);
    if (!sup_start(&test_disk, dies, n, NULL) || !script(&sv.r))
        return false;
    unsigned ended = sv.tests;
    if (!sup_stop())
        return false;
    if (ended != n)
        FAIL("%u of %u test powers ended extfs (%s ...): pick steps it reaches", ended, n,
             dies[0]);
    return after(&test_disk, save);
}

bool t_extfs_restart_steps(void)
{
    static const char *const steps1[] = {
        "die=write:1", "die=journal:1", "die=commit:1", "die=answered:1", "die=write:2",
    };
    static const char *const steps2[] = {
        "die=write:9", "die=journal:4", "die=commit:6", "die=answered:5", "die=write:7",
    };
    static const char *const steps3[] = {
        "die=answered:8", "die=write:41", "die=journal:5", "die=commit:6", "die=write:3",
        "die=write:23", "die=journal:2",
    };
    static const char *const format[] = { "die=write:40", "die=write:3" };
    struct fatrun r;
    if (!ramdisk_create(&blank_disk, DISK_SECTORS) || !ramdisk_create(&test_disk, DISK_SECTORS))
        return false;
    ref_free = 0;
    /* A formatted disk every run starts from (fixed-time: the same each time). */
    const char *fixed = "fixed-time";
    /* The steps were counted when the journal's tail moved every 64 commits,
     * so a start replayed a long log and its writes counted: those storms run
     * with tail-lazy. The first runs again with the tail as it is (a start
     * replays one or two transactions). */
    bool ok = extfs_start(&r, &blank_disk, false, &fixed, 1) && fat_stop(&r) &&
              storm(NULL, 0, false, NULL) && storm(steps1, 5, false, NULL);
    lazy_tail = true;
    ok = ok && storm(steps1, 5, false, NULL) && storm(steps2, 5, false, NULL) &&
         storm(steps3, 7, false, "restart") && storm(format, 2, true, NULL);
    lazy_tail = false;
    return ramdisk_destroy(&test_disk) && ramdisk_destroy(&blank_disk) && ok;
}

/* ---- replies that carried handles ------------------------------------------------------ */

/* fs.open's and fs.view's reply, the instance ended after the journal's
 * commit and before the slot's (journal:1), and after the answer
 * (answered:1): the client gets working handles once. */
bool t_extfs_restart_handles(void)
{
    static struct ramdisk test_disk;   /* each test its own: a failed one leaves its own */
    static const char *const dies[] = {
        "die=answered:1", "die=journal:1", "die=answered:1", "die=commit:1",
    };
    struct tfile f;
    uint32_t done = 0;
    char got[8] = { 0 };
    handle_t view = HANDLE_INVALID;
    if (!ramdisk_create(&test_disk, DISK_SECTORS) || !sup_start(&test_disk, dies, 4, NULL))
        return false;
    CHECK_ST(t_sync(&sv.r), OK);   /* instance 0's first answer, then it ends */
    CHECK_ST(t_open(&sv.r, "/h.txt", FS_READ | FS_WRITE | FS_CREATE, &f), OK);   /* 1: remade */
    CHECK_ST(t_write(&f, 0, "made", 4, &done), OK);   /* 2: answered, then it ends */
    CHECK_ST(fs_view_until(sv.r.fs, dl(), FS_VIEW_READ_ONLY, &view), OK);   /* 3: remade */
    struct fatrun v = { .fs = view };
    CHECK_ST(t_read(&f, 0, got, 4, &done), OK);
    CHECK(!memcmp(got, "made", 4));
    t_close(&f);
    CHECK_ST(t_mkdir(&v, "/no"), ERR_ACCESS_DENIED);   /* the view's flags came back */
    CHECK(file_is(&v, "/h.txt", "made"));
    CHECK_ST(jam_handle_close(view), OK);
    CHECK_EQ(sv.tests, 4u);
    return sup_stop() && ramdisk_destroy(&test_disk);
}

/* ---- the bad-request rule ------------------------------------------------------------- */

bool t_extfs_restart_bad(void)
{
    static const char *const kills[] = { "die=write:3", "die=write:3" };
    static struct ramdisk test_disk;
    bool is_dir = false;
    if (!ramdisk_create(&test_disk, DISK_SECTORS) ||
        !sup_start(&test_disk, NULL, 0, "crash-on=bad"))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/d"), OK);
    CHECK_ST(t_stat(&sv.r, "/d/bad", NULL, NULL, NULL), ERR_IO);
    CHECK_EQ(sv.crashes, 2u);
    CHECK_ST(t_stat(&sv.r, "/d", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    if (!sup_stop())
        return false;
    if (!sup_start(&test_disk, kills, 2, NULL))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/k"), OK);   /* ended twice at its third write */
    CHECK_EQ(sv.tests, 2u);
    CHECK_ST(t_stat(&sv.r, "/k", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    return sup_stop() && ramdisk_destroy(&test_disk);
}

/* ---- a request in progress at two deaths in a row ---------------------------------- */

/* The first instance ends with a request's log written and its commit block
 * not (its third write: the superblock's needs_recovery, the tail, the log);
 * its successor ends at its first write, the replayed log marked empty with
 * the next id, before the request runs again. The third must run it again,
 * not answer it from the slot as done: it was never committed
 * (docs/M13.6-REVIEW.md's R2). */
bool t_extfs_restart_twice(void)
{
    static const char *const dies[] = { "die=write:3", "die=write:1" };
    static struct ramdisk test_disk;
    bool is_dir = false;
    if (!ramdisk_create(&test_disk, DISK_SECTORS) || !sup_start(&test_disk, NULL, 0, NULL))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/a"), OK);   /* formatted, then closed clean */
    if (!sup_stop() || !sup_start(&test_disk, dies, 2, NULL))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/k"), OK);
    CHECK_EQ(sv.tests, 2u);
    CHECK_ST(t_stat(&sv.r, "/k", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    return sup_stop() && ramdisk_destroy(&test_disk);
}

/* ---- mapped files ------------------------------------------------------------------- */

#define MAP_CHUNKS 3u          /* the mapped file: 48 pages */
#define MAP_BUDGET 32u         /* the pager's pages: fewer, so a second pass fetches again */
#define PATCH      "patched"   /* written at PATCH_AT under the mapping */
#define PATCH_AT   1000u

/* VMO v's pages: what write_chunks(seed) wrote (PATCH over it if
 * patched), read a chunk at a time (vmo_read waits for pages as a fault). */
static bool vmo_chunks(handle_t v, uint32_t seed, bool patched, const char *what)
{
    static uint8_t got[CHUNK];
    for (uint32_t i = 0; i < MAP_CHUNKS; i++) {
        fill(seed + i);
        if (patched && !i)
            memcpy(buf + PATCH_AT, PATCH, sizeof(PATCH) - 1);
        CHECK_ST(jam_vmo_read(v, (uint64_t)i * CHUNK, got, CHUNK), OK);
        if (memcmp(got, buf, CHUNK))
            FAIL("%s: chunk %u of the mapped file reads wrong", what, i);
    }
    return true;
}

/* Mapped files across restarts (pager.c's "Restarts"): a map whose
 * instance ended having made its VMO (map:1) is run again; a page request
 * read and not supplied (supply:1) is taken again by the successor
 * (PAGER_NEXT_AGAIN); a write under a mapping ended after its journal
 * commit (journal:1) is seen by the mapping; a map to run answered and
 * then ended (answered:2) gets its VMO made again. Then the file, unlinked
 * under its mapping, is kept across a kill (the orphan the state's slot
 * names) and read again from the disk (the budget is less than the file);
 * once the mapping goes the orphan is dropped. */
bool t_extfs_restart_pager(void)
{
    static struct ramdisk test_disk;
    static const char *const dies[] = {
        "die=map:1", "die=supply:1", "die=journal:1", "die=answered:2",
    };
    handle_t root = startup_handle(SR_RESOURCE), pager = HANDLE_INVALID;
    handle_t a = HANDLE_INVALID, x = HANDLE_INVALID;
    struct tfile f, g;
    uint64_t size = 0, total = 0, free_before = 0, free_now = 0;
    uint32_t done = 0;
    if (!root || jam_pager_create(root, MAP_BUDGET, FAT_CALL_NS, 0, &pager) != OK) {
        printf("utest: extfs_restart_pager: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    if (!ramdisk_create(&test_disk, DISK_SECTORS) || !sup_begin(&test_disk, dies, 4, NULL, pager))
        return false;
    CHECK_ST(t_free(&sv.r, &total, &free_before), OK);
    CHECK_ST(t_open(&sv.r, "/m.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    if (!write_chunks(&f, MAP_CHUNKS, 3))
        return false;
    CHECK_ST(file_map_until(f.ch, dl(), FILE_MAP_READ, &a, &size), OK);   /* 0 ends */
    CHECK_EQ(size, MAP_CHUNKS * CHUNK);
    if (!vmo_chunks(a, 3, false, "mapped"))   /* 1 ends at its first supply */
        return false;
    CHECK_ST(t_write(&f, PATCH_AT, PATCH, sizeof(PATCH) - 1, &done), OK);   /* 2 ends */
    if (!vmo_chunks(a, 3, true, "written under it"))
        return false;
    t_close(&f);
    CHECK_ST(t_open(&sv.r, "/m.bin", FS_READ, &g), OK);
    CHECK_ST(file_map_until(g.ch, dl(), FILE_MAP_EXEC, &x, &size), OK);   /* 3 ends */
    if (!vmo_chunks(x, 3, true, "mapped to run"))
        return false;
    CHECK_EQ(sv.tests, 4u);
    CHECK_ST(t_unlink(&sv.r, "/m.bin"), ERR_BUSY);
    CHECK_ST(jam_handle_close(x), OK);
    t_close(&g);
    CHECK_ST(t_unlink(&sv.r, "/m.bin"), OK);
    jam_job_kill(sv.r.job);   /* the supervisor starts it again: a crash, nothing in progress */
    if (!vmo_chunks(a, 3, true, "unlinked, after a kill"))
        return false;
    CHECK_EQ(sv.crashes, 1u);
    CHECK_ST(t_free(&sv.r, &total, &free_now), OK);
    CHECK(free_now < free_before);   /* the orphan holds its blocks */
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(t_sync(&sv.r), OK);   /* the GONE first: the orphan dropped */
    CHECK_ST(t_free(&sv.r, &total, &free_now), OK);
    CHECK_EQ(free_now, free_before);
    CHECK_ST(t_stat(&sv.r, "/m.bin", NULL, NULL, NULL), ERR_NOT_FOUND);
    bool ok = sup_stop() && extfs_save(&test_disk, "restart-pager");
    CHECK_ST(jam_handle_close(pager), OK);
    return ramdisk_destroy(&test_disk) && ok;
}

/* ---- mapped files' stores written back ------------------------------------------------ */

#define WB_PAGES 4u   /* the written-back file's pages */

static uint8_t wb_want[WB_PAGES * PAGE_SIZE];

/* No page of the pager is dirty. */
static bool wb_clean(handle_t pager)
{
    static uint8_t page[PAGE_SIZE];
    struct pager_run run = { .len = PAGE_SIZE };
    return jam_pager_dirty(pager, 0, PAGER_DIRTY_OLDEST | PAGER_DIRTY_PEEK, &run, page) ==
           ERR_NOT_FOUND;
}

/* A page of seed's pattern stored at page p of mapping m, and in the model. */
static void wb_store(uint8_t *m, uint32_t p, uint32_t seed)
{
    fill(seed);
    memcpy(m + p * PAGE_SIZE, buf, PAGE_SIZE);
    memcpy(wb_want + p * PAGE_SIZE, buf, PAGE_SIZE);
}

/* The supervisor has seen n instances end by a test power (one may end
 * just after its answer). */
static bool wb_deaths(unsigned n)
{
    uint64_t end = dl();
    while (__atomic_load_n(&sv.tests, __ATOMIC_ACQUIRE) < n && now() < end)
        jam_sleep_until(now() + 10 * NS_PER_MS);
    CHECK_EQ(__atomic_load_n(&sv.tests, __ATOMIC_ACQUIRE), n);
    return true;
}

/* Right after a write-back was answered: no page dirty, and extfs's read
 * (the disk's bytes, nothing laid over them) the model. */
static bool wb_on_disk(struct tfile *f, handle_t pager, const char *what)
{
    static uint8_t got[WB_PAGES * PAGE_SIZE];
    uint32_t done = 0;
    if (!wb_clean(pager))
        FAIL("%s: a page still dirty", what);
    CHECK_ST(t_read(f, 0, got, sizeof(got), &done), OK);
    CHECK_EQ(done, sizeof(got));
    if (memcmp(got, wb_want, sizeof(got)))
        FAIL("%s: the disk doesn't hold the stores", what);
    return true;
}

/* A write-back ended at each step of WRITEMAP's 2.6 (wback.c's test
 * points, counted from each instance's start; a write-back of one run
 * passes four: begun, its data written, committed, answered), the msync
 * or fsync waiting on it answered OK by the successor, the data on the
 * disk right after:
 *   0  wback:1  begun, nothing written: dirty again, written again;
 *   1  wback:6  (the retry's four, then) the data written over the file's
 *               own block, no commit: written again;
 *   2  wback:7  committed, no WRITTEN: replayed, dirty again, written
 *               again, then answered;
 *   3  wback:8  answered, then ended: nothing left;
 *   4  wback:2  the data written into a hole's new blocks, no commit: they
 *               are free in the committed bitmaps, taken again;
 *   5  wback:7  a file.sync's write-back committed, not answered: the
 *               request runs again from its slot, its write-back with it. */
bool t_extfs_restart_wback(void)
{
    static struct ramdisk test_disk;
    static const char *const dies[] = {
        "die=wback:1", "die=wback:6", "die=wback:7", "die=wback:8", "die=wback:2", "die=wback:7",
    };
    handle_t root = startup_handle(SR_RESOURCE), pager = HANDLE_INVALID, a = HANDLE_INVALID;
    handle_t h = HANDLE_INVALID, self = startup_handle(SR_SELF_VMAR);
    struct tfile f, g;
    uint64_t size = 0, addr = 0, total = 0, before = 0, after = 0;
    uint32_t done = 0;
    if (!root || jam_pager_create(root, 4096, FAT_CALL_NS, 0, &pager) != OK) {
        printf("utest: extfs_restart_wback: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    if (!ramdisk_create(&test_disk, DISK_SECTORS) || !sup_begin(&test_disk, dies, 6, NULL, pager))
        return false;
    CHECK_ST(t_open(&sv.r, "/w.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    for (uint32_t p = 0; p < WB_PAGES; p++) {
        fill(20 + p);
        memcpy(wb_want + p * PAGE_SIZE, buf, PAGE_SIZE);
        CHECK_ST(t_write(&f, p * PAGE_SIZE, buf, PAGE_SIZE, &done), OK);
    }
    CHECK_ST(file_map_until(f.ch, dl(), FILE_MAP_WRITE, &a, &size), OK);
    CHECK_ST(jam_vmar_map(self, a, 0, sizeof(wb_want), VMAR_READ | VMAR_WRITE, &addr), OK);
    uint8_t *m = (uint8_t *)(uintptr_t)addr;
    for (uint32_t p = 0; p < WB_PAGES; p++) {
        wb_store(m, p, 30 + p);
        CHECK(!wb_clean(pager));
        CHECK_ST(jam_vmo_sync(a, 0, sizeof(wb_want), VMO_SYNC_WAIT), OK);
        if (!wb_deaths(p + 1) || !wb_on_disk(&f, pager, "an msync's write-back ended"))
            return false;
    }

    /* A hole: page 1 of /h.bin has no block until its write-back. */
    CHECK_ST(t_open(&sv.r, "/h.bin", FS_READ | FS_WRITE | FS_CREATE, &g), OK);
    CHECK_ST(file_truncate_until(g.ch, dl(), 3 * PAGE_SIZE), OK);
    CHECK_ST(t_free(&sv.r, &total, &before), OK);
    CHECK_ST(file_map_until(g.ch, dl(), FILE_MAP_WRITE, &h, &size), OK);
    CHECK_ST(jam_vmar_map(self, h, 0, 3 * PAGE_SIZE, VMAR_READ | VMAR_WRITE, &addr), OK);
    uint8_t *hm = (uint8_t *)(uintptr_t)addr;
    fill(40);
    memcpy(hm + PAGE_SIZE, buf, PAGE_SIZE);
    CHECK_ST(jam_vmo_sync(h, 0, 3 * PAGE_SIZE, VMO_SYNC_WAIT), OK);
    if (!wb_deaths(5))
        return false;
    CHECK(wb_clean(pager));
    static uint8_t got[PAGE_SIZE];
    CHECK_ST(t_read(&g, PAGE_SIZE, got, PAGE_SIZE, &done), OK);
    CHECK(done == PAGE_SIZE && !memcmp(got, buf, PAGE_SIZE));
    CHECK_ST(t_free(&sv.r, &total, &after), OK);
    CHECK(before - after >= PAGE_SIZE && before - after < 2 * PAGE_SIZE);   /* taken once */
    t_close(&g);

    /* A file.sync ended after its write-back's commit. */
    wb_store(m, 2, 50);
    CHECK(!wb_clean(pager));
    CHECK_ST(file_sync_until(f.ch, dl()), OK);
    if (!wb_deaths(6) || !wb_on_disk(&f, pager, "a file.sync's write-back ended"))
        return false;
    t_close(&f);
    CHECK_ST(jam_vmar_unmap(self, (uint64_t)(uintptr_t)m, sizeof(wb_want)), OK);
    CHECK_ST(jam_vmar_unmap(self, (uint64_t)(uintptr_t)hm, 3 * PAGE_SIZE), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(h), OK);
    bool ok = sup_stop() && extfs_save(&test_disk, "restart-wback");
    CHECK_ST(jam_handle_close(pager), OK);
    return ramdisk_destroy(&test_disk) && ok;
}

/* ---- the warm spare ---------------------------------------------------------------- */

/* A spare: bin/extfs with nothing but SR_STANDBY, in a job of its own. */
static bool spare(handle_t *job, handle_t *sup, handle_t *proc)
{
    handle_t end;
    CHECK_ST(new_job(job), OK);
    CHECK_ST(jam_channel_create(sup, &end), OK);
    const char *argv[] = { "extfs" };
    struct spawn_handle x = { SR_STANDBY, end };
    struct spawn_args a = { .path = "bin/extfs", .name = "extfs-spare", .argc = 1, .argv = argv,
                            .job = *job, .extra = &x, .nextra = 1 };
    CHECK_ST(spawn(&a, proc), OK);
    return true;
}

/* Promote it in sv's place: the handles a start gives, by role. */
static bool promote(handle_t sup, const char *ended)
{
    handle_t block, serve, state, keep;
    if (!ramdisk_serve(sv.r.rd, false, &block))
        return false;
    CHECK_ST(jam_handle_duplicate(sv.serve, RIGHT_SAME, &serve), OK);
    CHECK_ST(svcstate_give(sv.state, &state), OK);
    CHECK_ST(keeper_attach(&sv.keeper, &keep), OK);
    const char *argv[4] = { "extfs", "utest", EXTFS_ARG_FORMAT, ended };
    struct spawn_handle hs[4] = { { EXTFS_SR_BLOCK, block }, { EXTFS_SR_SERVE, serve },
                                  { SR_STATE, state }, { SR_KEEP, keep } };
    rights_t rights[4] = { RIGHT_SAME, RIGHT_SAME, SVCSTATE_SERVICE_RIGHTS, RIGHT_SAME };
    struct standby_args pa = { .hs = hs, .rights = rights, .n = 4, .argc = ended ? 4 : 3,
                               .argv = argv, .kill_ns = now() };
    CHECK_ST(standby_promote(sup, &pa), OK);
    CHECK_ST(keeper_restore(&sv.keeper), OK);
    return true;
}

bool t_extfs_spare(void)
{
    static struct ramdisk test_disk;
    handle_t job, sup, proc, job2, sup2, proc2;
    struct tfile f;
    uint32_t done = 0;
    char got[8];
    memset(&sv, 0, offsetof(struct sup, stack));
    if (!ramdisk_create(&test_disk, DISK_SECTORS))
        return false;
    sv.r.rd = &test_disk;
    keeper_init(&sv.keeper);
    CHECK_ST(jam_channel_create(&sv.r.fs, &sv.serve), OK);
    CHECK_ST(svcstate_create(EXTFS_STATE_SIZE, &sv.state), OK);
    CHECK(spare(&job, &sup, &proc) && promote(sup, NULL));
    CHECK_ST(t_mkdir(&sv.r, "/s"), OK);
    CHECK_ST(t_open(&sv.r, "/s/f", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(t_write(&f, 0, "spare", 5, &done), OK);

    /* The instance killed; the second spare promoted in its place. */
    CHECK(spare(&job2, &sup2, &proc2));
    jam_job_kill(job);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, FAT_CALL_NS, &info), OK);
    CHECK(info.killed);
    jam_handle_close(proc);
    jam_handle_close(job);
    jam_handle_close(sup);
    CHECK(ramdisk_join(&test_disk));
    CHECK(promote(sup2, EXTFS_ARG_KILLED));
    CHECK_ST(t_read(&f, 0, got, 5, &done), OK);   /* the same file channel, carried on */
    CHECK(done == 5 && !memcmp(got, "spare", 5));
    CHECK_ST(t_write(&f, 5, "!", 1, &done), OK);
    t_close(&f);
    CHECK(file_is(&sv.r, "/s/f", "spare!"));
    CHECK_ST(jam_handle_close(sv.r.fs), OK);
    CHECK_ST(spawn_wait(proc2, FAT_CALL_NS, &info), OK);
    CHECK(!info.killed && info.exit_code == 0);
    jam_handle_close(proc2);
    jam_job_kill(job2);
    jam_handle_close(job2);
    jam_handle_close(sup2);
    keeper_release(&sv.keeper);
    CHECK_ST(jam_handle_close(sv.serve), OK);
    CHECK_ST(jam_handle_close(sv.state), OK);
    return ramdisk_join(&test_disk) && ramdisk_destroy(&test_disk);
}
