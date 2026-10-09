/* volumes: disks and their filesystem services: the disk Jam OS booted
 * from (/esp, /data) and every other stick (/usb0, /usb1, ...). This
 * file has the disks and the steps; fsvc.c the services themselves (disk.h
 * what the two share).
 *
 * A disk is a `storage` channel (abi/idl/storage.idl) of volumes' own:
 * one devmgr asked a usb-storage driver for (svc.connect on its DR_SERVE)
 * and handed over on the DISKS channel (DISKS_ATTACH, <volumes.h>), or the
 * channel a test handed over with VOLUMES_TEST_DISK. volumes is the only
 * client of it: nothing else ever gets a disk's channel, and a filesystem
 * service gets one partition's `block` channel and no more.
 *
 * What happens to a disk (enum disk_state):
 *   1. its channel comes: storage.info is asked without waiting (a stick
 *      may take seconds to spin up, and volumes must keep serving);
 *   2. the answer comes: fewer than two partitions, or types other than
 *      0xEF then 0x0C, and the disk is not the boot disk (step 4). When
 *      the boot loader named the disk the machine booted from (its MBR
 *      disk id: volumes' argument bootdisk=0x<id>), a disk with another id
 *      waits (DISK_HELD) until that one has come, and is not the boot disk
 *      then; if it never comes (the stick was swapped, or made without an
 *      id), the waiting one goes on after HELD_FROM_START. Else (or then)
 *      a read-only filesystem service is started on the ESP and asked,
 *      again without waiting, for boot/jamos.elf;
 *   3. the file is there: this is the boot disk. The data partition's
 *      service starts too, and both are mounts (VOLUMES_MOUNTS, mounts.c):
 *      /esp and /data. Without the file, or if the service ends instead
 *      of answering (the ESP holds no FAT it can read), or with no answer
 *      within ESP_WAIT, the ESP's service is stopped and the disk is not
 *      the boot disk. One boot disk at a time: a second disk that
 *      qualifies while the first is mounted is not it either. Without a
 *      disk id from the loader the first Jam OS disk to get here is the
 *      boot disk;
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
 * on that channel whatever fat does. VOLUMES_REMOUNT (disk_remount; the
 * shell's `mount -w /usbN`) has the service stop in order (fsctl.stop:
 * files closed, everything flushed, the volume clean) and starts it again
 * on a new channel opened read-write, or back.
 *
 * A filesystem service (bin/fat, <fatsvc.h>; a struct vol in vols[]) is
 * supervised like a driver (supervise.c): a crash or an error exit is
 * restarted with backoff, and given up on when it keeps ending (a data
 * partition that holds no FAT volume and isn't blank makes fat exit 1
 * every time). Each start opens a new `block` channel on its partition;
 * its `fs` channel is kept from one instance to the next (spare.c), so it
 * stays a mount while it restarts, and the mounts' generation doesn't
 * move (the next instance carries on with the views and files the dead
 * one made: user/services/fat/adopt.c).
 *
 * A disk whose driver is gone (the stick unplugged, usb-storage crashed,
 * a test disk's channel closed: its `storage` channel's peer closes)
 * loses its services at once: their `block` channels are dead. They are
 * killed, not asked to stop: nothing can be written any more. A real disk
 * then waits (DISK_DOWN) for devmgr's word: ATTACH again (its driver was
 * restarted: step 1 in the same slot) or GONE (the device went: it is
 * forgotten). Both may come in either order with the channel's end. When
 * the DISKS channel itself closes (devmgr ended, every driver with it),
 * its disks are forgotten without notices: the next devmgr tells of them
 * again.
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

const char *disk_name(const struct disk *d)
{
    static char s[24];
    snprintf(s, sizeof(s), d->test ? "test disk %u" : "disk %u", d->id & ~TEST_ID);
    return s;
}

struct disk *disk_of(const struct vol *b)
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
 * jam_channel_read's status once nothing is left. Nothing volumes asked for
 * comes with handles or is bigger than cap: handles are closed, a message
 * that doesn't fit is dropped. */
