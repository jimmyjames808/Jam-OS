/* utest: fat carries on from a dead instance's state (user/services/fat/
 * adopt.c; docs/HISTORY.md, M11.6;
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
 * two deliberate kills of one request don't count. And the pages of a
 * mapped file (file.map, fat's pager.c) when fat dies making the VMO,
 * reading a page request or before a map's reply: the client gets its VMO,
 * every page right, and a key the state doesn't know detached. */
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
    handle_t        pager;          /* the mount's pager, ours (each instance a copy), or 0 */
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
    struct spawn_handle x[5] = { { FAT_SR_BLOCK, block }, { FAT_SR_SERVE, serve },
                                 { SR_STATE, state }, { SR_KEEP, keep } };
    rights_t xr[5] = { RIGHT_SAME, RIGHT_SAME, SVCSTATE_SERVICE_RIGHTS, RIGHT_SAME,
                       RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT };   /* as volumes hands it */
    unsigned nx = 4;
    if (s->pager && jam_handle_duplicate(s->pager, RIGHT_SAME, &x[nx].h) == OK)
        x[nx++].role = FAT_SR_PAGER;
    struct spawn_args a = { .path = "bin/fat", .argc = argc, .argv = argv, .job = s->r.job,
                            .extra = x, .nextra = nx, .extra_rights = xr };
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

/* A supervised fat over rd, its instances ended by dies[] in turn, each
 * handed a copy of pager (0: none). */
