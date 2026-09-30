/* devmgr: disks, and the filesystem services of the disk Jam OS booted
 * from.
 *
 * A disk is a `storage` channel (abi/idl/storage.idl): the DR_SERVE
 * channel of a usb-storage driver devmgr bound to a mass-storage interface
 * (usb.c), or the channel a test handed over with DEVMGR_TEST_DISK. devmgr
 * is the only client of it: nothing else ever gets a disk's channel, and a
 * filesystem service gets one partition's `block` channel and no more.
 *
 * What happens to a disk (enum disk_state):
 *   1. its driver starts: storage.info is asked without waiting (a stick
 *      may take seconds to spin up, and devmgr must keep serving);
 *   2. the answer comes: fewer than two partitions, or types other than
 *      0xEF then 0x0C, and the disk is left alone. Else a read-only
 *      filesystem service is started on the ESP and asked, again without
 *      waiting, for boot/jamos.elf;
 *   3. the file is there: this is the boot disk. The data partition's
 *      service starts too, and both are mounts (DEVMGR_MOUNTS, mounts.c):
 *      /esp and /data. Without the file, or if the service ends instead
 *      of answering (the ESP holds no FAT it can read), the ESP's service
 *      is stopped and the disk left alone. One boot disk at a time: a
 *      second disk that qualifies while the first is mounted is left alone
 *      as well.
 * No answer within INFO_WAIT or ESP_WAIT also leaves the disk alone.
 *
 * A filesystem service (bin/fat, <fatsvc.h>; a BIND_FS binding in devs[])
 * is supervised like a driver (supervise.c): a crash or an error exit is
 * restarted with backoff, and given up on when it keeps ending (a data
 * partition that holds no FAT volume and isn't blank makes fat exit 1
 * every time). Each start opens a new `block` channel on its partition and
 * makes a new `fs` channel, so the mounts' generation moves on when it
 * dies and again when it is back.
 *
 * A disk whose driver is gone (the stick unplugged, usb-storage crashed,
 * a test disk's channel closed) loses its services at once: their `block`
 * channels are dead. They are killed, not asked to stop: nothing can be
 * written any more. A driver that is restarted starts again at step 1.
 *
 * Test disks live apart from real ones: their mounts are /esp-test and
 * /data-test, and "one boot disk at a time" counts them separately, so a
 * test never takes /data away. */
#include <fatsvc.h>
#include <fs_idl.h>
#include <idl/storage.h>
#include "internal.h"

#define MAX_DISKS      8
#define FAT_PATH       "bin/fat"
#define BOOT_FILE      "/boot/jamos.elf"   /* on the ESP: what makes it the boot disk */
#define TYPE_ESP       0xef                /* MBR partition types */
#define TYPE_FAT32_LBA 0x0c
#define INFO_WAIT      (30 * NS_PER_S)     /* storage.info: the stick may be spinning up */
#define ESP_WAIT       (10 * NS_PER_S)     /* the ESP's service: start, mount, one stat */
#define CALL_WAIT      (2 * NS_PER_S)      /* a call to a driver that has answered info */
#define SYNC_WAIT      (5 * NS_PER_S)      /* fs.sync before devmgr stops */
#define TEST_ID        0x80000000u         /* a test disk's id: this | a counter */

enum disk_state {
    DISK_FREE,    /* an unused slot */
    DISK_DOWN,    /* its driver isn't running (its restart is due) */
    DISK_INFO,    /* storage.info asked, no answer yet */
    DISK_ESP,     /* the ESP's service asked for BOOT_FILE, no answer yet */
    DISK_BOOT,    /* the boot disk: its running services are mounts */
    DISK_OTHER,   /* not the boot disk, or it didn't answer: left alone */
};

struct disk {
    enum disk_state state;      /* where it is in the steps above */
    bool            test;       /* from DEVMGR_TEST_DISK */
    uint32_t        id;         /* usb-bus's device id, or TEST_ID | n */
    uint32_t        bind;       /* devs index + 1 of its driver; 0: a test disk */
    handle_t        ch;         /* a test disk: our end of its `storage` channel */
    uint16_t        gen;        /* bumped at every use of the slot: in its port key */
    uint32_t        txid;       /* DISK_INFO, DISK_ESP: the request not answered yet */
    uint64_t        deadline;   /* DISK_INFO, DISK_ESP: when it is given up on */
    uint32_t        fs[2];      /* devs index + 1 of each PART_*'s service; 0: none */
};

