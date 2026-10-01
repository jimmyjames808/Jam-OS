/* utest: devmgr's storage side, against the mock usb-storage of
 * diskmock.c handed over with DEVMGR_TEST_DISK, with the real fat service
 * on its partitions: the boot disk's two partitions become mounts, a disk
 * that isn't the boot disk gets read-only /usbN mounts for its FAT volumes
 * and is never formatted, a killed filesystem service comes
 * back as a new generation with a new channel, and a disk that goes takes
 * its mounts with it. A test disk's mounts are /esp-test and /data-test;
 * whatever the real stick has mounted is left alone and ignored here.
 *
 * Each test first makes the mock's ESP what it needs (a fat of the test's
 * own formats it and writes boot/jamos.elf, or doesn't), then hands the
 * disk to devmgr. The calls on a mount's channel are fattest.h's. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <devmgr.h>
#include <fatsvc.h>
#include <idl/storage.h>
#include <os.h>
#include "diskmock.h"
#include "fattest.h"
#include "utest.h"

#define ESP  "/esp-test"
#define DATA "/data-test"

/* What a test puts on the mock's ESP. */
enum esp_kind {
    ESP_BOOT,    /* a FAT volume with boot/jamos.elf: the boot disk */
    ESP_OTHER,   /* a FAT volume without it: someone else's stick */
    ESP_BLANK,   /* no volume at all */
};

/* DEVMGR_MOUNTS as last answered: the generation and each mount's channel. */
struct view {
    struct devmgr_mounts_rep rep;                  /* the answer */
    handle_t                 hs[DEVMGR_MAX_MOUNTS];   /* its channels (ours to close) */
};

static struct diskmock mock;

static void view_close(struct view *v)
{
    for (uint32_t i = 0; i < v->rep.count; i++)
        jam_handle_close(v->hs[i]);
    v->rep.count = 0;
}

/* v's mount `path` as fattest.h's calls take it (fs 0: not mounted). */
static struct fatrun mount_of(const struct view *v, const char *path)
{
    struct fatrun r = { 0 };
    for (uint32_t i = 0; i < v->rep.count; i++)
        if (strcmp(v->rep.mounts[i].path, path) == 0)
            r.fs = v->hs[i];
    return r;
}

static bool mounted(const struct view *v, const char *path)
{
    return mount_of(v, path).fs != HANDLE_INVALID;
}

/* Follow DEVMGR_MOUNTS from v's generation until the test disk's mounts
 * are as wanted (each there or not), for 15 s at most. */
static bool wait_mounts(handle_t dm, struct view *v, bool want_esp, bool want_data)
{
    uint64_t until = now() + 15 * NS_PER_S;
    /* v is the list as it is unless devmgr says otherwise: generation 0
     * (nothing asked yet) is answered at once, any other when it changes */
    while (!v->rep.generation || mounted(v, ESP) != want_esp || mounted(v, DATA) != want_data) {
        struct view next;
        if (now() > until)
            FAIL("mounts after 15 s: %s %s, want %s %s", mounted(v, ESP) ? ESP : "-",
                 mounted(v, DATA) ? DATA : "-", want_esp ? ESP : "-", want_data ? DATA : "-");
        status_t st = devmgr_mounts(dm, v->rep.generation, &next.rep, next.hs);
        if (st == ERR_TIMED_OUT)
            continue;
        CHECK_ST(st, OK);
        CHECK(next.rep.generation != 0 && next.rep.generation != v->rep.generation);
        view_close(v);
        *v = next;
    }
    return true;
}

/* Format the mock's ESP and put a file on it: a fat of our own, on a
 * writable `block` channel, run to its end. */
static bool make_esp(handle_t storage, enum esp_kind kind)
{
    struct fatrun r = { 0 };
    handle_t blk, serve;
    if (kind == ESP_BLANK)
        return true;
    CHECK_ST(storage_open_partition_until(storage, now() + 5 * NS_PER_S, DEVMGR_PART_ESP, 0, &blk),
             OK);
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

/* Make a mock disk and hand it to devmgr; *id: its name for
 * DEVMGR_FS_SVC. False with *skip when bootfs has no fat. */
static bool attach(handle_t dm, uint8_t type2, enum esp_kind kind, uint32_t *id, bool *skip)
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
    struct devmgr_req q = { 0, DEVMGR_TEST_DISK, 0, 0, 0 };
    struct devmgr_rep r;
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = dm, .wn = sizeof(q), .wbytes = (uint64_t)(uintptr_t)&q,
        .wh = (uint64_t)(uintptr_t)&client, .whn = 1, .rcap = sizeof(r),
        .rbytes = (uint64_t)(uintptr_t)&r, .ractual = (uint64_t)(uintptr_t)&n,
        .deadline_ns = now() + 10 * NS_PER_S,
    };
    CHECK_ST(jam_channel_call(&a), OK);   /* client goes with the request either way */
    CHECK(n >= DEVMGR_REP_HDR);
    CHECK_ST(r.status, OK);
    *id = r.a;
    return true;
}

