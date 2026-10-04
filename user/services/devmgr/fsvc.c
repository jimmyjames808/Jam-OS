/* devmgr: the filesystem services on the disks' partitions (disk.c has
 * the disks and the steps that decide what each is): starting one on a
 * partition, its handles and its mount path, the /usbN services of disks
 * that aren't the boot disk (one started at a time, so the numbers follow
 * the order the volumes were found in), a service that ended,
 * DEVMGR_REMOUNT, and DEVMGR_ESP_WRITE (the boot disk's ESP read-write
 * for init alone: the same stop and restart, and no mount meanwhile).
 *
 * A service's `fs` channel is devmgr's, both ends, from its first start
 * until it is retired, given up on or stopped in order (a remount): each
 * instance is handed a duplicate of the server end (fs_serve_end), so a
 * restart after a death (spare.c) is the same channel and the same mount.
 * A remount is a new channel: the service stopped in order. */
#include <fatsvc.h>
#include <fs_idl.h>
#include <idl/fsctl.h>
#include <idl/storage.h>
#include "disk.h"

const char *fs_mount_path(const struct binding *b)
{
    static char usb[16];
    const struct disk *d = disk_of(b);
    bool test = d && d->test;
    if (b->other) {
        snprintf(usb, sizeof(usb), "/usb%u%s", b->usbn, test ? "-test" : "");
        return usb;
    }
    if (b->part == PART_ESP)
        return test ? "/esp-test" : "/esp";
    return test ? "/data-test" : "/data";
}

void fs_ctl_close(struct binding *b)
{
    if (b->ctl)
        jam_handle_close(b->ctl);
    b->ctl = HANDLE_INVALID;
}

void fs_retire(struct binding *b)
{
    fs_ctl_close(b);
    fs_kept_release(b);
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
        if (disks[i].state != DISK_FREE && disks[i].id == id && part < MAX_PARTS &&
            disks[i].fs[part])
            return &devs[disks[i].fs[part] - 1];
    return NULL;
}

struct binding *fs_find_mount(uint32_t which, uint32_t flags)
{
    bool test = flags & DEVMGR_MOUNT_TEST;
    if (flags & ~DEVMGR_MOUNT_TEST)
        return NULL;
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        if (b->kind != BIND_FS || !b->path || b->test != test)
            continue;
        bool hit = which >= DEVMGR_MOUNT_USB ? b->other && b->usbn == which - DEVMGR_MOUNT_USB
                   : which == DEVMGR_MOUNT_ESP ? !b->other && b->part == PART_ESP
                   : which == DEVMGR_MOUNT_DATA && !b->other && b->part == PART_DATA;
        if (hit)
            return b;
    }
    return NULL;
}

bool fs_channel_kept(const struct binding *b)
{
    return b->kind == BIND_FS && b->client && b->serve;
}

status_t fs_serve_end(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (!fs_channel_kept(b)) {
        /* The first start, or the last one failed after its client end
         * went (start_driver closes it): a new channel. */
        handle_t client, serve;
        status_t st = jam_channel_create(&client, &serve);
        if (st != OK)
            return st;
        close_client(b);
        if (b->serve)
            jam_handle_close(b->serve);
        b->client = client;
        b->serve = serve;
        b->chan_gen++;   /* a new mount for DEVMGR_MOUNTS */
    }
    handle_t h;
    status_t st = jam_handle_duplicate(b->serve, RIGHT_SAME, &h);
    if (st != OK)
        return st;
    x[*n] = (struct spawn_handle){ FAT_SR_SERVE, h };
    xr[(*n)++] = RIGHT_SAME;
    return OK;
}

status_t fs_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    const struct disk *d = disk_of(b);
    if (!d || d->state == DISK_FREE || d->state == DISK_DOWN)
        return ERR_PEER_CLOSED;
    handle_t blk;
    /* Read-only unless it is the boot disk's data partition, another
     * disk's partition after `mount -w`, or the boot disk's ESP while init
     * writes it (ESP_WRITE). */
    bool read_only = !b->other && b->part == PART_DATA ? false : !b->rw;
    status_t st = OK;
    if (!fs_block_prepared(b, &blk))
        st = storage_open_partition_until(disk_ch(d), now() + CALL_WAIT, b->part, read_only,
                                          &blk);
    if (st != OK)
        return st;
    x[*n] = (struct spawn_handle){ FAT_SR_BLOCK, blk };
    xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;   /* not to be passed on */
    /* Its control channel: ours alone, a new one for every start. Without
     * one the service can still be synced and killed. */
    handle_t theirs;
    fs_ctl_close(b);
    if (jam_channel_create(&b->ctl, &theirs) == OK) {
        x[*n] = (struct spawn_handle){ FAT_SR_CTL, theirs };
        xr[(*n)++] = DEVMGR_DRV_CHAN_RIGHTS;
    } else {
        b->ctl = HANDLE_INVALID;
    }
    fs_kept_handles(b, x, xr, n);   /* SR_STATE, SR_KEEP */
    return OK;
}

