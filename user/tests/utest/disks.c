/* utest: volumes, against the mock usb-storage of diskmock.c handed over
 * with volumes.test_disk on /svc/volumes, with the real fat service on its
 * partitions: the boot disk's two partitions become mounts, a disk that
 * isn't the boot disk gets read-only /usbN mounts for its FAT volumes and
 * is never formatted, a killed filesystem service comes back on the
 * channel volumes kept, and a disk that goes takes its mounts with it. A
 * test disk's mounts are /esp-test and /data-test; whatever the real stick
 * has mounted is left alone and ignored here.
 *
 * Each test first makes the mock's ESP what it needs (a fat of the test's
 * own formats it and writes boot/jamos.elf, or doesn't), then hands the
 * disk to volumes, and follows the mounts through a watch of its own
 * (struct view, disks.h). The calls on a mount's channel are fattest.h's. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fatsvc.h>
#include <idl/storage.h>
#include "disks.h"
#include "utest.h"

struct diskmock mock;

handle_t volumes(void)
{
    handle_t vm = svc_get(SVC_VOLUMES);
    if (!vm)
        printf("utest: %s: no volumes channel (not started by init or the shell's utest?): "
               "skipped\n", utest_cur);
    return vm;
}

/* ---- the view: a watch, as a table ------------------------------------------------- */

static int view_find(const struct view *v, const char *path, uint32_t len)
{
    for (unsigned i = 0; i < v->n; i++)
        if (strlen(v->m[i].path) == len && !memcmp(v->m[i].path, path, len))
            return (int)i;
    return -1;
}

static void view_drop(struct view *v, int i)
{
    jam_handle_close(v->m[i].fs);
    v->m[i] = v->m[--v->n];
}

static void ev_mounted(void *ctx, const char *path, uint32_t len, uint32_t gen, uint8_t flags,
                       handle_t fs)
{
    struct view *v = ctx;
    v->events++;
    v->settled = false;
    int i = view_find(v, path, len);
    if (i >= 0)
        view_drop(v, i);   /* mounted again without unmounted: kept as the new one */
    if (v->n == VIEW_MAX || len >= sizeof(v->m[0].path)) {
        jam_handle_close(fs);
        return;
    }
    memcpy(v->m[v->n].path, path, len);
    v->m[v->n].path[len] = '\0';
    v->m[v->n].flags = flags;
    v->m[v->n].gen = gen;
    v->m[v->n++].fs = fs;
}

static void ev_unmounted(void *ctx, const char *path, uint32_t len, uint32_t gen)
{
    struct view *v = ctx;
    (void)gen;
    v->events++;
    v->settled = false;
    int i = view_find(v, path, len);
    if (i >= 0)
        view_drop(v, i);
}

static void ev_settled(void *ctx, uint32_t gen)
{
    struct view *v = ctx;
    v->events++;
    v->generation = gen;
    v->settled = true;
}

static const struct mounts_events view_events = {
    .mounted = ev_mounted, .unmounted = ev_unmounted, .settled = ev_settled,
};

status_t view_next(handle_t vm, struct view *v, uint64_t deadline)
{
    if (!v->watch) {
        status_t st = volumes_watch_until(vm, now() + 10 * NS_PER_S, &v->watch);
        if (st != OK) {
            v->watch = HANDLE_INVALID;
            return st;
        }
    }
    v->settled = false;
    while (!v->settled) {
        status_t st = mounts_read_event(v->watch, &view_events, v);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen = 0;
            st = jam_object_wait_one(v->watch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        }
        if (st != OK)
            return st;
    }
    return OK;
}

void view_close(struct view *v)
{
    while (v->n)
        view_drop(v, (int)v->n - 1);
    if (v->watch)
        jam_handle_close(v->watch);
    *v = (struct view){ 0 };
}

struct fatrun mount_of(const struct view *v, const char *path)
{
    struct fatrun r = { 0 };
    int i = view_find(v, path, (uint32_t)strlen(path));
    if (i >= 0)
        r.fs = v->m[i].fs;
    return r;
}

bool mounted(const struct view *v, const char *path)
{
    return mount_of(v, path).fs != HANDLE_INVALID;
}