status_t next_msg(handle_t h, void *buf, uint32_t cap, uint32_t *n)
{
    for (int guard = 0; guard < 64; guard++) {
        handle_t hs[4];
        uint32_t nh = 0;
        struct channel_read_args a = {
            .h = h, .rd_cap = cap, .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = 4, .rd_hactual = (uint64_t)(uintptr_t)&nh,
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
    say(false, "volumes: %s: %s: left alone", disk_name(d), why);
    drop_services(d, DISK_ALONE);
}

/* d is not the boot disk: whatever looked at its ESP goes, and its FAT
 * partitions are mounted read-only. */
void not_boot(struct disk *d, const char *why)
{
    say(false, "volumes: %s: %s: not the boot disk", disk_name(d), why);
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
    status_t st = jam_channel_write(d->ch, &q, sizeof(q), NULL, 0);
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
    d->txid = ask_stat(&vols[d->fs[PART_ESP] - 1], BOOT_FILE, &st);
    d->deadline = now() + ESP_WAIT;
    d->state = DISK_ESP;
    if (st != OK)
        d->deadline = now();   /* disk_run_due gives up on it */
}

/* Start the ESP's service and ask it for BOOT_FILE (step 2's end). */
static void look_at_esp(struct disk *d)
{
    char why[64];
    status_t st = fs_start(d, PART_ESP, false);
    if (st != OK) {
        snprintf(why, sizeof(why), "no filesystem service for its ESP (%s)", status_str(st));
        leave_alone(d, why);
        return;
    }
    ask_boot_file(d);
}

/* Is d the disk the machine booted from, as far as the MBR disk ids can
 * tell? A test disk, or a boot whose loader named no disk, can't tell:
 * yes. */
static bool booted_from(struct disk *d)
{
    if (d->test || !boot_mbr_id)
        return true;
    uint32_t id = 0;
    status_t st = storage_disk_id_until(d->ch, now() + CALL_WAIT, &id);
    d->mbr_id = st == OK ? id : 0;
    return d->mbr_id == boot_mbr_id;
}

/* d is a Jam OS disk, but not the one the machine booted from: it waits
 * for that one (DISK_HELD; disk_run_due ends the wait). */
static void hold_back(struct disk *d)
{
    uint64_t t = now(), until = volumes_started + HELD_FROM_START;
    d->state = DISK_HELD;
    d->deadline = until > t + HELD_MIN ? until : t + HELD_MIN;
    say(false, "volumes: %s: disk id %08x, but the machine booted from disk id %08x: it waits "
        "%lu ms for that one", disk_name(d), d->mbr_id, boot_mbr_id,
        (unsigned long)((d->deadline - t) / NS_PER_MS));
}

/* The boot disk is mounted: the disks waiting for it are not the boot
 * disk. */
static void release_held(const struct disk *boot)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state == DISK_HELD && disks[i].test == boot->test)
            not_boot(&disks[i], "the machine booted from another disk");
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
        st = storage_partition_until(d->ch, now() + CALL_WAIT, i, &type[i], NULL, NULL);
    say(false, "volumes: %s: %s %s, %lu MiB, %u partition(s), types %02x %02x", disk_name(d), vendor,
        product, (unsigned long)(r->blocks * r->block_size >> 20), r->partitions, type[0], type[1]);
    d->mib = r->blocks * r->block_size >> 20;
    if (st != OK) {
        snprintf(why, sizeof(why), "its partitions can't be read (%s)", status_str(st));
        leave_alone(d, why);
    } else if (r->partitions < 2 || type[0] != TYPE_ESP || type[1] != TYPE_FAT32_LBA) {
        not_boot(d, "not a Jam OS stick (an ESP, then a FAT32 data partition)");
    } else if (boot_disk_taken(d)) {
        not_boot(d, "another disk is the boot disk already");
    } else if (!booted_from(d)) {
        hold_back(d);
    } else {
        look_at_esp(d);
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
    d->boot_down = false;
    release_held(d);
    disk_told_boot(d, true);   /* back after it was pulled out */
    st = fs_start(d, PART_DATA, false);
    say(!d->test, "volumes: %s is the boot disk: its ESP is mounted, its data partition %s%s",
        disk_name(d), st == OK ? "too" : "is not: ", st == OK ? "" : status_str(st));
    if (st != OK && !d->test) {
        problems++;
        notice_post(&notices, "Your files can't be saved",
                    "Its data partition didn't open.", '!',
                    NOTICE_RASPBERRY);
    }
    mounts_update();
}

/* Read what d's driver answered. */
static void disk_answers(struct disk *d)
{
    _Alignas(8) uint8_t buf[STORAGE_REP_MAX];
    uint32_t n = 0;
    while (d->state != DISK_FREE && next_msg(d->ch, buf, sizeof(buf), &n) == OK) {
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
void fs_answers(struct vol *b)
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
            say(false, "volumes: %s partition %u is %s, %s", disk_name(d), b->part + 1,
                fs_mount_path(b), b->rw ? "read-write" : "read-only");
            disk_told_added(d, b);
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

/* ---- disks coming and going ------------------------------------------------------- */

/* The DISKS channel (<volumes.h>): devmgr's end tells of its disks. */
static handle_t disks_ch;     /* our end (0: none) */
static uint16_t disks_gen;    /* bumped with every new one: in its port key */

/* d's `storage` channel goes (its port binding too). */
static void close_ch(struct disk *d)
{
    if (!d->ch)
        return;
    jam_port_unbind(port, d->ch, KEY_DISK_OF(d - disks, d->gen));
    jam_handle_close(d->ch);
    d->ch = HANDLE_INVALID;
    d->gen++;   /* a packet already queued for it is stale */
}

/* d (a real disk) is forgotten: its services go, its slot is free. */
static void forget(struct disk *d)
{
    drop_services(d, DISK_FREE);
    close_ch(d);
}

/* Every disk the DISKS channel told of is forgotten, without notices:
 * devmgr ended (its drivers with it), or init gave us a new channel. */
static void forget_all(const char *why)
{
    unsigned n = 0;
    quiet = true;
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state != DISK_FREE && !disks[i].test) {
            forget(&disks[i]);
            n++;
        }
    quiet = false;
    if (n)
        say(false, "volumes: %s: %u disk(s) forgotten", why, n);
}

/* The real disk called id, or NULL. */
static struct disk *disk_named(uint32_t id)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state != DISK_FREE && !disks[i].test && disks[i].id == id)
            return &disks[i];
    return NULL;
}