const char *fs_format_arg(const struct binding *b)
{
    const struct disk *d = disk_of(b);
    bool boot_data = d && d->state == DISK_BOOT && !b->other && b->part == PART_DATA;
    return boot_data ? FAT_ARG_FORMAT : NULL;
}

const char *fs_end_arg(const struct binding *b)
{
    if (!b->ended_at)
        return NULL;
    return b->kill_at ? FAT_ARG_KILLED : FAT_ARG_CRASHED;
}

/* The lowest N no running or restarting service has as its /usbN (test
 * disks count apart: /usbN-test), or -1. */
static int usb_number(bool test)
{
    for (unsigned n = 0; n < MAX_USB_MOUNTS; n++) {
        bool used = false;
        for (unsigned i = 0; i < ndevs && !used; i++)
            used = devs[i].kind == BIND_FS && devs[i].path && devs[i].other &&
                   devs[i].test == test && devs[i].usbn == n;
        if (!used)
            return (int)n;
    }
    return -1;
}

/* A free BIND_FS binding (or a new one) for partition `part` of d; `other`:
 * d is not the boot disk, and the service gets a /usbN. */
static struct binding *fs_binding(struct disk *d, unsigned part, bool other)
{
    struct binding *b = NULL;
    int usbn = other ? usb_number(d->test) : 0;
    if (usbn < 0)
        return NULL;
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
                           .part = (uint8_t)part, .other = other, .usbn = (uint8_t)usbn };
    snprintf(b->name, sizeof(b->name), "fat-%s", fs_mount_path(b) + 1);
    d->fs[part] = (uint32_t)(b - devs) + 1;
    return b;
}

/* Start the filesystem service on partition `part` of d. */
status_t fs_start(struct disk *d, unsigned part, bool other)
{
    if (!in_bootfs(FAT_PATH)) {
        say(false, "devmgr: %s: %s is not in bootfs: no filesystem", disk_name(d), FAT_PATH);
        return ERR_NOT_FOUND;
    }
    struct binding *b = fs_binding(d, part, other);
    if (!b) {
        say(false, "devmgr: %s partition %u: no room for another mount", disk_name(d), part + 1);
        return ERR_NO_RESOURCES;
    }
    b->last = fs_run(b, false);
    status_t st = b->last;
    say(false, "devmgr: %s partition %u -> %s at %s%s (%s)", disk_name(d), part + 1, FAT_PATH,
        fs_mount_path(b), other || part == PART_ESP ? ", read-only" : "", status_str(st));
    if (st != OK)
        fs_retire(b);
    return st;
}

/* d moves to `state`, one in which nothing of it is mounted, and its
 * services go: killed (their `block` channels are dead, or nobody needs
 * them) and their bindings freed. The state first, so the mounts change
 * once, not once per service. */
void drop_services(struct disk *d, enum disk_state state)
{
    d->state = state;
    d->want = 0;
    for (unsigned part = 0; part < MAX_PARTS; part++) {
        if (!d->fs[part])
            continue;
        struct binding *b = &devs[d->fs[part] - 1];
        if (b->proc)
            stop_driver(b, true, true);
        fs_retire(b);
    }
    mounts_update();
}

/* May a partition of this MBR type hold a FAT volume? 00 is usb-storage's
 * "no table: the whole disk is one FAT volume". */
static bool fat_type(uint8_t type)
{
    return type == 0x00 || type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0b ||
           type == 0x0c || type == 0x0e || type == TYPE_ESP;
}

/* Start the next /usbN service that is waited for, unless one is still
 * finding out whether its partition holds a volume. One at a time, so the
 * numbers go to the volumes in the order they were found, with no gap
 * where a partition turned out to hold none. */
void others_pump(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        const struct binding *b = &devs[i];
        bool alive = b->state == DEVMGR_SUP_RUNNING || b->state == DEVMGR_SUP_RESTARTING;
        if (b->kind == BIND_FS && b->path && b->other && !b->ready && alive)
            return;
    }
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        struct disk *d = &disks[i];
        for (unsigned part = 0; part < MAX_PARTS && d->state == DISK_OTHER; part++) {
            if (!(d->want & 1u << part))
                continue;
            d->want &= (uint8_t)~(1u << part);
            if (fs_start(d, part, true) == OK)
                return;
        }
    }
}

/* Step 4: d is not the boot disk. Each of its FAT partitions gets a
 * read-only filesystem service and a /usbN (others_pump starts them). */