uint32_t mount_gen(const struct view *v, const char *path)
{
    int i = view_find(v, path, (uint32_t)strlen(path));
    return i >= 0 ? v->m[i].gen : 0;
}

bool wait_mounts(handle_t vm, struct view *v, bool want_esp, bool want_data)
{
    uint64_t until = now() + WAIT_MOUNT;
    while (!v->generation || mounted(v, ESP) != want_esp || mounted(v, DATA) != want_data) {
        status_t st = view_next(vm, v, until);
        if (st == ERR_TIMED_OUT)
            FAIL("mounts after 15 s: %s %s, want %s %s", mounted(v, ESP) ? ESP : "-",
                 mounted(v, DATA) ? DATA : "-", want_esp ? ESP : "-", want_data ? DATA : "-");
        CHECK_ST(st, OK);
        CHECK(v->generation != 0);
    }
    return true;
}

const char *usb_path(const struct view *v)
{
    for (unsigned i = 0; i < v->n; i++) {
        const char *p = v->m[i].path;
        size_t n = strlen(p);
        if (n > 9 && !strncmp(p, "/usb", 4) && !strcmp(p + n - 5, "-test"))
            return p;
    }
    return NULL;
}

bool wait_usb(handle_t vm, struct view *v, int want)
{
    uint64_t until = now() + WAIT_MOUNT;
    for (;;) {
        const char *p = v->generation ? usb_path(v) : NULL;
        uint8_t ro = 0;
        if (v->generation && want < 0 && !p)
            return true;
        if (p && want >= 0 &&
            fs_statfs_until(mount_of(v, p).fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL) == OK &&
            ro == want)
            return true;
        status_t st = view_next(vm, v, until);
        if (st == ERR_TIMED_OUT)
            FAIL("/usbN-test after 15 s: %s, want %s", p ? p : "none",
                 want < 0 ? "none" : want ? "read-only" : "read-write");
        CHECK_ST(st, OK);
    }
}

bool view_quiet(const struct view *v)
{
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(v->watch, SIG_READABLE, now() + NS_PER_S, &seen), ERR_TIMED_OUT);
    return true;
}

status_t remount(handle_t vm, const char *path, bool writable)
{
    return volumes_remount_until(vm, now() + 30 * NS_PER_S, path, (uint32_t)strlen(path),
                                 writable);
}

/* ---- the mock disk ------------------------------------------------------------------- */

/* Format the mock's ESP and put a file on it: a fat of our own, on a
 * writable `block` channel, run to its end. */
static bool make_esp(handle_t storage, enum esp_kind kind)
{
    struct fatrun r = { 0 };
    handle_t blk, serve;
    if (kind == ESP_BLANK)
        return true;
    CHECK_ST(storage_open_partition_until(storage, now() + 5 * NS_PER_S, PART_ESP, 0, &blk), OK);
    CHECK_ST(jam_channel_create(&r.fs, &serve), OK);
    CHECK_ST(new_job(&r.job), OK);
    const char *argv[] = { "fat", "utest-esp", FAT_ARG_FORMAT };
    struct spawn_handle x[2] = { { FAT_SR_BLOCK, blk }, { FAT_SR_SERVE, serve } };
    struct spawn_args a = {
        .path = "bin/fat", .argc = 3, .argv = argv, .job = r.job, .extra = x, .nextra = 2,
    };
    CHECK_ST(spawn(&a, &r.proc), OK);
    if (kind == ESP_BOOT) {
        CHECK_ST(t_mkdir(&r, "/boot"), OK);
        if (!put_file(&r, "/boot/jamos.elf", "not a kernel: the name is what counts"))
            return false;
    } else if (!put_file(&r, "/readme.txt", "someone else's stick")) {
        return false;
    }
    CHECK_ST(jam_handle_close(r.fs), OK);
    return fat_wait(&r, 0);
}

bool attach(handle_t vm, uint8_t type2, enum esp_kind kind, uint32_t *id, bool *skip)
{
    const struct bootfs_view *b;
    const void *data;
    uint64_t size;
    handle_t client;
    *skip = bootfs_default(&b) != OK || bootfs_lookup(b, "bin/fat", &data, &size) != OK;
    if (*skip) {
        printf("utest: %s: no bin/fat: skipped\n", utest_cur);
        return false;
    }
    if (!dm_start(&mock, DM_TYPE_ESP, type2, &client) || !make_esp(client, kind))
        return false;
    /* client goes with the request either way */
    CHECK_ST(volumes_test_disk_until(vm, now() + 10 * NS_PER_S, client, id), OK);
    return true;
}