static struct disk disks[MAX_DISKS];
/* Transaction ids of the requests written without waiting. The kernel
 * numbers channel_call's from 1 up, so these are billions of calls away
 * from any id a caller on the same channel could be waiting for. */
static uint32_t txids = 0xd15c0000u;
static uint32_t test_ids;   /* test disks made so far */

static handle_t disk_ch(const struct disk *d)
{
    return d->test ? d->ch : devs[d->bind - 1].client;
}

static const char *disk_name(const struct disk *d)
{
    static char s[24];
    snprintf(s, sizeof(s), d->test ? "test disk %u" : "disk %u", d->id & ~TEST_ID);
    return s;
}

static struct disk *disk_of(const struct binding *b)
{
    return b->disk ? &disks[b->disk - 1] : NULL;
}

static struct disk *disk_alloc(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        if (disks[i].state != DISK_FREE)
            continue;
        uint16_t gen = (uint16_t)(disks[i].gen + 1);
        disks[i] = (struct disk){ .state = DISK_DOWN, .gen = gen };
        return &disks[i];
    }
    return NULL;
}

/* The next message queued on h into buf (cap bytes): OK and *n, or
 * jam_channel_read's status once nothing is left. Nothing devmgr asked for
 * comes with handles or is bigger than cap: handles are closed, a message
 * that doesn't fit is dropped. */
static status_t next_msg(handle_t h, void *buf, uint32_t cap, uint32_t *n)
{
    for (int guard = 0; guard < 64; guard++) {
        handle_t hs[4];
        uint32_t nh = 0;
        struct channel_read_args a = {
            .h = h, .bytes_cap = cap, .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)n, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(h, *n, nh);
            continue;
        }
        if (st != OK)
            return st;
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        return OK;
    }
    return ERR_SHOULD_WAIT;
}

/* ---- filesystem services ------------------------------------------------------ */

const char *fs_mount_path(const struct binding *b)
{
    const struct disk *d = disk_of(b);
    bool test = d && d->test;
    if (b->part == PART_ESP)
        return test ? "/esp-test" : "/esp";
    return test ? "/data-test" : "/data";
}

void fs_retire(struct binding *b)
{
    struct disk *d = disk_of(b);
    if (d && d->fs[b->part] == (uint32_t)(b - devs) + 1)
        d->fs[b->part] = 0;
    sup_reset(b);
    close_client(b);
    b->state = DEVMGR_SUP_NONE;
    b->disk = 0;
    b->path = NULL;   /* a free slot now */
}

struct binding *fs_find(uint32_t id, uint32_t part)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state != DISK_FREE && disks[i].id == id && part < 2 && disks[i].fs[part])
            return &devs[disks[i].fs[part] - 1];
    return NULL;
}

status_t fs_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    const struct disk *d = disk_of(b);
    if (!d || d->state == DISK_FREE || d->state == DISK_DOWN)
        return ERR_PEER_CLOSED;
    handle_t blk;
    status_t st = storage_open_partition_until(disk_ch(d), now() + CALL_WAIT, b->part,
                                               b->part == PART_ESP, &blk);
    if (st != OK)
        return st;
    x[*n] = (struct spawn_handle){ FAT_SR_BLOCK, blk };
    xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;   /* not to be passed on */
    return OK;
}

/* A free BIND_FS binding (or a new one) for partition `part` of d. */
static struct binding *fs_binding(struct disk *d, unsigned part)
{
    struct binding *b = NULL;
    for (unsigned i = 0; i < ndevs && !b; i++)
        if (devs[i].kind == BIND_FS && !devs[i].path && !devs[i].proc &&
            devs[i].state == DEVMGR_SUP_NONE)
            b = &devs[i];
    if (!b && ndevs < MAX_DEVS)
        b = &devs[ndevs++];
    if (!b)
        return NULL;
    uint32_t gen = b->gen;   /* kept across uses: stale port packets stay stale */
    *b = (struct binding){ .kind = BIND_FS, .path = FAT_PATH, .test = d->test, .gen = gen,
                           .usb_if = -1, .disk = (uint32_t)(d - disks) + 1,
                           .part = (uint8_t)part };
    snprintf(b->name, sizeof(b->name), "fat-%s", fs_mount_path(b) + 1);
    d->fs[part] = (uint32_t)(b - devs) + 1;
    return b;
}

