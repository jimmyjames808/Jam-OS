/* utest: logd (user/services/logd) as a process. Its /data is an `fs`
 * channel (LOGD_SR_FS): the fat service over a RAM disk (fattest.h), or the
 * test itself refusing everything. Its log is a channel the test writes
 * text into (LOGD_SR_LOG) in place of the kernel log, so a test knows every
 * byte the file must hold, and closing the channel ends logd, so it can
 * look at the finished file. One test runs logd on the kernel log itself,
 * when utest was given the root resource that takes.
 *
 * Covered: what was logged before logd started comes first, then what
 * follows; the file is the next free boot-NNNN.txt in /logs (made if
 * missing); syncs come at most once a second; with /data refusing or gone
 * logd keeps running and tries again rarely. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

/* logd's startup handles (user/services/logd/logd.h has the same numbers). */
#define LOGD_SR_FS  (SR_USER + 0)
#define LOGD_SR_LOG (SR_USER + 1)

#define DISK_SECTORS (16 * MIB_SECTORS)
#define LOG1         "/logs/boot-0001.txt"   /* the first log on a blank /data */
#define END_NS       (10 * NS_PER_S)   /* for logd to end once its log has */

static struct ramdisk disk;

/* A logd under test. */
struct logd {
    handle_t job, proc;   /* its job (alone in it) and process */
    handle_t feed;        /* where its log is written; 0: it reads the kernel log */
};

static bool have_logd(void)
{
    const struct bootfs_view *b;
    const void *data;
    uint64_t size;
    if (bootfs_default(&b) == OK && bootfs_lookup(b, "bin/logd", &data, &size) == OK &&
        bootfs_lookup(b, "bin/fat", &data, &size) == OK)
        return true;
    printf("utest: %s: no bin/logd or bin/fat: skipped\n", utest_cur);
    return false;
}

/* bin/logd with `fs` (duplicated) as its /data, and as its log a channel
 * (l->feed), or the kernel log read through `root` if that is given. */
static bool logd_start(struct logd *l, handle_t fs, handle_t root)
{
    handle_t dup, second;
    const char *argv[] = { "bin/logd" };
    *l = (struct logd){ 0 };
    CHECK_ST(jam_handle_duplicate(fs, RIGHT_SAME, &dup), OK);
    if (root)
        CHECK_ST(jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_READ, &second), OK);
    else
        CHECK_ST(jam_channel_create(&l->feed, &second), OK);
    CHECK_ST(new_job(&l->job), OK);
    struct spawn_handle x[] = { { LOGD_SR_FS, dup }, { root ? SR_RESOURCE : LOGD_SR_LOG, second } };
    struct spawn_args a = {
        .path = "bin/logd", .argc = 1, .argv = argv, .job = l->job, .extra = x, .nextra = 2,
    };
    CHECK_ST(spawn(&a, &l->proc), OK);   /* consumes the extras */
    return true;
}

/* Its log ends (the feed closes): logd exits 0 by itself and leaves its
 * job empty. */