/* d's `storage` channel is ch (consumed), watched from now on. False: it
 * couldn't be watched (ch closed). */
static bool take_ch(struct disk *d, handle_t ch)
{
    close_ch(d);
    status_t st = jam_port_bind(port, ch, KEY_DISK_OF(d - disks, d->gen),
                                SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        say(false, "volumes: %s: can't watch its channel (%s)", disk_name(d), status_str(st));
        jam_handle_close(ch);
        return false;
    }
    d->ch = ch;
    return true;
}

/* DISKS_ATTACH: disk id's driver runs; ch (consumed) is a `storage`
 * channel of ours. A disk that was there under that id (its driver
 * restarted) starts again at step 1 in its slot. */
static void attach(uint32_t id, handle_t ch)
{
    struct disk *d = disk_named(id);
    if (d && d->state != DISK_DOWN)
        drop_services(d, DISK_DOWN);   /* its old channel's end not seen yet */
    if (!d && (d = disk_alloc()) != NULL)
        d->id = id;
    if (!d) {
        say(true, "volumes: disk %u: no room for another disk", id);
        jam_handle_close(ch);
        return;
    }
    if (!take_ch(d, ch)) {
        leave_alone(d, "its channel can't be watched");
        return;
    }
    ask_info(d);
}

/* The SYNC call (txid) on the DISKS channel: answered once every writable
 * mount has been synced. */
static void sync_request(uint32_t txid)
{
    disk_sync_all();
    struct volumes_rep r = { .txid = txid, .status = OK };
    (void)jam_channel_write(disks_ch, &r, VOLUMES_REP_HDR, NULL, 0);
}

void disks_set(handle_t ch)
{
    if (disks_ch) {
        jam_port_unbind(port, disks_ch, KEY_DISKS | disks_gen);
        jam_handle_close(disks_ch);
        disks_ch = HANDLE_INVALID;
        forget_all("a new disks channel");
    }
    if (!ch)
        return;
    disks_gen++;
    status_t st = jam_port_bind(port, ch, KEY_DISKS | disks_gen, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st != OK) {
        say(true, "volumes: can't watch the disks channel (%s): no disks", status_str(st));
        jam_handle_close(ch);
        return;
    }
    disks_ch = ch;
}

void disks_event(uint64_t key)
{
    if (!disks_ch || key != (KEY_DISKS | disks_gen))
        return;   /* stale */
    for (int guard = 0; guard < 64; guard++) {
        _Alignas(8) uint8_t buf[64];
        handle_t hs[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = disks_ch, .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = 4, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(disks_ch, n, nh);   /* nothing devmgr sends is that big */
            continue;
        }
        if (st == ERR_PEER_CLOSED) {
            /* devmgr ended: its drivers went with it. */
            jam_port_unbind(port, disks_ch, KEY_DISKS | disks_gen);
            jam_handle_close(disks_ch);
            disks_ch = HANDLE_INVALID;
            forget_all("devmgr's disks channel closed");
            return;
        }
        if (st != OK)
            return;
        const struct disks_msg *m = (const struct disks_msg *)buf;
        bool whole = n == sizeof(*m);
        if (whole && m->ordinal == DISKS_ATTACH && nh == 1 && !m->txid) {
            attach(m->id, hs[0]);
            continue;
        }
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        if (whole && m->ordinal == DISKS_GONE && !nh && !m->txid && disk_named(m->id)) {
            say(false, "volumes: %s: the device is gone", disk_name(disk_named(m->id)));
            forget(disk_named(m->id));
        } else if (whole && m->ordinal == DISKS_SYNC && !nh && m->txid) {
            sync_request(m->txid);
        }
    }
}

/* The DISKS channel is gone too (devmgr ended, its drivers with it): a
 * disk's driver's end is no news. */