/* Start the filesystem service on partition `part` of d. */
static status_t fs_start(struct disk *d, unsigned part)
{
    if (!in_bootfs(FAT_PATH)) {
        say(false, "devmgr: %s: %s is not in bootfs: no filesystem", disk_name(d), FAT_PATH);
        return ERR_NOT_FOUND;
    }
    struct binding *b = fs_binding(d, part);
    if (!b)
        return ERR_NO_RESOURCES;
    b->last = start_driver(b);
    status_t st = b->last;
    say(false, "devmgr: %s partition %u -> %s at %s%s (%s)", disk_name(d), part + 1, FAT_PATH,
        fs_mount_path(b), part == PART_ESP ? ", read-only" : "", status_str(st));
    if (st != OK)
        fs_retire(b);
    return st;
}

/* d moves to `state`, one in which nothing of it is mounted, and its
 * services go: killed (their `block` channels are dead, or nobody needs
 * them) and their bindings freed. The state first, so the mounts change
 * once, not once per service. */
static void drop_services(struct disk *d, enum disk_state state)
{
    d->state = state;
    for (unsigned part = 0; part < 2; part++) {
        if (!d->fs[part])
            continue;
        struct binding *b = &devs[d->fs[part] - 1];
        if (b->proc)
            stop_driver(b, true, true);
        fs_retire(b);
    }
    mounts_update();
}

/* ---- the steps --------------------------------------------------------------- */

/* d is not (or no longer) a disk to mount. */
static void leave_alone(struct disk *d, const char *why)
{
    say(false, "devmgr: %s: %s: left alone", disk_name(d), why);
    drop_services(d, DISK_OTHER);
}

/* Step 1: ask d's driver what it holds. */
static void ask_info(struct disk *d)
{
    if (++txids == 0)
        txids++;
    struct storage_info_req q = { .txid = txids, .ordinal = STORAGE_INFO };
    d->txid = q.txid;
    d->deadline = now() + INFO_WAIT;
    d->state = DISK_INFO;
    status_t st = jam_channel_write(disk_ch(d), &q, sizeof(q), NULL, 0);
    if (st != OK) {
        char why[48];
        snprintf(why, sizeof(why), "can't ask its driver (%s)", status_str(st));
        leave_alone(d, why);
    }
}

/* Is another disk of d's kind the boot disk already? */
static bool boot_disk_taken(const struct disk *d)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (&disks[i] != d && disks[i].state == DISK_BOOT && disks[i].test == d->test)
            return true;
    return false;
}

/* SCSI INQUIRY text: space-padded, not terminated. */
static void inquiry_text(char *out, const uint8_t *in, unsigned n)
{
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == 0))
        n--;
    for (unsigned i = 0; i < n; i++)
        out[i] = in[i] >= 0x20 && in[i] < 0x7f ? (char)in[i] : '?';
    out[n] = 0;
}

/* Ask the ESP's service, which has just been started, for BOOT_FILE: an
 * fs.stat written without waiting for the answer (fs_answers takes it). */
static void ask_boot_file(struct disk *d)
{
    struct fs_stat_req q;
    memset(&q, 0, sizeof(q));
    if (++txids == 0)
        txids++;
    q.txid = txids;
    q.ordinal = FS_STAT;
    memcpy(q.path, BOOT_FILE, sizeof(BOOT_FILE));
    d->txid = q.txid;
    d->deadline = now() + ESP_WAIT;
    d->state = DISK_ESP;
    if (jam_channel_write(devs[d->fs[PART_ESP] - 1].client, &q, sizeof(q), NULL, 0) != OK)
        d->deadline = now();   /* disk_run_due gives up on it */
}

/* Step 2: storage.info answered. The two partition types decide whether
 * the ESP is worth a look. */