static bool logd_end(struct logd *l)
{
    struct process_info info;
    struct job_info ji;
    CHECK_ST(jam_handle_close(l->feed), OK);
    CHECK_ST(spawn_wait(l->proc, END_NS, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(info_of(l->job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        CHECK_EQ(ji.used[k], 0);
    CHECK_ST(jam_handle_close(l->proc), OK);
    CHECK_ST(jam_handle_close(l->job), OK);
    return true;
}

static bool logd_alive(const struct logd *l)
{
    struct process_info info;
    CHECK_ST(spawn_wait(l->proc, 0, &info), ERR_TIMED_OUT);
    return true;
}

/* Write line number i of the log into l's feed, and add it to `all` (a
 * string of at most cap bytes: what the file must hold), if given. */
static bool say_line(const struct logd *l, unsigned i, char *all, size_t cap)
{
    char line[64];
    int len = snprintf(line, sizeof(line), "[%4u.000000] utest: log line %u\n", i, i);
    CHECK_ST(jam_channel_write(l->feed, line, (uint32_t)len, NULL, 0), OK);
    if (all) {
        size_t n = strlen(all);
        CHECK(n + (size_t)len < cap);
        memcpy(all + n, line, (size_t)len + 1);
    }
    return true;
}

/* A whole log, twice: each run takes the next number, starts with what was
 * logged before it ran, follows the rest, and syncs at most once a second. */
bool t_logd_writes_the_log(void)
{
    static char all[4000], second[128];
    struct fatrun fat;
    struct logd l;
    all[0] = second[0] = 0;
    if (!have_logd())
        return true;
    if (!ramdisk_create(&disk, DISK_SECTORS) || !fat_start(&fat, &disk, false))
        return false;
    /* three boots' logs are there already: this one is boot-0004 */
    CHECK_ST(t_mkdir(&fat, "/logs"), OK);
    if (!put_file(&fat, "/logs/boot-0001.txt", "1") ||
        !put_file(&fat, "/logs/boot-0002.txt", "2") || !put_file(&fat, "/logs/boot-0003.txt", "3"))
        return false;
    if (!logd_start(&l, fat.fs, HANDLE_INVALID))
        return false;
    /* lines 0..2 are queued before logd reads anything */
    for (unsigned i = 0; i < 3; i++)
        if (!say_line(&l, i, all, sizeof(all)))
            return false;
    jam_nanosleep(now() + 500 * NS_PER_MS);   /* the file is made; the first sync is done */
    /* then one every 100 ms for 2.5 s: a file.sync a second, so 2 or 3.
     * What the disk sees is fat's: up to 4 block.sync per file.sync (the
     * volume marked dirty, the file flushed, the volume marked clean). A
     * sync per line would be 25 file.syncs, 50 block.syncs or more. */
    uint32_t syncs = ramdisk_syncs(&disk);
    for (unsigned i = 3; i < 28; i++) {
        if (!say_line(&l, i, all, sizeof(all)))
            return false;
        jam_nanosleep(now() + 100 * NS_PER_MS);
    }
    syncs = ramdisk_syncs(&disk) - syncs;
    if (syncs < 2 || syncs > 12)
        FAIL("%u block syncs in 2.5 s of steady logging, want one file sync a second", syncs);
    if (!logd_end(&l) || !file_is(&fat, "/logs/boot-0004.txt", all))
        return false;
    CHECK_ST(t_stat(&fat, "/logs/boot-0005.txt", NULL, NULL, NULL), ERR_NOT_FOUND);

    /* the next boot: boot-0005, and boot-0004 stays as it is */
    if (!logd_start(&l, fat.fs, HANDLE_INVALID) || !say_line(&l, 100, second, sizeof(second)))
        return false;
    if (!logd_end(&l) || !file_is(&fat, "/logs/boot-0005.txt", second) ||
        !file_is(&fat, "/logs/boot-0004.txt", all))
        return false;
    return fat_stop(&fat) && ramdisk_destroy(&disk);
}

/* No /data: a filesystem that refuses every request. logd keeps running,
 * asks again after 1 s, then 2 s (not in a loop), and ends cleanly when
 * its log does. A blank /data is no problem: /logs is made. */
bool t_logd_without_data(void)
{
    static const struct fs_ops none;   /* no handlers: each request is ERR_NOT_SUPPORTED */
    static char all[128];
    struct fatrun fat;
    struct logd l;
    handle_t fs, server;
    all[0] = 0;
    if (!have_logd())
        return true;
    CHECK_ST(jam_channel_create(&fs, &server), OK);
    if (!logd_start(&l, fs, HANDLE_INVALID))
        return false;
    unsigned asked = 0;
    uint64_t until = now() + 3500 * NS_PER_MS;
    for (unsigned i = 0; now() < until; i++) {
        if (!say_line(&l, i, NULL, 0))
            return false;
        jam_nanosleep(now() + 50 * NS_PER_MS);
        while (fs_serve_one(server, &none, NULL) == OK)
            asked++;
    }
    /* one request per try (the first one fails): at 0 s, 1 s and 3 s */
    CHECK(asked >= 2 && asked <= 4);
    if (!logd_alive(&l) || !logd_end(&l))
        return false;
    CHECK_ST(jam_handle_close(server), OK);
    CHECK_ST(jam_handle_close(fs), OK);

    /* a blank /data (fat formats it): logd makes /logs and takes boot-0001 */
    if (!ramdisk_create(&disk, DISK_SECTORS) || !fat_start(&fat, &disk, false))
        return false;
    if (!logd_start(&l, fat.fs, HANDLE_INVALID) || !say_line(&l, 1, all, sizeof(all)))
        return false;
    if (!logd_end(&l) || !file_is(&fat, "/logs/boot-0001.txt", all))
        return false;
    return fat_stop(&fat) && ramdisk_destroy(&disk);
}

/* /data goes away under a running logd (the stick is pulled: its
 * filesystem service ends): logd stays up through the log that follows
 * and ends cleanly. What it had synced is on the disk. */
bool t_logd_data_goes_away(void)
{
    static char all[512];
    struct fatrun fat;
    struct logd l;
    all[0] = 0;
    if (!have_logd())
        return true;
    if (!ramdisk_create(&disk, DISK_SECTORS) || !fat_start(&fat, &disk, false))
        return false;
    if (!logd_start(&l, fat.fs, HANDLE_INVALID))
        return false;
    for (unsigned i = 0; i < 5; i++)
        if (!say_line(&l, i, all, sizeof(all)))
            return false;
    /* written, and synced a second after that at the latest */
    jam_nanosleep(now() + 1500 * NS_PER_MS);
    CHECK(ramdisk_syncs(&disk) >= 1);
    if (!ramdisk_unplug(&disk))
        return false;
    for (unsigned i = 5; i < 25; i++) {
        if (!say_line(&l, i, NULL, 0))
            return false;
        jam_nanosleep(now() + 100 * NS_PER_MS);
    }
    if (!logd_alive(&l) || !logd_end(&l))
        return false;
    /* fat ended when the disk went; its job is empty now that logd has let
     * go of the file's buffer */
    CHECK_ST(jam_handle_close(fat.fs), OK);
    if (!fat_wait(&fat, 0))
        return false;

    /* the stick again: the five lines are in the file */
    if (!fat_start(&fat, &disk, true) || !file_is(&fat, "/logs/boot-0001.txt", all))
        return false;
    return fat_stop(&fat) && ramdisk_destroy(&disk);
}

/* Is the text `what` in the n bytes at buf? */
static bool contains(const char *buf, uint32_t n, const char *what)
{
    size_t len = strlen(what);
    for (uint32_t i = 0; len <= n && i <= n - len; i++)
        if (memcmp(buf + i, what, len) == 0)
            return true;
    return false;
}

/* The end of `path` (its last bytes, at most cap) into buf, or with
 * `head` its first ones; *n: how many (0 while fat still has the file
 * open for its writer, which locks everyone else out). */
static bool file_end(const struct fatrun *r, const char *path, bool head, char *buf,
                     uint32_t cap, uint32_t *n)
{
    struct tfile f;
    *n = 0;
    status_t st = t_open(r, path, FS_READ, &f);
    if (st == ERR_BAD_STATE)
        return true;
    CHECK_ST(st, OK);
    st = t_read(&f, head || f.size < cap ? 0 : f.size - cap, buf, cap, n);
    t_close(&f);
    CHECK_ST(st, OK);
    return true;
}

/* The real thing: logd on the kernel log, as init starts it. The file
 * starts where the log does (or with logd's note of what the ring had
 * dropped by then) and a line logged now arrives in it. Needs the root
 * resource with RIGHT_READ as our SR_RESOURCE; a utest started without it
 * skips this. */
bool t_logd_kernel_log(void)
{
    static char got[2048];
    struct fatrun fat;
    struct logd l;
    struct process_info info;
    handle_t root = startup_handle(SR_RESOURCE);
    char marker[48];
    uint64_t size = 0;
    uint32_t n = 0;
    if (!have_logd())
        return true;
    if (!root) {
        printf("utest: %s: no root resource (SR_RESOURCE) to read the kernel log with: "
               "skipped\n", utest_cur);
        return true;
    }
    if (!ramdisk_create(&disk, DISK_SECTORS) || !fat_start(&fat, &disk, false))
        return false;
    if (!logd_start(&l, fat.fs, root))
        return false;
    snprintf(marker, sizeof(marker), "logd marker %lu", (unsigned long)now());
    printf("utest: %s: %s\n", utest_cur, marker);
    /* While logd writes the file nobody else can open it; its size shows,
     * as of logd's last sync. The first sync comes with the first piece of
     * the log, the next a second later, with everything logged so far. */
    for (uint64_t until = now() + 10 * NS_PER_S; size < 1000;) {
        CHECK(now() < until);
        jam_nanosleep(now() + 50 * NS_PER_MS);
        status_t st = t_stat(&fat, LOG1, &size, NULL, NULL);
        CHECK(st == OK || st == ERR_NOT_FOUND);
    }
    jam_nanosleep(now() + 2 * NS_PER_S);
    CHECK_ST(jam_job_kill(l.job), OK);   /* the kernel log never ends */
    CHECK_ST(spawn_wait(l.proc, END_NS, &info), OK);
    CHECK_ST(jam_handle_close(l.proc), OK);
    CHECK_ST(jam_handle_close(l.job), OK);
    /* the marker is among the last bytes of the file */
    for (uint64_t until = now() + 10 * NS_PER_S; !contains(got, n, marker);) {
        CHECK(now() < until);
        jam_nanosleep(now() + 50 * NS_PER_MS);
        if (!file_end(&fat, LOG1, false, got, sizeof(got), &n))
            return false;
    }
    /* and it starts with a line of the log: "[    0.000000] ...", or
     * "[logd: N bytes of the log were lost]" */
    if (!file_end(&fat, LOG1, true, got, sizeof(got), &n))
        return false;
    CHECK(n > 0 && got[0] == '[');
    return fat_stop(&fat) && ramdisk_destroy(&disk);
}