/* The mock is pulled, and devmgr has dropped its mounts. */
static bool detach(handle_t dm, struct view *v)
{
    if (!dm_stop(&mock))
        return false;
    if (!wait_mounts(dm, v, false, false))
        return false;
    view_close(v);
    return true;
}

static bool wait_count(const uint32_t *counter, uint32_t want)
{
    uint64_t until = now() + 15 * NS_PER_S;
    while (dm_count(counter) < want) {
        CHECK(now() < until);
        jam_nanosleep(now() + 10 * NS_PER_MS);
    }
    return true;
}

/* The boot disk: both partitions are mounted, the ESP read-only and the
 * blank data partition formatted, and the channels are real filesystems.
 * They come from the control channel's DEVMGR_MOUNTS and from nowhere
 * else. */
bool t_disk_mounts(void)
{
    handle_t dm = devmgr(), hs[DEVMGR_MAX_HANDLES];
    struct view v = { 0 };
    struct devmgr_rep r;
    uint32_t id = 0, nh = 0;
    uint64_t size = 0;
    uint8_t ro = 0;
    bool skip, is_dir = true;
    if (!dm)
        return true;
    if (!attach(dm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(dm, &v, true, true))
        return false;
    /* devmgr asked once and opened each partition once (the ESP a second
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

    /* the same generation again: no answer before DEVMGR_MOUNTS_WAIT */
    struct view same;
    uint64_t t0 = now();
    CHECK_ST(devmgr_mounts(dm, v.rep.generation, &same.rep, same.hs), ERR_TIMED_OUT);
    CHECK(now() - t0 >= DEVMGR_MOUNTS_WAIT - 100 * NS_PER_MS);
    /* a filesystem's channel is not GET_SERVICE's to give, and the query
     * channel gets no mounts at all */
    CHECK_ST(devmgr_call(dm, DEVMGR_GET_SERVICE, DEVMGR_FS_SVC, DEVMGR_PART_DATA, id, &r, hs,
                         DEVMGR_MAX_HANDLES, &nh, now() + 10 * NS_PER_S), ERR_ACCESS_DENIED);
    CHECK_EQ(nh, 0);
    if (svc_get(SVC_DEVMGR))
        CHECK_ST(devmgr_mounts(svc_get(SVC_DEVMGR), 0, &same.rep, same.hs),
                 ERR_ACCESS_DENIED);
    return detach(dm, &v);
}

/* A test disk's /usbN-test mount in v (the first), or NULL. */
static const char *usb_path(const struct view *v)
{
    for (uint32_t i = 0; i < v->rep.count; i++) {
        const char *p = v->rep.mounts[i].path;
        size_t n = strnlen(p, sizeof(v->rep.mounts[i].path));
        if (n > 9 && !strncmp(p, "/usb", 4) && !strcmp(p + n - 5, "-test"))
            return p;
    }
    return NULL;
}

/* Follow DEVMGR_MOUNTS until the test disk's /usbN-test is there and
 * answers statfs with read_only == want (0 or 1), or, want < 0, is gone;
 * for 15 s at most. */
static bool wait_usb(handle_t dm, struct view *v, int want)
{
    uint64_t until = now() + 15 * NS_PER_S;
    for (;;) {
        const char *p = v->rep.generation ? usb_path(v) : NULL;
        uint8_t ro = 0;
        if (v->rep.generation && want < 0 && !p)
            return true;
        if (p && want >= 0 &&
            fs_statfs_until(mount_of(v, p).fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL) == OK &&
            ro == want)
            return true;
        if (now() > until)
            FAIL("/usbN-test after 15 s: %s, want %s", p ? p : "none",
                 want < 0 ? "none" : want ? "read-only" : "read-write");
        struct view next;
        status_t st = devmgr_mounts(dm, v->rep.generation, &next.rep, next.hs);
        if (st == ERR_TIMED_OUT)
            continue;
        CHECK_ST(st, OK);
        view_close(v);
        *v = next;
    }
}

static status_t remount(handle_t dm, unsigned n, bool writable)
{
    struct devmgr_rep r;
    return devmgr_call(dm, DEVMGR_REMOUNT, DEVMGR_USB_MOUNT, (uint16_t)n,
                       DEVMGR_REMOUNT_TEST | (writable ? DEVMGR_REMOUNT_WRITE : 0), &r, NULL, 0,
                       NULL, now() + 30 * NS_PER_S);
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
 * anything is on it) gets no mounts and not one write: devmgr looks at the
 * ESP (no boot disk), then tries each partition read-only, and every
 * filesystem service gives up by itself. Nothing is formatted, the blank
 * FAT32-typed partition included. */
static bool nothing_to_mount(handle_t dm, bool *skip)
{
    struct view v = { 0 };
    uint32_t id = 0;
    if (!attach(dm, DM_TYPE_FAT32, ESP_BLANK, &id, skip))
        return false;
    if (!wait_count(&mock.closed[0], 2) || !wait_count(&mock.closed[1], 1))
        return false;
    jam_nanosleep(now() + 300 * NS_PER_MS);   /* time to do what it must not */
    CHECK_EQ(dm_count(&mock.opened[0]), 2);   /* the boot-disk check, then as any stick's */
    CHECK_EQ(dm_count(&mock.opened[1]), 1);
    CHECK_EQ(dm_count(&mock.opened_rw), 0);
    CHECK_EQ(dm_count(&mock.writes), 0);
    CHECK_EQ(dm_count(&mock.refused), 0);     /* nothing even tried */
    CHECK(data_blank());
    CHECK_ST(devmgr_mounts(dm, 0, &v.rep, v.hs), OK);
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA) && !usb_path(&v));
    return detach(dm, &v);
}

