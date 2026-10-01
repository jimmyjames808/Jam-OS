/* devmgr: disks and their filesystem services: the disk Jam OS booted
 * from (/esp, /data) and every other stick (/usb0, /usb1, ...). This
 * file has the disks and the steps; fsvc.c the services themselves (disk.h
 * what the two share).
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
 *      0xEF then 0x0C, and the disk is not the boot disk (step 4). Else a
 *      read-only filesystem service is started on the ESP and asked, again
 *      without waiting, for boot/jamos.elf;
 *   3. the file is there: this is the boot disk. The data partition's
 *      service starts too, and both are mounts (DEVMGR_MOUNTS, mounts.c):
 *      /esp and /data. Without the file, or if the service ends instead
 *      of answering (the ESP holds no FAT it can read), or with no answer
 *      within ESP_WAIT, the ESP's service is stopped and the disk is not
 *      the boot disk. One boot disk at a time: a second disk that
 *      qualifies while the first is mounted is not it either;
 *   4. any other disk (mount_others): a filesystem service is started on
 *      each partition whose type says FAT (fat_type; usb-storage lists a
 *      stick with no partition table and a FAT boot sector as one
 *      partition of type 00), on a READ-ONLY `block` channel, and takes
 *      the lowest free /usbN, one service at a time (others_pump) so the
 *      numbers follow the order the volumes were found in. It is a mount
 *      once it has answered a first fs.stat (its volume is mounted); one
 *      that ends with
 *      FAT_EXIT_NO_VOLUME instead is not started again: the partition is
 *      left alone, and nothing was written to it. A GPT disk (its table
 *      lists one partition of type EE) gets no mounts.
 * No answer to storage.info within INFO_WAIT leaves the disk alone.
 *
 * Never formatted: fat formats only when it is started with
 * FAT_ARG_FORMAT, and fs_format_arg gives that to the boot disk's data
 * partition alone. No other partition of any disk ever gets it, in either
 * mode.
 *
 * Read-only below the filesystem: an other disk's partition is opened
 * read-only (storage.open_partition), so usb-storage refuses every write
 * on that channel whatever fat does. DEVMGR_REMOUNT (disk_remount; the
 * shell's `mount -w /usbN`) has the service stop in order (fsctl.stop:
 * files closed, everything flushed, the volume clean) and starts it again
 * on a new channel opened read-write, or back.
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
#include <fs_idl.h>
#include <idl/storage.h>
#include "disk.h"

struct disk disks[MAX_DISKS];
/* Transaction ids of the requests written without waiting. The kernel
 * numbers channel_call's from 1 up, so these are billions of calls away
 * from any id a caller on the same channel could be waiting for. */
uint32_t txids = 0xd15c0000u;
static uint32_t test_ids;   /* test disks made so far */

handle_t disk_ch(const struct disk *d)
{
    return d->test ? d->ch : devs[d->bind - 1].client;
}

const char *disk_name(const struct disk *d)
{
    static char s[24];
    snprintf(s, sizeof(s), d->test ? "test disk %u" : "disk %u", d->id & ~TEST_ID);
    return s;
}

struct disk *disk_of(const struct binding *b)
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
status_t next_msg(handle_t h, void *buf, uint32_t cap, uint32_t *n)
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

/* ---- the steps --------------------------------------------------------------- */

/* d's driver doesn't answer: nothing of it is mounted. */
void leave_alone(struct disk *d, const char *why)
{
    say(false, "devmgr: %s: %s: left alone", disk_name(d), why);
    drop_services(d, DISK_ALONE);
}

/* d is not the boot disk: whatever looked at its ESP goes, and its FAT
 * partitions are mounted read-only. */
void not_boot(struct disk *d, const char *why)
{
    say(false, "devmgr: %s: %s: not the boot disk", disk_name(d), why);
    drop_services(d, DISK_OTHER);
    mount_others(d);
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
    status_t st;
    d->txid = ask_stat(&devs[d->fs[PART_ESP] - 1], BOOT_FILE, &st);
    d->deadline = now() + ESP_WAIT;
    d->state = DISK_ESP;
    if (st != OK)
        d->deadline = now();   /* disk_run_due gives up on it */
}

/* Step 2: storage.info answered. The two partition types decide whether
 * the ESP is worth a look. */