static void got_info(struct disk *d, const struct storage_info_rep *r)
{
    char vendor[9], product[17], why[64];
    inquiry_text(vendor, r->vendor, 8);
    inquiry_text(product, r->product, 16);
    uint8_t type[2] = { 0, 0 };
    status_t st = OK;
    for (uint8_t i = 0; st == OK && i < 2 && i < r->partitions; i++)
        st = storage_partition_until(disk_ch(d), now() + CALL_WAIT, i, &type[i], NULL, NULL);
    say(false, "devmgr: %s: %s %s, %lu MiB, %u partition(s), types %02x %02x", disk_name(d), vendor,
        product, (unsigned long)(r->blocks * r->block_size >> 20), r->partitions, type[0], type[1]);
    if (st != OK) {
        snprintf(why, sizeof(why), "its partitions can't be read (%s)", status_str(st));
        leave_alone(d, why);
    } else if (r->partitions < 2 || type[0] != TYPE_ESP || type[1] != TYPE_FAT32_LBA) {
        leave_alone(d, "not a Jam OS stick (an ESP, then a FAT32 data partition)");
    } else if (boot_disk_taken(d)) {
        leave_alone(d, "another disk is the boot disk already");
    } else if ((st = fs_start(d, PART_ESP)) != OK) {
        snprintf(why, sizeof(why), "no filesystem service for its ESP (%s)", status_str(st));
        leave_alone(d, why);
    } else {
        ask_boot_file(d);
    }
}

/* Step 3: the ESP's service answered the stat. */
static void got_stat(struct disk *d, status_t st, bool is_dir)
{
    char why[64];
    if (st != OK || is_dir) {
        snprintf(why, sizeof(why), "no %s on its ESP (%s)", BOOT_FILE + 1,
                 st != OK ? status_str(st) : "a directory");
        leave_alone(d, why);
        return;
    }
    if (boot_disk_taken(d)) {
        leave_alone(d, "another disk is the boot disk already");
        return;
    }
    d->state = DISK_BOOT;
    st = fs_start(d, PART_DATA);
    say(!d->test, "devmgr: %s is the boot disk: its ESP is mounted, its data partition %s%s",
        disk_name(d), st == OK ? "too" : "is not: ", st == OK ? "" : status_str(st));
    if (st != OK && !d->test)
        problems++;
    mounts_update();
}

/* Read what d's driver answered. */
static void disk_answers(struct disk *d)
{
    _Alignas(8) uint8_t buf[STORAGE_REP_MAX];
    uint32_t n = 0;
    while (d->state != DISK_FREE && next_msg(disk_ch(d), buf, sizeof(buf), &n) == OK) {
        const struct storage_info_rep *r = (const void *)buf;
        if (d->state != DISK_INFO || n < sizeof(struct idl_rep_hdr) || r->txid != d->txid)
            continue;   /* not what we wait for: an answer that came too late */
        status_t st = idl_rep_status(buf, n, sizeof(*r));
        if (st == OK) {
            got_info(d, r);
            continue;
        }
        char why[48];
        snprintf(why, sizeof(why), "storage.info failed (%s)", status_str(st));
        leave_alone(d, why);
    }
}

/* Read what b (a filesystem service) answered. */
static void fs_answers(struct binding *b)
{
    _Alignas(8) uint8_t buf[64];
    uint32_t n = 0;
    while (b->client && next_msg(b->client, buf, sizeof(buf), &n) == OK) {
        const struct fs_stat_rep *r = (const void *)buf;
        struct disk *d = disk_of(b);
        if (!d || d->state != DISK_ESP || b->part != PART_ESP ||
            n < sizeof(struct idl_rep_hdr) || r->txid != d->txid)
            continue;   /* not what we wait for: a late answer to someone's call */
        status_t st = idl_rep_status(buf, n, sizeof(*r));
        got_stat(d, st, st == OK && r->is_dir);
    }
}

void disk_events(struct binding *b)
{
    if (b->kind == BIND_FS)
        fs_answers(b);
    else if (disk_of(b))
        disk_answers(disk_of(b));
}

/* ---- disks coming and going ------------------------------------------------------- */