/* A disk that isn't the boot disk is never /esp or /data: an ESP without
 * boot/jamos.elf, an ESP that is no FAT volume (the filesystem service
 * gives up on it, and devmgr does not start it again), and a second
 * partition of another type (devmgr doesn't even ask the ESP for the
 * kernel). What it holds that is FAT is a read-only /usbN instead
 * (t_disk_other has the details). */
bool t_disk_not_boot(void)
{
    handle_t dm = devmgr();
    struct view v = { 0 };
    uint32_t id = 0;
    uint8_t ro = 0;
    bool skip = false;
    if (!dm)
        return true;
    if (!nothing_to_mount(dm, &skip))
        return skip;
    if (!attach(dm, 0x83, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_usb(dm, &v, 1))
        return false;
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA));
    CHECK_EQ(dm_count(&mock.opened[0]), 2);   /* make_esp's, and the /usbN service's */
    CHECK_EQ(dm_count(&mock.opened[1]), 0);   /* type 83: not FAT, not looked at */
    CHECK_EQ(dm_count(&mock.opened_rw), 1);   /* make_esp's */
    struct fatrun usb = mount_of(&v, usb_path(&v));
    CHECK_ST(fs_statfs_until(usb.fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL), OK);
    CHECK(ro);
    if (!detach(dm, &v))
        return false;
    return wait_usb(dm, &v, -1);
}

/* Someone else's stick: an ESP-typed FAT volume without boot/jamos.elf and
 * a blank FAT32-typed partition. The FAT volume is mounted read-only at
 * /usbN-test on a `block` channel opened read-only (the disk itself would
 * refuse a write); DEVMGR_REMOUNT reopens it read-write with a new
 * service and a new channel, and back. The blank partition is never
 * opened for writing, never mounted and never formatted, in either mode. */