static bool devmgr_gone(void)
{
    signals_t seen = 0;
    return !disks_ch || jam_object_wait_one(disks_ch, SIG_PEER_CLOSED, 0, &seen) == OK;
}

void disk_started(struct vol *b)
{
    status_t st;
    if (b->other && !b->ready) {
        /* A mount once it answers (fs_answers). One that restarts on the
         * channel it kept is one still (disk_stopped). */
        b->probe = ask_stat(b, "/", &st);
    } else {
        mounts_update();
    }
}

void disk_stopped(struct vol *b)
{
    if (!disk_of(b))
        return;
    /* Its kept `fs` channel stays a mount while it restarts: calls on it
     * wait for the next instance. */
    if (!fs_channel_kept(b))
        b->ready = false;
    mounts_update();
}

status_t disk_test(handle_t ch, uint32_t *id)
{
    struct disk *d = disk_alloc();
    if (!d) {
        jam_handle_close(ch);
        return ERR_NO_RESOURCES;
    }
    if (!take_ch(d, ch)) {
        d->state = DISK_FREE;
        return ERR_NO_RESOURCES;
    }
    d->test = true;
    d->id = *id = TEST_ID | ++test_ids;
    say(false, "volumes: %s attached", disk_name(d));
    ask_info(d);
    return OK;
}

void disk_key(uint64_t key)
{
    uint32_t slot = KEY_INDEX(key);
    if (slot >= MAX_DISKS || disks[slot].state == DISK_FREE || !disks[slot].ch ||
        KEY_GEN(key) != disks[slot].gen)
        return;   /* stale */
    struct disk *d = &disks[slot];
    disk_answers(d);
    signals_t seen = 0;
    if (!d->ch || jam_object_wait_one(d->ch, SIG_PEER_CLOSED, 0, &seen) != OK)
        return;
    if (d->test) {
        say(false, "volumes: %s gone", disk_name(d));
        drop_services(d, DISK_FREE);
        close_ch(d);
        return;
    }
    /* Its driver ended: the disk waits for devmgr's next ATTACH (the
     * driver restarted) or GONE (the device went). If devmgr itself went,
     * the disk is forgotten quietly instead. */
    close_ch(d);
    if (devmgr_gone()) {
        quiet = true;
        drop_services(d, DISK_FREE);
        quiet = false;
        return;
    }
    say(false, "volumes: %s: its driver ended", disk_name(d));
    drop_services(d, DISK_DOWN);
}

void disk_close_all(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        close_ch(&disks[i]);
    if (disks_ch)
        jam_handle_close(disks_ch);
    disks_ch = HANDLE_INVALID;
}

/* d waited for the disk the machine booted from, which hasn't come: d
 * may be the boot disk after all. */
static void held_over(struct disk *d)
{
    if (boot_disk_taken(d)) {
        not_boot(d, "another disk is the boot disk already");
        return;
    }
    say(false, "volumes: %s: the disk the machine booted from (id %08x) is not here: this one "
        "(id %08x) may be the boot disk", disk_name(d), boot_mbr_id, d->mbr_id);
    look_at_esp(d);
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
        else if (d->state == DISK_HELD)
            held_over(d);
    }
    others_pump();   /* whatever happened may have been what the next one waited for */
}

uint64_t disk_next_deadline(void)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if ((disks[i].state == DISK_INFO || disks[i].state == DISK_ESP ||
             disks[i].state == DISK_HELD) && disks[i].deadline < next)
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
            const struct vol *b = &vols[d->fs[part] - 1];
            bool writable = b->other ? b->rw && b->ready : part == PART_DATA;
            if (!writable || b->state != VOLUMES_SUP_RUNNING || !b->client)
                continue;
            status_t st = fs_sync_until(b->client, now() + SYNC_WAIT);
            say(st != OK && !d->test, "volumes: %s: %s synced before stopping (%s)", disk_name(d),
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
            if (!disks[i].fs[part] || n == VOLUMES_MAX_MOUNTS)
                continue;
            const struct vol *b = &vols[disks[i].fs[part] - 1];
            /* Running, or between two instances on the channel it kept
             * (its death is being handled, or its restart is due). */
            bool alive = b->state == VOLUMES_SUP_RUNNING || b->state == VOLUMES_SUP_RESTARTING;
            if (!alive || !b->client || (!b->proc && !fs_channel_kept(b)))
                continue;
            if (b->other && !b->ready)
                continue;   /* no volume mounted yet (or none at all) */
            if (!b->other && b->part == PART_ESP && b->rw)
                continue;   /* init's while it writes the stick (ESP_WRITE): nobody else's */
            out[n] = (struct mount){ .vol = (uint32_t)(b - vols), .gen = b->chan_gen };
            snprintf(out[n].path, sizeof(out[n].path), "%s", fs_mount_path(b));
            n++;
        }
    }
    return n;
}