static bool sup_start_pager(struct ramdisk *rd, const char *const *dies, unsigned n,
                            const char *crash, handle_t pager)
{
    memset(&sv, 0, offsetof(struct sup, stack));
    sv.r.rd = rd;
    sv.pager = pager;
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

static bool sup_start(struct ramdisk *rd, const char *const *dies, unsigned n, const char *crash)
{
    return sup_start_pager(rd, dies, n, crash, HANDLE_INVALID);
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

/* ---- mapped files ---------------------------------------------------------------------- */

#define MAP_CHUNKS 4u                          /* a mapped file: four chunks ... */
#define MAP_TAIL   1000u                       /* ... and this many bytes of a fifth */
#define MAP_SIZE   (MAP_CHUNKS * CHUNK + MAP_TAIL)
#define MAP_SEED   7u

/* A pager for a supervised fat, or 0 with a line saying the test is
 * skipped (no RIGHT_ROOT_PAGER: utest run from the shell). */
static handle_t test_pager(void)
{
    handle_t root = startup_handle(SR_RESOURCE), p = HANDLE_INVALID;
    if (!root || jam_pager_create(root, 4096, FAT_CALL_NS, 0, &p) != OK) {
        printf("utest: fat_restart_pager: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return HANDLE_INVALID;
    }
    return p;
}

/* `path` made MAP_SIZE bytes long, MAP_SEED's chunks. */
static bool map_file(const char *path)
{
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(&sv.r, path, FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    if (!write_chunks(&f, MAP_CHUNKS, MAP_SEED))
        return false;
    fill(MAP_SEED + MAP_CHUNKS);
    CHECK_ST(t_write(&f, (uint64_t)MAP_CHUNKS * CHUNK, buf, MAP_TAIL, &done), OK);
    t_close(&f);
    return true;
}

/* The VMO's pages, read with vmo_read (which waits for them as a fault
 * does): the file's bytes, then zeros to the end of its last page. */
static bool vmo_is_file(handle_t vmo)
{
    static uint8_t got[CHUNK];
    for (uint32_t i = 0; i <= MAP_CHUNKS; i++) {
        uint32_t n = i < MAP_CHUNKS ? CHUNK : MAP_TAIL;
        fill(MAP_SEED + i);
        CHECK_ST(jam_vmo_read(vmo, (uint64_t)i * CHUNK, got, n), OK);
        if (memcmp(got, buf, n))
            FAIL("chunk %u of a mapped file reads wrong", i);
    }
    uint32_t tail = PAGE_SIZE - MAP_SIZE % PAGE_SIZE;
    CHECK_ST(jam_vmo_read(vmo, MAP_SIZE, got, tail), OK);
    for (uint32_t j = 0; j < tail; j++)
        if (got[j])
            FAIL("byte %u past the end of a mapped file is %u, not 0", MAP_SIZE + j, got[j]);
    return true;
}

static uint64_t koid_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.koid : 0;
}

/* Every mapping of `path` gone and the file closed: once fat has let go
 * (the keeper told, as volumes' loop would hear it) the file can be
 * removed, and the pager has nothing queued: every GONE answered (fat
 * serves what is queued before each fs request). */
static bool unmapped(handle_t pager, const char *path)
{
    CHECK_ST(t_sync(&sv.r), OK);   /* the file's close, and the keeper told to let go */
    keeper_drain(&sv.keeper);      /* no instance is left to die: the supervisor is idle */
    CHECK_ST(t_unlink(&sv.r, path), OK);
    CHECK_ST(jam_object_wait_one(pager, SIG_READABLE, 0, NULL), ERR_TIMED_OUT);
    return true;
}

/* fat ended making the VMO (map:1: the successor runs file.map again, and
 * the VMO of the undone run, a key it doesn't know, is detached when it
 * goes) and between reading a page request and supplying it (supply:1:
 * the successor takes the request again). Then: a second map shares the
 * first's VMO (the keeper gave fat its handle back), an EXEC map is a VMO
 * of its own, a file open and mapped to run can't be removed or written
 * until both go, and a file open for writing is mapped to read, not to
 * run. */
static bool map_deaths(handle_t pager)
{
    static const char *const dies[] = { "die=map:1", "die=supply:1" };
    struct tfile f, w;
    handle_t vmo = HANDLE_INVALID, again = HANDLE_INVALID, own = HANDLE_INVALID;
    uint64_t size = 0;
    if (!sup_start_pager(&test_disk, dies, 2, NULL, pager) || !map_file("/m.bin"))
        return false;
    CHECK_ST(t_open(&sv.r, "/m.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, now() + FAT_CALL_NS, FILE_MAP_READ, &vmo, &size), OK);
    CHECK_EQ(size, (uint64_t)MAP_SIZE);
    if (!vmo_is_file(vmo))
        return false;
    CHECK_EQ(sv.tests, 2u);
    CHECK_ST(file_map_until(f.ch, now() + FAT_CALL_NS, FILE_MAP_READ, &again, &size), OK);
    CHECK_EQ(koid_of(again), koid_of(vmo));
    CHECK_ST(file_map_until(f.ch, now() + FAT_CALL_NS, FILE_MAP_EXEC, &own, &size), OK);
    CHECK(koid_of(own) != koid_of(vmo));
    if (!vmo_is_file(own))
        return false;
    CHECK_ST(t_unlink(&sv.r, "/m.bin"), ERR_BUSY);
    CHECK_ST(t_open(&sv.r, "/m.bin", FS_READ | FS_WRITE, &w), ERR_BUSY);
    CHECK_ST(jam_handle_close(vmo), OK);
    CHECK_ST(jam_handle_close(again), OK);
    CHECK_ST(jam_handle_close(own), OK);
    t_close(&f);
    if (!unmapped(pager, "/m.bin"))
        return false;
    /* Open for writing: mapped to read (its writes reach the pages:
     * fat_coherent.c), never to run. */
    CHECK_ST(t_open(&sv.r, "/w.bin", FS_READ | FS_WRITE | FS_CREATE, &w), OK);
    CHECK_ST(file_map_until(w.ch, now() + FAT_CALL_NS, FILE_MAP_EXEC, &own, &size), ERR_BUSY);
    CHECK_ST(file_map_until(w.ch, now() + FAT_CALL_NS, FILE_MAP_READ, &vmo, &size), OK);
    CHECK_ST(jam_handle_close(vmo), OK);
    t_close(&w);
    return sup_stop();
}

/* file.map's reply lost with the process (reply:2 for the shared VMO, the
 * open being the first request; reply:1 for an EXEC map): the successor
 * sends the shared VMO again, and makes the EXEC one again under a new
 * key (the old one's GONE then finds a key nobody knows). */
static bool map_replies(handle_t pager)
{
    static const char *const dies[] = { "die=reply:2", "die=reply:1" };
    struct tfile f;
    handle_t vmo = HANDLE_INVALID, own = HANDLE_INVALID;
    uint64_t size = 0;
    if (!sup_start_pager(&test_disk, NULL, 0, NULL, pager) || !map_file("/r.bin") ||
        !sup_stop() || !sup_start_pager(&test_disk, dies, 2, NULL, pager))
        return false;
    CHECK_ST(t_open(&sv.r, "/r.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, now() + FAT_CALL_NS, FILE_MAP_READ, &vmo, &size), OK);
    CHECK_ST(file_map_until(f.ch, now() + FAT_CALL_NS, FILE_MAP_EXEC, &own, &size), OK);
    CHECK_EQ(sv.tests, 2u);
    CHECK_EQ(size, (uint64_t)MAP_SIZE);
    if (!vmo_is_file(vmo) || !vmo_is_file(own))
        return false;
    CHECK_ST(jam_handle_close(vmo), OK);
    CHECK_ST(jam_handle_close(own), OK);
    t_close(&f);
    return unmapped(pager, "/r.bin") && sup_stop();
}

bool t_fat_restart_pager(void)
{
    handle_t pager = test_pager();
    if (!pager)
        return true;
    bool ok = ramdisk_create(&test_disk, DISK_SECTORS) && map_deaths(pager) &&
              map_replies(pager);
    CHECK_ST(jam_handle_close(pager), OK);
    return ramdisk_destroy(&test_disk) && ok;
}
