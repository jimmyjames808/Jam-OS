/* utest: fat carries on from a dead instance's state (user/services/fat/
 * adopt.c; docs/M11.6-PLAN.md, "The demonstration and its tests",
 * deterministic deaths). The test plays devmgr: it makes the state VMO and
 * keeps it, is the keeper (<keep.h>), keeps the `fs` channel's server end
 * and hands each instance a duplicate, and restarts fat over a new `block`
 * session of the same RAM disk whenever it dies, telling it how the last
 * one ended. fat's test powers (test.c) end an instance at an exact step of
 * a request: a held write, the commit, a block write of the send, before
 * and after the answer.
 *
 * The check is the strongest there is: a script of requests (directories,
 * files written and left unsynced, a rename, an unlink, a 1.25 MiB file
 * written FS_GATHER) runs once undisturbed and again with fat ended at
 * every step, several times a run; the client sees no error and the same
 * data, and the two disks are the same byte for byte afterwards (fat's
 * fixed-time power: every timestamp is the same). Then: an fs.open's and an
 * fs.view's reply that died with the process are made again; a request in
 * progress at two crashes is answered ERR_IO with fat still serving, while
 * two deliberate kills of one request don't count. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fatsvc.h>
#include <fsview.h>
#include <keep.h>
#include <os.h>
#include <svcstate.h>
#include "fattest.h"
#include "utest.h"

#define DISK_SECTORS (16u * MIB_SECTORS)
#define CHUNK        (64u << 10)
#define BIG_CHUNKS   6u            /* /big.bin: written, left open and unsynced */
#define GATHER_CHUNKS 20u          /* /g.bin: written FS_GATHER */
#define MAX_DEATHS   8u
#define SUP_POLL     (50 * NS_PER_MS)

/* One supervised fat: what devmgr keeps for a mount. */
struct sup {
    struct fatrun   r;              /* r.fs: our client end; r.rd: the disk */
    handle_t        serve;          /* the `fs` server end, ours; each instance gets a dup */
    handle_t        state;          /* the state VMO, every right */
    struct keeper   keeper;
    const char     *dies[MAX_DEATHS];   /* instance i's test power, or NULL */
    const char     *crash;          /* every instance's crash-on word, or NULL */
    unsigned        instance;       /* instances started */
    unsigned        tests, crashes; /* ended by a test power (exit FAT_EXIT_TEST), crashed */
    bool            bad;            /* something the supervisor saw was wrong (said) */
    bool            stop;           /* the test is done with deaths (atomic) */
    handle_t        thread;
    _Alignas(16) uint8_t stack[32 << 10];
};

static struct sup sv;
static struct ramdisk ref_disk, test_disk;
static uint8_t buf[CHUNK];

/* Start instance sv.instance of fat. ended: how the last one ended. */
static status_t start(struct sup *s, const char *ended)
{
    handle_t block, serve, state, keep;
    if (!ramdisk_serve(s->r.rd, false, &block))
        return ERR_INTERNAL;
    status_t st = jam_handle_duplicate(s->serve, RIGHT_SAME, &serve);
    if (st == OK)
        st = svcstate_give(s->state, &state);
    if (st == OK)
        st = keeper_attach(&s->keeper, &keep);   /* the dead one's puts taken first */
    if (st == OK)
        st = new_job(&s->r.job);
    if (st != OK)
        return st;
    const char *argv[8] = { "fat", "utest", FAT_ARG_FORMAT, "fixed-time" };
    int argc = 4;
    if (ended)
        argv[argc++] = ended;
    if (s->instance < MAX_DEATHS && s->dies[s->instance])
        argv[argc++] = s->dies[s->instance];
    if (s->crash)
        argv[argc++] = s->crash;
    struct spawn_handle x[4] = { { FAT_SR_BLOCK, block }, { FAT_SR_SERVE, serve },
                                 { SR_STATE, state }, { SR_KEEP, keep } };
    rights_t xr[4] = { RIGHT_SAME, RIGHT_SAME, SVCSTATE_SERVICE_RIGHTS, RIGHT_SAME };
    struct spawn_args a = { .path = "bin/fat", .argc = argc, .argv = argv, .job = s->r.job,
                            .extra = x, .nextra = 4, .extra_rights = xr };
    st = spawn(&a, &s->r.proc);
    s->instance++;
    return st == OK ? keeper_restore(&s->keeper) : st;
}