bool detach(handle_t vm, struct view *v)
{
    if (!dm_stop(&mock))
        return false;
    if (!wait_mounts(vm, v, false, false))
        return false;
    view_close(v);
    return true;
}

bool wait_count(const uint32_t *counter, uint32_t want)
{
    uint64_t until = now() + 15 * NS_PER_S;
    while (dm_count(counter) < want) {
        CHECK(now() < until);
        jam_sleep_until(now() + 10 * NS_PER_MS);
    }
    return true;
}

/* ---- the tests ----------------------------------------------------------------------- */

/* The boot disk: both partitions are mounted, the ESP read-only and the
 * blank data partition formatted, and the channels are real filesystems.
 * They come from a watch and from nowhere else: devmgr's query channel
 * can't reach volumes at all. */
bool t_disk_mounts(void)
{
    handle_t vm = volumes(), w;
    struct view v = { 0 };
    uint32_t id = 0;
    uint64_t size = 0;
    uint8_t ro = 0;
    bool skip, is_dir = true;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    /* volumes asked once and opened each partition once (the ESP a second
     * time, after our own fat), the ESP read-only: nothing tried to write it */
    struct fatrun esp = mount_of(&v, ESP), data = mount_of(&v, DATA);
    CHECK_EQ(dm_count(&mock.infos), 1);
    CHECK_EQ(dm_count(&mock.opened[0]), 2);
    CHECK_EQ(dm_count(&mock.opened[1]), 1);
    CHECK_ST(t_stat(&esp, "/boot/jamos.elf", &size, &is_dir, NULL), OK);
    CHECK(!is_dir && size > 0);
    CHECK_ST(fs_statfs_until(esp.fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL), OK);
    CHECK(ro);
    CHECK_ST(t_mkdir(&esp, "/x"), ERR_ACCESS_DENIED);
    CHECK_EQ(dm_count(&mock.refused), 0);
    CHECK_ST(fs_statfs_until(data.fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL), OK);
    CHECK(!ro);
    CHECK_ST(t_mkdir(&data, "/logs"), OK);
    if (!put_file(&data, "/logs/boot-0001.txt", "hello") ||
        !file_is(&data, "/logs/boot-0001.txt", "hello"))
        return false;
    /* the flags: the ESP read-only, both a test disk's */
    for (unsigned i = 0; i < v.n; i++)
        if (!strcmp(v.m[i].path, ESP) || !strcmp(v.m[i].path, DATA))
            CHECK_EQ(v.m[i].flags, MOUNTS_TEST | (!strcmp(v.m[i].path, ESP) ? MOUNTS_READ_ONLY : 0));
    /* nothing changed: nothing more is said */
    if (!view_quiet(&v))
        return false;
    /* devmgr's query channel is no way to volumes */
    if (svc_get(SVC_DEVMGR)) {
        CHECK(volumes_watch_until(svc_get(SVC_DEVMGR), now() + 10 * NS_PER_S, &w) != OK);
    }
    return detach(vm, &v);
}

/* Is the mock's data partition still blank (it starts as zeros)? */
static bool data_blank(void)
{
    const uint8_t *p = mock.ram + (uint64_t)DM_DATA_START * DM_BLOCK;
    for (uint32_t i = 0; i < DM_DATA_BLOCKS * DM_BLOCK; i++)
        if (p[i])
            return false;
    return true;
}

/* A disk that isn't the boot disk and holds no FAT volume at all (both
 * partitions blank, typed EF and 0C: what a Jam OS stick looks like before
 * anything is on it) gets no mounts and not one write: volumes looks at the
 * ESP (no boot disk), then tries each partition read-only, and every
 * filesystem service gives up by itself. Nothing is formatted, the blank
 * FAT32-typed partition included. */