void mount_others(struct disk *d)
{
    d->state = DISK_OTHER;
    d->want = 0;
    for (unsigned i = 0; i < d->nparts && i < MAX_PARTS; i++) {
        if (fat_type(d->type[i]))
            d->want |= (uint8_t)(1u << i);
        else
            say(false, "devmgr: %s partition %u: type %02x is not FAT: left alone", disk_name(d),
                i + 1, d->type[i]);
    }
    if (!d->want)
        say(false, "devmgr: %s: no FAT partition to mount: left alone", disk_name(d));
    others_pump();
}

/* An fs.stat of `path` written to b's service without waiting for the
 * answer (fs_answers takes it). Returns its transaction id. */
uint32_t ask_stat(const struct binding *b, const char *path, status_t *st)
{
    struct fs_stat_req q;
    memset(&q, 0, sizeof(q));
    if (++txids == 0)
        txids++;
    q.txid = txids;
    q.ordinal = FS_STAT;
    memcpy(q.path, path, strlen(path) + 1);
    *st = jam_channel_write(b->client, &q, sizeof(q), NULL, 0);
    return q.txid;
}

bool fs_check_ended(struct binding *b, bool no_volume)
{
    struct disk *d = disk_of(b);
    if (!d)
        return false;
    if (d->state == DISK_ESP && b->part == PART_ESP && !b->other) {
        not_boot(d, "its ESP's filesystem service ended: no FAT volume it can read");
        return true;
    }
    if (!b->other || !no_volume)
        return false;
    say(false, "devmgr: %s partition %u (type %02x) holds no FAT volume %s can read: left alone, "
        "nothing written to it", disk_name(d), b->part + 1, d->type[b->part], FAT_PATH);
    problems += !job_empty(b->job, b->path);
    forget_driver(b);
    fs_retire(b);
    return true;
}

/* b's running service stopped in order and started again on a `block`
 * channel opened read-write or read-only (its slot freed if it can't
 * start). */
static status_t fs_restart(struct binding *b, bool writable)
{
    uint64_t t0 = now();
    char path[16];
    snprintf(path, sizeof(path), "%s", fs_mount_path(b));
    /* The service stops in order (fsctl.stop): its files closed, what was
     * written on the stick, the volume clean; a write that comes later
     * fails instead of being lost. Only one that doesn't answer is killed. */
    status_t stopped = b->ctl ? fsctl_stop_until(b->ctl, now() + SYNC_WAIT) : ERR_NOT_SUPPORTED;
    if (stopped != OK)
        say(false, "devmgr: %s: its filesystem service did not stop in order (%s): killed%s",
            path, status_str(stopped), b->rw ? "; what it had not synced is lost" : "");
    stop_driver(b, stopped != OK, true);
    b->state = DEVMGR_SUP_NONE;
    sup_reset(b);
    /* Stopped in order: it kept nothing, and the next one starts fresh on a
     * new `fs` channel (the old one's clients see ERR_PEER_CLOSED). */
    fs_kept_release(b);
    b->rw = writable;
    b->last = fs_run(b, false);   /* a new `block` channel, opened the new way */
    status_t st = b->last;
    say(false, "devmgr: %s: its filesystem service started again, %s (%s), %lu ms after "
        "the remount was asked for", path, writable ? "read-write" : "read-only",
        status_str(st), (unsigned long)((now() - t0) / NS_PER_MS));
    if (st != OK) {
        fs_retire(b);
        mounts_update();
    }
    return st;
}

status_t disk_remount(unsigned n, bool test, bool writable)
{
    struct binding *b = NULL;
    for (unsigned i = 0; i < ndevs && !b; i++)
        if (devs[i].kind == BIND_FS && devs[i].path && devs[i].other && devs[i].test == test &&
            devs[i].usbn == n)
            b = &devs[i];
    if (!b)
        return ERR_NOT_FOUND;
    if (b->state != DEVMGR_SUP_RUNNING || !b->proc || !b->ready)
        return ERR_BAD_STATE;   /* not mounted (yet, or no longer) */
    if (b->rw == writable)
        return OK;
    return fs_restart(b, writable);
}

status_t disk_esp_write(bool writable, handle_t *out)
{
    struct binding *b = NULL;
    for (unsigned i = 0; i < MAX_DISKS && !b; i++)
        if (disks[i].state == DISK_BOOT && !disks[i].test && disks[i].fs[PART_ESP])
            b = &devs[disks[i].fs[PART_ESP] - 1];
    if (!b)
        return ERR_NOT_FOUND;
    if (b->state != DEVMGR_SUP_RUNNING || !b->proc || !b->client)
        return ERR_BAD_STATE;   /* stopped, or restarting after a crash */
    status_t st = b->rw == writable ? OK : fs_restart(b, writable);
    if (st == OK && writable)
        st = jam_handle_duplicate(b->client, RIGHT_SAME, out);
    return st;
}