/* The instance has ended: its job emptied, the disk's session over. */
static bool reap(struct sup *s, struct process_info *info)
{
    CHECK_ST(jam_process_get_info(s->r.proc, info), OK);
    jam_job_kill(s->r.job);
    CHECK_ST(jam_handle_close(s->r.proc), OK);
    CHECK_ST(jam_handle_close(s->r.job), OK);
    return ramdisk_join(s->r.rd);
}

/* The supervisor: every death restarted at once, "killed" after a test
 * power's end (as a deliberate kill at that instruction), "crashed" after
 * the kernel killed it. */
static void supervise(void *arg)
{
    struct sup *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        signals_t seen;
        if (jam_object_wait_one(s->r.proc, SIG_TERMINATED, now() + SUP_POLL, &seen) != OK)
            continue;
        struct process_info info;
        if (!reap(s, &info)) {
            s->bad = true;
            return;
        }
        bool test = !info.killed && info.exit_code == FAT_EXIT_TEST;
        if (!test && !info.killed) {
            printf("utest: fat ended with %ld, not by a test power or a crash\n",
                   (long)info.exit_code);
            s->bad = true;
            return;
        }
        s->tests += test;
        s->crashes += info.killed;
        status_t st = start(s, test ? FAT_ARG_KILLED : FAT_ARG_CRASHED);
        if (st != OK) {
            printf("utest: fat couldn't be started again (%s)\n", status_str(st));
            s->bad = true;
            return;
        }
    }
}

/* A supervised fat over rd, its instances ended by dies[] in turn. */
static bool sup_start(struct ramdisk *rd, const char *const *dies, unsigned n, const char *crash)
{
    memset(&sv, 0, offsetof(struct sup, stack));
    sv.r.rd = rd;
    for (unsigned i = 0; i < n && i < MAX_DEATHS; i++)
        sv.dies[i] = dies[i];
    sv.crash = crash;
    keeper_init(&sv.keeper);
    CHECK_ST(jam_channel_create(&sv.r.fs, &sv.serve), OK);
    CHECK_ST(svcstate_create(FAT_STATE_SIZE, &sv.state), OK);
    CHECK_ST(start(&sv, NULL), OK);
    CHECK_ST(thread_spawn("fat supervisor", supervise, &sv, sv.stack, sizeof(sv.stack),
                          &sv.thread), OK);
    return true;
}

/* No more deaths: the supervisor goes, then fat is stopped as fat_stop
 * does (exit 0 once its client is gone); the keeper lets go. */
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

/* Every kind of request, unsynced writes and an FS_GATHER file among them;
 * every answer checked. */
static bool script(const struct fatrun *r)
{
    struct tfile big, g;
    CHECK_ST(t_mkdir(r, "/d"), OK);
    if (!put_file(r, "/d/a.txt", "alpha"))
        return false;
    CHECK_ST(t_open(r, "/big.bin", FS_READ | FS_WRITE | FS_CREATE, &big), OK);
    if (!write_chunks(&big, BIG_CHUNKS, 1))
        return false;
    CHECK_ST(t_rename(r, "/d/a.txt", "/d/b.txt"), OK);
    if (!put_file(r, "/gone.txt", "x"))
        return false;
    CHECK_ST(t_unlink(r, "/gone.txt"), OK);
    CHECK_ST(t_open(r, "/g.bin", FS_READ | FS_WRITE | FS_CREATE | FS_GATHER, &g), OK);
    if (!write_chunks(&g, GATHER_CHUNKS, 100) || !read_chunks(&big, BIG_CHUNKS, 1))
        return false;
    t_close(&g);
    t_close(&big);
    CHECK_ST(t_mkdir(r, "/d/e"), OK);
    if (!put_file(r, "/d/e/c.txt", "gamma"))
        return false;
    CHECK_ST(t_sync(r), OK);
    CHECK_ST(t_stat(r, "/gone.txt", NULL, NULL, NULL), ERR_NOT_FOUND);
    if (!file_is(r, "/d/b.txt", "alpha") || !file_is(r, "/d/e/c.txt", "gamma"))
        return false;
    CHECK_ST(t_open(r, "/g.bin", FS_READ, &g), OK);
    bool ok = read_chunks(&g, GATHER_CHUNKS, 100);
    t_close(&g);
    return ok;
}