bool disk_attach(struct binding *b, uint32_t id)
{
    struct disk *d = disk_alloc();
    if (!d)
        return false;
    d->id = id;
    d->bind = (uint32_t)(b - devs) + 1;
    b->disk = (uint32_t)(d - disks) + 1;
    return true;
}

void disk_detach(struct binding *b)
{
    struct disk *d = disk_of(b);
    b->disk = 0;
    if (!d)
        return;
    drop_services(d, DISK_FREE);
}

void disk_started(struct binding *b)
{
    if (b->kind == BIND_FS)
        mounts_update();
    else if (disk_of(b))
        ask_info(disk_of(b));
}

void disk_stopped(struct binding *b)
{
    struct disk *d = disk_of(b);
    if (!d)
        return;
    if (b->kind == BIND_FS)
        mounts_update();
    else
        drop_services(d, DISK_DOWN);
}

bool fs_check_ended(struct binding *b)
{
    struct disk *d = disk_of(b);
    if (!d || d->state != DISK_ESP || b->part != PART_ESP)
        return false;
    leave_alone(d, "its ESP's filesystem service ended: no FAT volume it can read");
    return true;
}

status_t disk_test(handle_t ch, uint32_t *id)
{
    struct disk *d = disk_alloc();
    status_t st = d ? OK : ERR_NO_RESOURCES;
    if (st == OK)
        st = jam_port_bind(port, ch, KEY_DISK_OF(d - disks, d->gen),
                           SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        if (d)
            d->state = DISK_FREE;
        jam_handle_close(ch);
        return st;
    }
    d->test = true;
    d->ch = ch;
    d->id = *id = TEST_ID | ++test_ids;
    say(false, "devmgr: %s attached", disk_name(d));
    ask_info(d);
    return OK;
}

void disk_key(uint64_t key)
{
    uint32_t slot = KEY_INDEX(key);
    if (slot >= MAX_DISKS || disks[slot].state == DISK_FREE || !disks[slot].test ||
        KEY_GEN(key) != disks[slot].gen)
        return;   /* stale */
    struct disk *d = &disks[slot];
    disk_answers(d);
    signals_t seen = 0;
    if (jam_object_wait_one(d->ch, SIG_PEER_CLOSED, 0, &seen) != OK)
        return;
    say(false, "devmgr: %s gone", disk_name(d));
    drop_services(d, DISK_FREE);
    jam_port_unbind(port, d->ch, key);
    jam_handle_close(d->ch);
    d->ch = HANDLE_INVALID;
}

void disk_run_due(void)
{
    uint64_t t = now();
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        struct disk *d = &disks[i];
        if (d->deadline > t)
            continue;
        if (d->state == DISK_INFO)
            leave_alone(d, "its driver did not answer storage.info");
        else if (d->state == DISK_ESP)
            leave_alone(d, "its ESP's filesystem service did not answer");
    }
}

uint64_t disk_next_deadline(void)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if ((disks[i].state == DISK_INFO || disks[i].state == DISK_ESP) &&
            disks[i].deadline < next)
            next = disks[i].deadline;
    return next;
}

void disk_sync_all(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        const struct disk *d = &disks[i];
        if (d->state != DISK_BOOT || !d->fs[PART_DATA])
            continue;
        const struct binding *b = &devs[d->fs[PART_DATA] - 1];
        if (b->state != DEVMGR_SUP_RUNNING || !b->client)
            continue;
        status_t st = fs_sync_until(b->client, now() + SYNC_WAIT);
        say(st != OK && !d->test, "devmgr: %s: %s synced before stopping (%s)", disk_name(d),
            fs_mount_path(b), status_str(st));
    }
}

unsigned disk_mounts(struct mount *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        for (unsigned part = 0; part < 2 && disks[i].state == DISK_BOOT; part++) {
            if (!disks[i].fs[part] || n == DEVMGR_MAX_MOUNTS)
                continue;
            const struct binding *b = &devs[disks[i].fs[part] - 1];
            if (b->state != DEVMGR_SUP_RUNNING || !b->proc || !b->client)
                continue;
            out[n] = (struct mount){ .bind = (uint32_t)(b - devs), .gen = b->gen };
            snprintf(out[n].path, sizeof(out[n].path), "%s", fs_mount_path(b));
            n++;
        }
    }
    return n;
}