static bool nothing_to_mount(handle_t vm, bool *skip)
{
    struct view v = { 0 };
    uint32_t id = 0;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BLANK, &id, skip))
        return false;
    if (!wait_count(&mock.closed[0], 2) || !wait_count(&mock.closed[1], 1))
        return false;
    jam_sleep_until(now() + 300 * NS_PER_MS);   /* time to do what it must not */
    CHECK_EQ(dm_count(&mock.opened[0]), 2);   /* the boot-disk check, then as any stick's */
    CHECK_EQ(dm_count(&mock.opened[1]), 1);
    CHECK_EQ(dm_count(&mock.opened_rw), 0);
    CHECK_EQ(dm_count(&mock.writes), 0);
    CHECK_EQ(dm_count(&mock.refused), 0);     /* nothing even tried */
    CHECK(data_blank());
    CHECK_ST(view_next(vm, &v, now() + 10 * NS_PER_S), OK);   /* a new watch: the list */
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA) && !usb_path(&v));
    return detach(vm, &v);
}

/* A disk that isn't the boot disk is never /esp or /data: an ESP without
 * boot/jamos.elf, an ESP that is no FAT volume (the filesystem service
 * gives up on it, and volumes does not start it again), and a second
 * partition of another type (volumes doesn't even ask the ESP for the
 * kernel). What it holds that is FAT is a read-only /usbN instead
 * (t_disk_other has the details). */
bool t_disk_not_boot(void)
{
    handle_t vm = volumes();
    struct view v = { 0 };
    uint32_t id = 0;
    uint8_t ro = 0;
    bool skip = false;
    if (!vm)
        return true;
    if (!nothing_to_mount(vm, &skip))
        return skip;
    if (!attach(vm, 0x83, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_usb(vm, &v, 1))
        return false;
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA));
    CHECK_EQ(dm_count(&mock.opened[0]), 2);   /* make_esp's, and the /usbN service's */
    CHECK_EQ(dm_count(&mock.opened[1]), 0);   /* type 83: not FAT, not looked at */
    CHECK_EQ(dm_count(&mock.opened_rw), 1);   /* make_esp's */
    struct fatrun usb = mount_of(&v, usb_path(&v));
    CHECK_ST(fs_statfs_until(usb.fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL), OK);
    CHECK(ro);
    if (!detach(vm, &v))
        return false;
    return wait_usb(vm, &v, -1);
}

/* Someone else's stick: an ESP-typed FAT volume without boot/jamos.elf and
 * a blank FAT32-typed partition. The FAT volume is mounted read-only at
 * /usbN-test on a `block` channel opened read-only (the disk itself would
 * refuse a write); volumes.remount reopens it read-write with a new
 * service and a new channel, and back. The blank partition is never
 * opened for writing, never mounted and never formatted, in either mode. */