static void got_info(struct disk *d, const struct storage_info_rep *r)
{
    char vendor[9], product[17], why[64];
    inquiry_text(vendor, r->vendor, 8);
    inquiry_text(product, r->product, 16);
    uint8_t *type = d->type;
    status_t st = OK;
    memset(d->type, 0, sizeof(d->type));
    d->nparts = r->partitions;
    for (uint8_t i = 0; st == OK && i < MAX_PARTS && i < r->partitions; i++)
        st = storage_partition_until(disk_ch(d), now() + CALL_WAIT, i, &type[i], NULL, NULL);
    say(false, "devmgr: %s: %s %s, %lu MiB, %u partition(s), types %02x %02x", disk_name(d), vendor,
        product, (unsigned long)(r->blocks * r->block_size >> 20), r->partitions, type[0], type[1]);
    if (st != OK) {
        snprintf(why, sizeof(why), "its partitions can't be read (%s)", status_str(st));
        leave_alone(d, why);
    } else if (r->partitions < 2 || type[0] != TYPE_ESP || type[1] != TYPE_FAT32_LBA) {
        not_boot(d, "not a Jam OS stick (an ESP, then a FAT32 data partition)");
    } else if (boot_disk_taken(d)) {
        not_boot(d, "another disk is the boot disk already");
    } else if ((st = fs_start(d, PART_ESP, false)) != OK) {
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
        not_boot(d, why);
        return;
    }
    if (boot_disk_taken(d)) {
        not_boot(d, "another disk is the boot disk already");
        return;
    }
    d->state = DISK_BOOT;
    st = fs_start(d, PART_DATA, false);
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
        if (b->other) {
            /* The first fs.stat of a /usbN service: its volume is mounted. */
            if (!d || b->ready || n < sizeof(struct idl_rep_hdr) || r->txid != b->probe)
                continue;
            if (idl_rep_status(buf, n, sizeof(*r)) != OK)
                continue;   /* it can't even stat its root: no mount */
            b->ready = true;
            say(false, "devmgr: %s partition %u is %s, %s", disk_name(d), b->part + 1,
                fs_mount_path(b), b->rw ? "read-write" : "read-only");
            mounts_update();
            continue;
        }
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
    status_t st;
    if (b->kind == BIND_FS && b->other) {
        b->ready = false;   /* a mount once it answers (fs_answers) */
        b->probe = ask_stat(b, "/", &st);
    } else if (b->kind == BIND_FS) {
        mounts_update();
    } else if (disk_of(b)) {
        ask_info(disk_of(b));
    }
}

void disk_stopped(struct binding *b)
{
    struct disk *d = disk_of(b);
    if (!d)
        return;
    if (b->kind == BIND_FS) {
        b->ready = false;
        mounts_update();
    } else {
        drop_services(d, DISK_DOWN);
    }
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
            not_boot(d, "its ESP's filesystem service did not answer");
    }
    others_pump();   /* whatever happened may have been what the next one waited for */
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
        for (unsigned part = 0; part < MAX_PARTS; part++) {
            if ((d->state != DISK_BOOT && d->state != DISK_OTHER) || !d->fs[part])
                continue;
            const struct binding *b = &devs[d->fs[part] - 1];
            bool writable = b->other ? b->rw && b->ready : part == PART_DATA;
            if (!writable || b->state != DEVMGR_SUP_RUNNING || !b->client)
                continue;
            status_t st = fs_sync_until(b->client, now() + SYNC_WAIT);
            say(st != OK && !d->test, "devmgr: %s: %s synced before stopping (%s)", disk_name(d),
                fs_mount_path(b), status_str(st));
        }
    }
}

unsigned disk_mounts(struct mount *out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        bool mounted = disks[i].state == DISK_BOOT || disks[i].state == DISK_OTHER;
        for (unsigned part = 0; part < MAX_PARTS && mounted; part++) {
            if (!disks[i].fs[part] || n == DEVMGR_MAX_MOUNTS)
                continue;
            const struct binding *b = &devs[disks[i].fs[part] - 1];
            if (b->state != DEVMGR_SUP_RUNNING || !b->proc || !b->client)
                continue;
            if (b->other && !b->ready)
                continue;   /* no volume mounted yet (or none at all) */
            out[n] = (struct mount){ .bind = (uint32_t)(b - devs), .gen = b->gen };
            snprintf(out[n].path, sizeof(out[n].path), "%s", fs_mount_path(b));
            n++;
        }
    }
    return n;
}