/* The two disks, byte for byte. */
static bool same_disks(void)
{
    for (uint32_t s = 0; s < DISK_SECTORS; s++)
        if (memcmp(ref_disk.mem + (size_t)s * RAMDISK_SECTOR,
                   test_disk.mem + (size_t)s * RAMDISK_SECTOR, RAMDISK_SECTOR))
            FAIL("sector %u differs from the undisturbed run's", s);
    return true;
}

/* The script with fat ended by dies[] in turn: no error, and the disk as
 * the undisturbed run left it. */
static bool storm(const char *const *dies, unsigned n)
{
    memset(test_disk.mem, 0, (size_t)DISK_SECTORS * RAMDISK_SECTOR);
    if (!sup_start(&test_disk, dies, n, NULL) || !script(&sv.r))
        return false;
    unsigned ended = sv.tests;
    if (!sup_stop())
        return false;
    if (ended != n)
        FAIL("%u of %u test powers ended fat (%s ...): pick steps it reaches", ended, n,
             dies[0]);
    return same_disks();
}

bool t_fat_restart_steps(void)
{
    static const char *const steps1[] = {
        "die=held:1", "die=commit:1", "die=send:1", "die=reply:1", "die=answered:1",
    };
    static const char *const steps2[] = {
        "die=held:7", "die=commit:4", "die=send:6", "die=reply:5", "die=answered:3",
    };
    static const char *const steps3[] = {
        "die=answered:12", "die=held:25", "die=send:15", "die=commit:9", "die=reply:11",
    };
    if (!ramdisk_create(&ref_disk, DISK_SECTORS) || !ramdisk_create(&test_disk, DISK_SECTORS))
        return false;
    bool ok = sup_start(&ref_disk, NULL, 0, NULL) && script(&sv.r) && sup_stop() &&
              storm(steps1, 5) && storm(steps2, 5) && storm(steps3, 5);
    return ramdisk_destroy(&test_disk) && ramdisk_destroy(&ref_disk) && ok;
}

/* ---- replies that carried handles ------------------------------------------------------ */

/* fs.open's and fs.view's reply, the instance ended before it went
 * (reply:1) and after (answered:1): the client gets working handles once. */
bool t_fat_restart_handles(void)
{
    static const char *const dies[] = {
        "die=answered:1", "die=reply:1", "die=answered:1", "die=reply:1",
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
    CHECK_ST(fs_view_until(sv.r.fs, now() + FAT_CALL_NS, FS_VIEW_READ_ONLY, &view), OK);  /* 3 */
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

/* A request that crashes fat every time: in progress at two crashes, it
 * is answered ERR_IO and fat serves on. One ended twice by deliberate kills
 * (test powers, told "killed") is run to the end. */
bool t_fat_restart_bad_request(void)
{
    static const char *const kills[] = { "die=held:1", "die=held:1" };
    bool is_dir = false;
    if (!ramdisk_create(&test_disk, DISK_SECTORS) ||
        !sup_start(&test_disk, NULL, 0, "crash-on=bad"))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/d"), OK);
    CHECK_ST(t_stat(&sv.r, "/d/bad", NULL, NULL, NULL), ERR_IO);
    CHECK_EQ(sv.crashes, 2u);
    CHECK_ST(t_stat(&sv.r, "/d", NULL, &is_dir, NULL), OK);   /* still serving */
    CHECK(is_dir);
    if (!sup_stop())
        return false;
    memset(test_disk.mem, 0, (size_t)DISK_SECTORS * RAMDISK_SECTOR);
    if (!sup_start(&test_disk, kills, 2, NULL))
        return false;
    CHECK_ST(t_mkdir(&sv.r, "/k"), OK);   /* ended twice in its first held write */
    CHECK_EQ(sv.tests, 2u);
    CHECK_ST(t_stat(&sv.r, "/k", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    return sup_stop() && ramdisk_destroy(&test_disk);
}