bool t_disk_other(void)
{
    handle_t vm = volumes(), old;
    struct view v = { 0 };
    struct tfile f;
    uint32_t id = 0;
    bool skip;
    char usbn[16];
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_OTHER, &id, &skip))
        return skip;
    uint32_t writes = dm_count(&mock.writes);   /* make_esp's */
    if (!wait_usb(vm, &v, 1) || !wait_count(&mock.closed[1], 1))
        return false;
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA));
    snprintf(usbn, sizeof(usbn), "%s", usb_path(&v));
    struct fatrun usb = mount_of(&v, usbn);
    /* make_esp's channel, the boot-disk check's, the /usbN service's; the
     * blank partition's once, read-only, and its service gave up */
    CHECK_EQ(dm_count(&mock.opened[0]), 3);
    CHECK_EQ(dm_count(&mock.opened[1]), 1);
    CHECK_EQ(dm_count(&mock.opened_rw), 1);
    if (!file_is(&usb, "/readme.txt", "someone else's stick"))
        return false;
    CHECK_ST(t_mkdir(&usb, "/x"), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&usb, "/new.txt", FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
    CHECK_EQ(dm_count(&mock.writes), writes);
    CHECK_EQ(dm_count(&mock.refused), 0);   /* fat refused: the disk never had to */

    /* only /usbN, only writable 0 or 1, only on volumes' channel */
    CHECK_ST(remount(vm, "/usb9-test", true), ERR_NOT_FOUND);
    CHECK_ST(remount(vm, "/data", true), ERR_NOT_FOUND);
    CHECK_ST(volumes_remount_until(vm, now() + 10 * NS_PER_S, usbn, (uint32_t)strlen(usbn), 2),
             ERR_INVALID_ARGS);
    if (svc_get(SVC_DEVMGR))
        CHECK(remount(svc_get(SVC_DEVMGR), usbn, true) != OK);

    /* read-write: a new service on a new channel, the old one dead */
    CHECK_ST(jam_handle_duplicate(usb.fs, RIGHT_SAME, &old), OK);
    CHECK_ST(remount(vm, usbn, true), OK);
    if (!wait_usb(vm, &v, 0))
        return false;
    struct fatrun before = { .fs = old };
    CHECK_ST(t_stat(&before, "/readme.txt", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(old), OK);
    usb = mount_of(&v, usbn);
    CHECK_EQ(dm_count(&mock.opened_rw), 2);
    CHECK_ST(remount(vm, usbn, true), OK);   /* already: nothing restarts */
    CHECK_EQ(dm_count(&mock.opened[0]), 4);
    if (!put_file(&usb, "/new.txt", "written after mount -w") ||
        !file_is(&usb, "/new.txt", "written after mount -w"))
        return false;
    CHECK(dm_count(&mock.writes) > writes);

    /* and read-only again: what was written is there, nothing more goes in */
    CHECK_ST(remount(vm, usbn, false), OK);
    if (!wait_usb(vm, &v, 1))
        return false;
    usb = mount_of(&v, usbn);
    writes = dm_count(&mock.writes);
    if (!file_is(&usb, "/new.txt", "written after mount -w"))
        return false;
    CHECK_ST(t_mkdir(&usb, "/y"), ERR_ACCESS_DENIED);
    CHECK_EQ(dm_count(&mock.writes), writes);
    CHECK_EQ(dm_count(&mock.opened_rw), 2);
    CHECK_EQ(dm_count(&mock.refused), 0);

    /* the blank partition: opened once, read-only, and still blank */
    CHECK_EQ(dm_count(&mock.opened[1]), 1);
    CHECK(data_blank());
    if (!detach(vm, &v))
        return false;
    return wait_usb(vm, &v, -1);
}

/* Killing a filesystem service (a deliberate kill: restarted at once, not
 * counted): volumes kept its `fs` channel, so the mount never went and the
 * channel clients had still works, served by the new instance (on a new
 * `block` channel), which carries on from the dead one's state: a file it
 * had open stays open, what was written to it and not synced is there,
 * and the watch hears nothing (namespaces need nothing new); what was on
 * the volume is still there; the other mount never moved. */
bool t_disk_fs_restart(void)
{
    handle_t vm = volumes(), old;
    struct view v = { 0 };
    uint32_t id = 0, state = 0, restarts = 0, backoff = 0, promoted = 0;
    uint8_t spare = 0;
    uint64_t koid = 0;
    bool skip, is_dir = false;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    struct fatrun data = mount_of(&v, DATA), before = { 0 };
    CHECK_ST(jam_handle_duplicate(data.fs, RIGHT_SAME, &old), OK);
    before.fs = old;
    CHECK_ST(t_mkdir(&data, "/kept"), OK);
    CHECK_ST(t_sync(&data), OK);
    struct tfile f;
    uint32_t done = 0;
    char got[8] = { 0 };
    CHECK_ST(t_open(&data, "/unsynced.txt", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(t_write(&f, 0, "kept", 4, &done), OK);   /* no sync: fat holds it */
    CHECK_ST(volumes_kill_until(vm, now() + 30 * NS_PER_S, DATA, sizeof(DATA) - 1, &koid), OK);
    CHECK(koid != 0);
    /* the channel we had: a call on it waits for the next instance */
    CHECK_ST(t_stat(&before, "/kept", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    /* the file is still open, its unsynced write there */
    CHECK_ST(t_read(&f, 0, got, 4, &done), OK);
    CHECK_EQ(done, 4);
    CHECK(!memcmp(got, "kept", 4));
    t_close(&f);
    if (!view_quiet(&v))
        return false;
    CHECK(mounted(&v, ESP) && mounted(&v, DATA));
    CHECK(file_is(&data, "/unsynced.txt", "kept"));
    /* a second `block` channel on the data partition; the first was closed
     * by the death; the ESP's was never touched */
    CHECK_EQ(dm_count(&mock.opened[1]), 2);
    CHECK_EQ(dm_count(&mock.closed[1]), 1);
    CHECK_EQ(dm_count(&mock.opened[0]), 2);
    CHECK_ST(volumes_supervision_until(vm, now() + 10 * NS_PER_S, DATA, sizeof(DATA) - 1, &state,
                                       &restarts, &backoff, &promoted, &spare), OK);
    CHECK_EQ(state, VOLUMES_SUP_RUNNING);
    CHECK_EQ(restarts, 1);   /* one restart */
    CHECK_EQ(backoff, 0);    /* at once */
    CHECK(promoted <= 1);    /* from the warm spare, if one waited */
    /* a mount that isn't there */
    CHECK_ST(volumes_supervision_until(vm, now() + 10 * NS_PER_S, "/nowhere", 8, &state,
                                       &restarts, &backoff, &promoted, &spare), ERR_NOT_FOUND);
    CHECK_ST(volumes_kill_until(vm, now() + 10 * NS_PER_S, "/nowhere", 8, &koid), ERR_NOT_FOUND);
    CHECK_ST(jam_handle_close(old), OK);
    return detach(vm, &v);
}

/* volumes asks its filesystem services its own things (fs.sync, a
 * restart's first-answer probe) on a channel of its own, never handed out.
 * Two threads flood the mount's shared `fs` channel with stats and read
 * whatever comes back on it, as a program reading its end may: volumes'
 * syncs are still answered at once, before and after the service is
 * killed and restarted on the channels it kept. Before, a sync's answer on
 * the shared channel was taken by such a reader (or queued behind its
 * flood): volumes waited its 5 s and called the sync failed. */
struct flood {
    handle_t ch;        /* a duplicate of the mount's shared channel */
    uint32_t txid;      /* the next stat's */
    uint32_t sent, got; /* stats sent, messages read (anyone's) */
    uint8_t  buf[FS_REP_MAX] __attribute__((aligned(8)));
};
static volatile bool flood_stop;

static void flood_main(void *arg)
{
    struct flood *f = arg;
    while (!flood_stop) {
        for (unsigned k = 0; k < 4; k++)
            f->sent += fs_stat_send(f->ch, f->txid++, "/", 1) == OK;
        for (;;) {   /* a reader waiting on the end: what comes is handed to it */
            uint32_t n = 0, nh = 0;
            struct channel_reply_wait_args a = {
                .h = HANDLE_INVALID, .wait = f->ch, .rd_bytes = (uint64_t)(uintptr_t)f->buf,
                .rd_cap = sizeof(f->buf), .rd_actual = (uint64_t)(uintptr_t)&n,
                .rd_hactual = (uint64_t)(uintptr_t)&nh, .deadline_ns = now() + 2 * NS_PER_MS,
            };
            if (jam_channel_reply_wait(&a) != OK)
                break;
            f->got++;
        }
    }
}

bool t_disk_own_channel(void)
{
    static uint8_t stacks[2][16384] __attribute__((aligned(16)));
    static struct flood fl[2];
    handle_t vm = volumes(), th[2];
    struct view v = { 0 };
    uint32_t id = 0;
    uint64_t koid = 0;
    bool skip;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    struct fatrun data = mount_of(&v, DATA);
    flood_stop = false;
    for (unsigned i = 0; i < 2; i++) {
        fl[i] = (struct flood){ .txid = 0x100 | (uint32_t)i << 24 };
        CHECK_ST(jam_handle_duplicate(data.fs, RIGHT_SAME, &fl[i].ch), OK);
        CHECK_ST(thread_spawn("flood", flood_main, &fl[i], stacks[i], sizeof(stacks[i]), &th[i]),
                 OK);
    }
    jam_sleep_until(now() + 200 * NS_PER_MS);
    bool ok = true;
    for (unsigned k = 0; ok && k < 2; k++) {
        uint64_t t0 = now();
        status_t st = volumes_sync_until(vm, now() + 20 * NS_PER_S);
        uint64_t ms = (now() - t0) / NS_PER_MS;
        printf("utest: disk_own_channel: sync %u: %s in %lu ms, %u stats sent, %u messages read "
               "by the flood\n", k, status_str(st), (unsigned long)ms, fl[0].sent + fl[1].sent,
               fl[0].got + fl[1].got);
        ok = st == OK && ms < 2000;   /* not SYNC_WAIT's 5 s, its answer taken */
        if (ok && k == 0)   /* the next instance gets volumes' own channel too */
            ok = volumes_kill_until(vm, now() + 30 * NS_PER_S, DATA, sizeof(DATA) - 1,
                                    &koid) == OK;
    }
    flood_stop = true;
    if (!wait_threads(th, 2))
        return false;
    CHECK(ok);
    CHECK(fl[0].got + fl[1].got > 0);   /* the flood read its end */
    for (unsigned i = 0; i < 2; i++)
        CHECK_ST(jam_handle_close(fl[i].ch), OK);
    CHECK_ST(t_mkdir(&data, "/after"), OK);
    return detach(vm, &v);
}

/* The disk goes away: its mounts go with it, and the services' channels
 * are dead. A second disk then mounts from scratch. */
bool t_disk_vanishes(void)
{
    handle_t vm = volumes();
    struct view v = { 0 };
    struct fatrun esp = { 0 }, data = { 0 };
    uint32_t id = 0, id2 = 0;
    bool skip;
    if (!vm)
        return true;
    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(vm, &v, true, true))
        return false;
    CHECK_ST(jam_handle_duplicate(mount_of(&v, ESP).fs, RIGHT_SAME, &esp.fs), OK);
    CHECK_ST(jam_handle_duplicate(mount_of(&v, DATA).fs, RIGHT_SAME, &data.fs), OK);
    if (!detach(vm, &v))
        return false;
    CHECK_ST(t_stat(&esp, "/boot", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(t_stat(&data, "/", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(esp.fs), OK);
    CHECK_ST(jam_handle_close(data.fs), OK);

    if (!attach(vm, DM_TYPE_FAT32, ESP_BOOT, &id2, &skip))
        return skip;
    CHECK(id2 != id);
    if (!wait_mounts(vm, &v, true, true))
        return false;
    return detach(vm, &v);
}

/* volumes.list_channel: a channel that speaks voldisks (the disk listing,
 * digest and health) and nothing else, for the shell's `disks`. A control
 * channel speaks voldisks too. */
bool t_disk_list_channel(void)
{
    handle_t vm = volumes(), list = HANDLE_INVALID, other = HANDLE_INVALID, w = HANDLE_INVALID;
    if (!vm)
        return true;
    const uint64_t until = now() + 10 * NS_PER_S;
    CHECK_ST(volumes_list_channel_until(vm, until, &list), OK);
    for (unsigned k = 0; k < 2; k++) {
        handle_t ch = k ? vm : list;
        uint32_t id, bs, parts, nl, ml, sl, fl;
        uint64_t blocks;
        uint8_t bus, ready, kind, scheme, ro, sha[32];
        char name[9], model[41], serial[21], fw[9];
        /* the real disks, if the machine has any: listed, or past the last */
        status_t st = voldisks_disk_until(ch, until, 0, &id, &bus, &ready, &kind, &scheme, &ro,
                                          &bs, &blocks, &parts, name, &nl, model, &ml, serial,
                                          &sl, fw, &fl);
        CHECK(st == OK || st == ERR_OUT_OF_RANGE);
        CHECK_ST(voldisks_disk_until(ch, until, 1000, &id, &bus, &ready, &kind, &scheme, &ro,
                                     &bs, &blocks, &parts, name, &nl, model, &ml, serial, &sl,
                                     fw, &fl), ERR_OUT_OF_RANGE);
        CHECK_ST(voldisks_digest_until(ch, until, 0xdeadbeef, 0, 1, sha), ERR_NOT_FOUND);
    }
    /* it speaks nothing of `volumes`: it changes no mount and kills nothing */
    uint64_t koid;
    CHECK_ST(volumes_watch_until(list, until, &w), ERR_ACCESS_DENIED);
    CHECK_ST(volumes_kill_until(list, until, "/data", 5, &koid), ERR_ACCESS_DENIED);
    CHECK_ST(volumes_remount_until(list, until, "/usb0", 5, 1), ERR_ACCESS_DENIED);
    CHECK_ST(volumes_list_channel_until(list, until, &other), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(list), OK);
    return true;
}