bool t_disk_other(void)
{
    handle_t dm = devmgr(), old;
    struct view v = { 0 };
    struct devmgr_rep r;
    struct tfile f;
    uint32_t id = 0;
    bool skip;
    if (!dm)
        return true;
    if (!attach(dm, DM_TYPE_FAT32, ESP_OTHER, &id, &skip))
        return skip;
    uint32_t writes = dm_count(&mock.writes);   /* make_esp's */
    if (!wait_usb(dm, &v, 1) || !wait_count(&mock.closed[1], 1))
        return false;
    CHECK(!mounted(&v, ESP) && !mounted(&v, DATA));
    unsigned n = (unsigned)(usb_path(&v)[4] - '0');
    struct fatrun usb = mount_of(&v, usb_path(&v));
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

    /* only /usbN, only on the control channel */
    CHECK_ST(remount(dm, 9, true), ERR_NOT_FOUND);
    CHECK_ST(devmgr_call(dm, DEVMGR_REMOUNT, DEVMGR_FS_SVC, DEVMGR_PART_ESP, id, &r, NULL, 0, NULL,
                         now() + 10 * NS_PER_S), ERR_INVALID_ARGS);
    if (svc_get(SVC_DEVMGR))
        CHECK_ST(remount(svc_get(SVC_DEVMGR), n, true), ERR_ACCESS_DENIED);

    /* read-write: a new service on a new channel, the old one dead */
    CHECK_ST(jam_handle_duplicate(usb.fs, RIGHT_SAME, &old), OK);
    CHECK_ST(remount(dm, n, true), OK);
    if (!wait_usb(dm, &v, 0))
        return false;
    struct fatrun before = { .fs = old };
    CHECK_ST(t_stat(&before, "/readme.txt", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(old), OK);
    usb = mount_of(&v, usb_path(&v));
    CHECK_EQ(dm_count(&mock.opened_rw), 2);
    CHECK_ST(remount(dm, n, true), OK);   /* already: nothing restarts */
    CHECK_EQ(dm_count(&mock.opened[0]), 4);
    if (!put_file(&usb, "/new.txt", "written after mount -w") ||
        !file_is(&usb, "/new.txt", "written after mount -w"))
        return false;
    CHECK(dm_count(&mock.writes) > writes);

    /* and read-only again: what was written is there, nothing more goes in */
    CHECK_ST(remount(dm, n, false), OK);
    if (!wait_usb(dm, &v, 1))
        return false;
    usb = mount_of(&v, usb_path(&v));
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
    if (!detach(dm, &v))
        return false;
    return wait_usb(dm, &v, -1);
}

/* Killing a filesystem service: the mount goes, then comes back under a
 * new generation with a new channel on a new `block` channel; the old
 * channel is dead; what was on the volume is still there; the other mount
 * never moved. */
bool t_disk_fs_restart(void)
{
    handle_t dm = devmgr(), old;
    struct view v = { 0 };
    struct devmgr_rep r;
    uint32_t id = 0;
    bool skip, is_dir = false;
    if (!dm)
        return true;
    if (!attach(dm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(dm, &v, true, true))
        return false;
    uint32_t gen = v.rep.generation;
    struct fatrun data = mount_of(&v, DATA), before = { 0 };
    CHECK_ST(jam_handle_duplicate(data.fs, RIGHT_SAME, &old), OK);
    before.fs = old;
    CHECK_ST(t_mkdir(&data, "/kept"), OK);
    CHECK_ST(t_sync(&data), OK);
    CHECK_ST(devmgr_call(dm, DEVMGR_KILL, DEVMGR_FS_SVC, DEVMGR_PART_DATA, id, &r, NULL, 0, NULL,
                         now() + 30 * NS_PER_S), OK);
    if (!wait_mounts(dm, &v, true, false))   /* gone ... */
        return false;
    CHECK(v.rep.generation != gen);
    gen = v.rep.generation;
    if (!wait_mounts(dm, &v, true, true))    /* ... and back */
        return false;
    CHECK(v.rep.generation != gen);
    data = mount_of(&v, DATA);
    CHECK_ST(t_stat(&before, "/kept", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(t_stat(&data, "/kept", NULL, &is_dir, NULL), OK);
    CHECK(is_dir);
    /* a second `block` channel on the data partition; the first was closed
     * by the death; the ESP's was never touched */
    CHECK_EQ(dm_count(&mock.opened[1]), 2);
    CHECK_EQ(dm_count(&mock.closed[1]), 1);
    CHECK_EQ(dm_count(&mock.opened[0]), 2);
    CHECK_ST(devmgr_call(dm, DEVMGR_SUPERVISION, DEVMGR_FS_SVC, DEVMGR_PART_DATA, id, &r, NULL, 0,
                         NULL, now() + 10 * NS_PER_S), OK);
    CHECK_EQ(r.a, DEVMGR_SUP_RUNNING);
    CHECK_EQ(r.b, 1);   /* one restart */
    CHECK_ST(jam_handle_close(old), OK);
    return detach(dm, &v);
}

/* The disk goes away: its mounts go with it, and the services' channels
 * are dead. A second disk then mounts from scratch. */
bool t_disk_vanishes(void)
{
    handle_t dm = devmgr();
    struct view v = { 0 };
    struct fatrun esp = { 0 }, data = { 0 };
    uint32_t id = 0, id2 = 0;
    bool skip;
    if (!dm)
        return true;
    if (!attach(dm, DM_TYPE_FAT32, ESP_BOOT, &id, &skip))
        return skip;
    if (!wait_mounts(dm, &v, true, true))
        return false;
    CHECK_ST(jam_handle_duplicate(mount_of(&v, ESP).fs, RIGHT_SAME, &esp.fs), OK);
    CHECK_ST(jam_handle_duplicate(mount_of(&v, DATA).fs, RIGHT_SAME, &data.fs), OK);
    if (!detach(dm, &v))
        return false;
    CHECK_ST(t_stat(&esp, "/boot", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(t_stat(&data, "/", NULL, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_close(esp.fs), OK);
    CHECK_ST(jam_handle_close(data.fs), OK);

    if (!attach(dm, DM_TYPE_FAT32, ESP_BOOT, &id2, &skip))
        return skip;
    CHECK(id2 != id);
    if (!wait_mounts(dm, &v, true, true))
        return false;
    return detach(dm, &v);
}
