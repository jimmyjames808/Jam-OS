/* utest: a pager that never answers a write-back (docs/WRITEMAP-PLAN.md's
 * WT1; the held half of tools/pager-test.sh's wpress, which a shell boot
 * can't do: a test power reaches only whoever starts extfs). Two extfs
 * over RAM disks, each with a pager of its own, mounted at /u7 and /u8:
 *   /u7's extfs has the hang=wback:1 power: the first write-back it begins
 *   stops it answering for good, its pages still writing; its pager's
 *   budget is 256 pages (1 MiB, 64 of them may be dirty) and answers
 *   within 5 s.
 *   /u8's is a plain extfs (a 16 MiB budget) holding a 4 MiB file of
 *   records, which `pagertest hash` reads six times while
 *   `pagertest rec put` stores 2 MiB of records, twice the budget, into a
 *   file on /u7 with no msync. The writer is throttled at the dirty limit
 *   and then killed when the pager's time is up (a page fault, "the file's
 *   pager didn't answer" in the kernel's line: the process info says a
 *   fault killed it); the reader, which has nothing to do with /u7, ends
 *   0 and its file is whole after (rec check), and /u8 and we answer.
 * /u8's disk is saved for e2fsck as the other extfs cases' are. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define ANSWER_NS (5 * NS_PER_S)
#define BUDGET    256u     /* /u7's pager, in pages */
#define W_RECS    512u     /* 4096-byte records: 2 MiB, twice the budget */
#define G_RECS    1024u    /* /u8's file: 4 MiB */

static struct ramdisk hd, gd;
static struct fatrun hr, gr;

/* pagertest with the words in argv (n of them, argv[0] included), both
 * mounts in its namespace: started, its handle in *proc. */
static bool pt_start(const char *const *argv, int n, handle_t *proc)
{
    static const char *const grants[] = { "/u7", "/u8", "/boot", NULL };
    struct spawn_args a = { .path = "bin/pagertest", .argc = n, .argv = argv,
                            .job = startup_handle(SR_JOB), .ns = grants };
    CHECK_ST(spawn(&a, proc), OK);
    return true;
}

/* ... and run to its end (within 60 s): it exits 0. */
static bool pt_run(const char *const *argv, int n)
{
    handle_t proc;
    struct process_info info = { 0 };
    CHECK(pt_start(argv, n, &proc));
    status_t st = spawn_wait(proc, 60 * NS_PER_S, &info);
    jam_handle_close(proc);
    CHECK_ST(st, OK);
    if (info.killed || info.exit_code)
        FAIL("pagertest %s %s ended with %ld%s", argv[1], argv[2], (long)info.exit_code,
             info.killed ? " (killed)" : "");
    return true;
}

/* Killed: its job gone, the disk's session ended. */
static bool kill_instance(struct fatrun *r)
{
    struct process_info info;
    jam_job_kill(r->job);
    CHECK_ST(spawn_wait(r->proc, FAT_CALL_NS, &info), OK);
    CHECK(info.killed);
    CHECK_ST(jam_handle_close(r->proc), OK);
    CHECK_ST(jam_handle_close(r->job), OK);
    CHECK_ST(jam_handle_close(r->fs), OK);
    CHECK_ST(jam_handle_close(r->ctl), OK);
    return ramdisk_join(r->rd);
}

static bool press(void)
{
    const char *make_w[] = { "pagertest", "rec", "make", "/u7/w.bin", "512", "4096" };
    const char *make_g[] = { "pagertest", "rec", "make", "/u8/g.bin", "1024", "4096" };
    const char *put_g[] = { "pagertest", "rec", "put", "/u8/g.bin", "1024", "7", "msync", "128",
                            "4096" };
    CHECK(pt_run(make_w, 6) && pt_run(make_g, 6) && pt_run(put_g, 9));
    /* The reader of /u8 and the writer of /u7 together. */
    const char *hash[] = { "pagertest", "hash", "/u8/g.bin", "6" };
    const char *put_w[] = { "pagertest", "rec", "put", "/u7/w.bin", "512", "5", "nosync", "1",
                            "4096" };
    handle_t reader, writer;
    uint64_t t0 = now();
    CHECK(pt_start(put_w, 9, &writer) && pt_start(hash, 4, &reader));
    struct process_info ri = { 0 }, wi = { 0 };
    status_t rs = spawn_wait(reader, 60 * NS_PER_S, &ri);
    uint64_t t_reader = now() - t0;
    status_t ws = spawn_wait(writer, 60 * NS_PER_S, &wi);
    uint64_t t_writer = now() - t0;
    jam_handle_close(reader);
    jam_handle_close(writer);
    printf("utest: extfs_wpress: the reader ended after %lu ms, the writer after %lu ms\n",
           (unsigned long)(t_reader / NS_PER_MS), (unsigned long)(t_writer / NS_PER_MS));
    CHECK_ST(rs, OK);
    CHECK_ST(ws, OK);
    if (ri.killed || ri.exit_code)
        FAIL("the reader of /u8 ended with %ld%s", (long)ri.exit_code, ri.killed ? " (killed)" : "");
    /* The writer: killed by a fault, not before the pager's time was up. */
    if (!wi.killed || !(wi.flags & PROCESS_FLAG_FAULT))
        FAIL("the writer to /u7 wasn't killed by a fault (exit %ld, killed %u)", (long)wi.exit_code,
             wi.killed);
    CHECK(t_writer >= ANSWER_NS - NS_PER_S);
    CHECK(t_writer < ANSWER_NS + 30 * NS_PER_S);
    /* /u8 is whole, and we can still use it. */
    const char *check_g[] = { "pagertest", "rec", "check", "/u8/g.bin", "1024", "7", "1024", "4096" };
    CHECK(pt_run(check_g, 8));
    uint64_t size = 0;
    CHECK_ST(t_stat(&gr, "/g.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, G_RECS * 4096ull);
    return true;
}

bool t_extfs_wpress(void)
{
    static const char *const hang[] = { "hang=wback:1" };
    handle_t root = startup_handle(SR_RESOURCE), hp = HANDLE_INVALID, gp = HANDLE_INVALID;
    if (!root || jam_pager_create(root, BUDGET, ANSWER_NS, 0, &hp) != OK) {
        printf("utest: extfs_wpress: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    CHECK_ST(jam_pager_create(root, 4096, FAT_CALL_NS, 0, &gp), OK);
    CHECK(ramdisk_create(&hd, EXT_DISK_SECTORS) && ramdisk_create(&gd, EXT_DISK_SECTORS));
    CHECK(extfs_start_pager_words(&hr, &hd, hp, hang, 1));
    CHECK(extfs_start_pager(&gr, &gd, gp));
    handle_t held, good;
    CHECK_ST(jam_handle_duplicate(hr.fs, RIGHT_SAME, &held), OK);
    CHECK_ST(ns_mount("/u7", held), OK);
    CHECK_ST(jam_handle_duplicate(gr.fs, RIGHT_SAME, &good), OK);
    CHECK_ST(ns_mount("/u8", good), OK);
    bool ok = press();
    CHECK_ST(ns_unmount("/u8"), OK);
    CHECK_ST(ns_unmount("/u7"), OK);
    if (!ok)
        return false;
    CHECK(fat_stop(&gr));
    CHECK(extfs_save(&gd, "wpress"));
    CHECK(kill_instance(&hr));
    CHECK_ST(jam_handle_close(hp), OK);
    CHECK_ST(jam_handle_close(gp), OK);
    return ramdisk_destroy(&hd) && ramdisk_destroy(&gd);
}
