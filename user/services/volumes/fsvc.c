/* volumes: the filesystem services on the disks' partitions (disk.c has
 * the disks and the steps that decide what each is): starting one on a
 * partition, its handles and its mount path, the /usbN services of disks
 * that aren't the boot disk (one started at a time, so the numbers follow
 * the order the volumes were found in), a service that ended,
 * volumes.remount, and volesp (the boot disk's ESP read-write
 * for init alone: the same stop and restart, and no mount meanwhile).
 *
 * Which service a partition gets (fs_choose, in the start's task): a
 * partition whose type can hold ext (MBR 0x83, GPT's Linux filesystem
 * type, Jam OS's data type) and the boot disk's data partition are looked
 * at for the ext superblock (probe.c); bin/extfs starts on one that has it,
 * bin/fat on any other FAT-typed one (a Linux-only type with no ext volume
 * is left alone). An ext volume is mounted read-write only where it is
 * Jam OS's: the data partition of a Jam OS drive (volumes' own mark check,
 * mark.c), which is /jam, and a test disk's; someone else's ext volume is
 * read-only always, and `mount -w` on it is refused (Q9 of
 * docs/M13.6-PLAN.md). The boot disk's own data partition, ext or FAT, is
 * /data as before.
 *
 * A service's `fs` channel is volumes', both ends, from its first start
 * until it is retired, given up on or stopped in order (a remount): each
 * instance is handed a duplicate of the server end (fs_serve_end), so a
 * restart after a death (spare.c) is the same channel and the same mount.
 * A remount is a new channel: the service stopped in order.
 *
 * A start waits for its `block` channel (storage.open_partition, a call to
 * the disk's driver), so every start runs in a task (calls.c): fs_start's
 * own (start_main), a restart's (supervise.c), a remount's and the ESP
 * write's (fs_restart, in their request's task). */
#include <extfssvc.h>
#include <fatsvc.h>
#include <fs_idl.h>
#include <idl/fsctl.h>
#include <idl/storage.h>
#include "disk.h"

const char *fs_mount_path(const struct vol *b)
{
    static char usb[16];
    const struct disk *d = disk_of(b);
    bool test = d && d->test;
    if (b->other) {
        if (b->jam)
            return test ? "/jam-test" : "/jam";
        snprintf(usb, sizeof(usb), "/%s%u%s", b->nvme ? "nvme" : "usb", b->usbn,
                 test ? "-test" : "");
        return usb;
    }
    if (b->part == PART_ESP)
        return test ? "/esp-test" : "/esp";
    return test ? "/data-test" : "/data";
}

void fs_ctl_close(struct vol *b)
{
    if (b->ctl) {
        jam_port_unbind(port, b->ctl, KEY_FSCTL_OF(b - vols, b->ctl_gen));
        call_fail(b->ctl);   /* a task waiting for fsctl.stop's answer: none comes */
        jam_handle_close(b->ctl);
    }
    b->ctl = HANDLE_INVALID;
}

void fsctl_key(uint64_t key)
{
    uint32_t i = KEY_INDEX(key);
    if (i >= nvols || !vols[i].ctl || KEY_GEN(key) != (vols[i].ctl_gen & 0xffffu))
        return;   /* stale */
    struct vol *b = &vols[i];
    _Alignas(8) uint8_t buf[CALL_REP_MAX];
    _Static_assert(FSCTL_REP_MAX <= CALL_REP_MAX, "fsctl's answers fit");
    struct idl_msg m;
    for (int guard = 0; guard < 64 && b->ctl; guard++) {
        status_t st = idl_reply_read(b->ctl, buf, sizeof(buf), &m);
        if (st == ERR_INTERNAL)
            continue;
        if (st == ERR_PEER_CLOSED)
            call_fail(b->ctl);   /* the service ended without answering */
        if (st != OK)
            return;
        if (!call_offer(b->ctl, buf, &m))
            idl_msg_drop(&m);
    }
    /* 64 read and maybe more queued (a service that writes as fast as we
     * read): the binding fires when the channel turns readable, not while
     * it stays so, and without another turn fsctl.stop's answer, or the
     * channel's end, would never be seen. Unwatched, the channel goes: the
     * service can still be killed. */
    if (b->ctl && port_again(port, b->ctl, key, SIG_READABLE | SIG_PEER_CLOSED) != OK)
        fs_ctl_close(b);
}

void fs_retire(struct vol *b)
{
    fs_ctl_close(b);
    fs_kept_release(b);
    struct disk *d = disk_of(b);
    if (d && d->fs[b->part] == (uint32_t)(b - vols) + 1)
        d->fs[b->part] = 0;
    sup_reset(b);
    fs_close_client(b);
    b->state = VOLUMES_SUP_NONE;
    b->disk = 0;
    b->path = NULL;   /* a free slot now */
    b->serial++;      /* the tasks at work on it stop */
    b->starting = b->stopping = b->busy = false;
    if (tasks)
        task_kick(tasks);
}

struct vol *fs_find_mount(const char *path, uint32_t len)
{
    for (unsigned i = 0; i < nvols; i++) {
        struct vol *b = &vols[i];
        if (!b->path)
            continue;
        const char *p = fs_mount_path(b);
        if (strlen(p) == len && !memcmp(p, path, len))
            return b;
    }
    return NULL;
}

bool fs_channel_kept(const struct vol *b)
{
    return b->client && b->serve;
}

/* A channel pair whose server end receives no handles. */
static status_t fs_pair(handle_t *client, handle_t *serve)
{
    status_t st = jam_channel_create(client, serve);
    if (st != OK)
        return st;
    st = jam_channel_set_flags(*serve, CHANNEL_NO_HANDLES);
    if (st != OK) {
        jam_handle_close(*client);
        jam_handle_close(*serve);
    }
    return st;
}

status_t fs_new_channel(struct vol *b, handle_t *client, handle_t *serve)
{
    status_t st = fs_pair(client, serve);
    if (st != OK)
        return st;
    /* Without our own one our calls go on the shared channel, as before. */
    if (!b->own && fs_pair(&b->own, &b->own_serve) != OK)
        b->own = b->own_serve = HANDLE_INVALID;
    b->chan_gen++;   /* a new mount for the watches, whoever made it */
    return OK;
}

handle_t fs_asker(const struct vol *b)
{
    return b->own ? b->own : b->client;
}

status_t fs_serve_end(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (!fs_channel_kept(b)) {
        /* The first start, or the last one failed after its client end
         * went (fs_spawn closes it): a new channel. */
        fs_close_client(b);   /* our own channel goes with it: made again beside the new */
        if (b->serve)
            jam_handle_close(b->serve);
        b->serve = HANDLE_INVALID;
        status_t st = fs_new_channel(b, &b->client, &b->serve);   /* marked: receives no handles */
        if (st != OK) {
            b->client = b->serve = HANDLE_INVALID;
            return st;
        }
    }
    handle_t h;
    status_t st = jam_handle_duplicate(b->serve, RIGHT_SAME, &h);
    if (st != OK)
        return st;
    x[*n] = (struct spawn_handle){ FAT_SR_SERVE, h };
    xr[(*n)++] = RIGHT_SAME;
    if (b->own_serve && jam_handle_duplicate(b->own_serve, RIGHT_SAME, &h) == OK) {
        x[*n] = (struct spawn_handle){ FAT_SR_OWN, h };
        xr[(*n)++] = RIGHT_SAME;
    }
    return OK;
}

status_t fs_open_block(struct vol *b, bool read_only, handle_t *out)
{
    const struct disk *d = disk_of(b);
    if (!d || !d->ch || d->state == DISK_FREE || d->state == DISK_DOWN)
        return ERR_PEER_CLOSED;
    if (!in_task())   /* nobody to give the CPU to: the call as it was */
        return storage_open_partition_until(d->ch, now() + CALL_WAIT, b->part, read_only, out);
    uint32_t serial = b->serial;
    struct call c;
    call_begin(&c, d->ch);
    status_t st = storage_open_partition_send(d->ch, c.txid, b->part, read_only);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + CALL_WAIT);
    if (st == OK)
        st = storage_open_partition_result(c.rep, &c.m, out);
    if (st == OK && (b->serial != serial || stopping)) {
        jam_handle_close(*out);   /* b went meanwhile: nobody to give it to */
        st = ERR_PEER_CLOSED;
    }
    return st;
}

status_t fs_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    const struct disk *d = disk_of(b);
    if (!d || d->state == DISK_FREE || d->state == DISK_DOWN)
        return ERR_PEER_CLOSED;
    handle_t blk;
    /* Read-only unless it is the boot disk's data partition, another
     * disk's partition after `mount -w`, or the boot disk's ESP while init
     * writes it (volesp). */
    bool read_only = !b->other && b->part == PART_DATA ? false : !b->rw;
    status_t st = OK;
    if (!fs_block_prepared(b, &blk))
        st = fs_open_block(b, read_only, &blk);   /* waits, in a task */
    if (st != OK)
        return st;
    x[*n] = (struct spawn_handle){ FAT_SR_BLOCK, blk };
    xr[(*n)++] = FAT_CHAN_RIGHTS;   /* not to be passed on */
    /* Its control channel: ours alone, a new one for every start. Without
     * one the service can still be synced and killed. Neither end takes a
     * handle (no fsctl message carries one), so neither queue can ever
     * hold a channel end (M12's D2). */
    handle_t ours = HANDLE_INVALID, theirs = HANDLE_INVALID;
    fs_ctl_close(b);
    if (jam_channel_create(&ours, &theirs) == OK &&
        jam_channel_set_flags(theirs, CHANNEL_NO_HANDLES) == OK &&
        jam_channel_set_flags(ours, CHANNEL_NO_HANDLES) == OK &&
        jam_port_bind(port, ours, KEY_FSCTL_OF(b - vols, b->ctl_gen + 1),
                      SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT) == OK) {
        b->ctl = ours;   /* its answers come to the loop (fsctl_key) */
        b->ctl_gen++;
        x[*n] = (struct spawn_handle){ FAT_SR_CTL, theirs };
        xr[(*n)++] = FAT_CHAN_RIGHTS;
    } else {
        if (ours)
            jam_handle_close(ours);
        if (theirs)
            jam_handle_close(theirs);
    }
    fs_kept_handles(b, x, xr, n);   /* SR_STATE, SR_KEEP */
    return OK;
}

const char *fs_format_arg(const struct vol *b)
{
    const struct disk *d = disk_of(b);
    bool boot_data = d && d->state == DISK_BOOT && !b->other && b->part == PART_DATA;
    /* A partition of a disk prepare.c is making Jam OS's (by our own check),
     * while it is writable: fat formats it if its first sector holds no
     * boot signature, and never otherwise. (An ext volume is never made
     * here: its partition is blank, so it goes to fat. U7b changes that.) */
    bool fresh = d && b->other && b->rw && (d->fresh >> b->part & 1) && disk_jamos_gpt(d);
    return !b->ext && (boot_data || fresh) ? FAT_ARG_FORMAT : NULL;
}

const char *fs_end_arg(const struct vol *b)
{
    if (!b->ended_at)
        return NULL;
    return b->kill_at ? FAT_ARG_KILLED : FAT_ARG_CRASHED;
}

/* The lowest N no running or restarting service has as its /usbN, or
 * /nvmeN when `nvme` (test disks count apart: /usbN-test), or -1. */
static int mount_number(bool test, bool nvme)
{
    for (unsigned n = 0; n < (nvme ? MAX_NVME_MOUNTS : MAX_USB_MOUNTS); n++) {
        bool used = false;
        for (unsigned i = 0; i < nvols && !used; i++)
            used = vols[i].path && vols[i].other && !vols[i].jam && vols[i].nvme == nvme &&
                   vols[i].test == test && vols[i].usbn == n;
        if (!used)
            return (int)n;
    }
    return -1;
}

/* A free slot (or a new one) for partition `part` of d; `other`:
 * d is not the boot disk, and the service gets a /usbN (an NVMe drive's:
 * /nvmeN). */
static struct vol *fs_slot(struct disk *d, unsigned part, bool other)
{
    struct vol *b = NULL;
    bool nvme = d->bus == DISKS_BUS_NVME;
    int usbn = other ? mount_number(d->test, nvme) : 0;
    if (usbn < 0)
        return NULL;
    for (unsigned i = 0; i < nvols && !b; i++)
        if (!vols[i].path && !vols[i].proc &&
            vols[i].state == VOLUMES_SUP_NONE)
            b = &vols[i];
    if (!b && nvols < MAX_VOLS)
        b = &vols[nvols++];
    if (!b)
        return NULL;
    uint32_t gen = b->gen, serial = b->serial, ctl_gen = b->ctl_gen;
    /* kept across uses: stale port packets stay stale, old tasks stopped */
    *b = (struct vol){ .path = FAT_PATH, .test = d->test, .gen = gen, .serial = serial + 1,
                       .ctl_gen = ctl_gen,
                       .disk = (uint32_t)(d - disks) + 1, .part = (uint8_t)part,
                       .other = other, .nvme = other && nvme, .usbn = (uint8_t)usbn,
                       .rw = other && (d->fresh >> part & 1) };   /* prepare.c: to be formatted */
    snprintf(b->name, sizeof(b->name), "fat-%s", fs_mount_path(b) + 1);
    d->fs[part] = (uint32_t)(b - vols) + 1;
    return b;
}

/* ---- which service a partition gets ------------------------------------------- */

/* May a partition of this MBR type hold a FAT volume? 00 is usb-storage's
 * "no table: the whole disk is one FAT volume". */
static bool fat_type(uint8_t type)
{
    return type == 0x00 || type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0b ||
           type == 0x0c || type == 0x0e || type == TYPE_ESP;
}

/* GPT type GUIDs as their bytes on disk (the first three fields little-endian). */
static const uint8_t guid_esp[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t guid_basic[16] = { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 };
static const uint8_t guid_jamos[16] = { 0x65, 0xe5, 0xba, 0x71, 0xa8, 0xd4, 0x15, 0x43,
                                        0x8c, 0x81, 0x9b, 0x35, 0x46, 0x2c, 0xc1, 0x58 };
static const uint8_t guid_linux[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

/* May a GPT partition of this type GUID (its bytes on disk) hold a FAT
 * volume? The ESP's type, Microsoft's basic data, which is also NTFS's and
 * exFAT's (fat looks at the boot sector and exits FAT_EXIT_NO_VOLUME having
 * read one block), and Jam OS's own data type. The MSR, recovery and
 * anything else are listed, never started. */
static bool fat_guid(const uint8_t *g)
{
    return !memcmp(g, guid_esp, 16) || !memcmp(g, guid_basic, 16) || !memcmp(g, guid_jamos, 16);
}

/* ... an ext volume? GPT's Linux filesystem type and Jam OS's own data
 * type (a partition of that type holds FAT or ext: probe.c looks). */
static bool ext_guid(const uint8_t *g)
{
    return !memcmp(g, guid_linux, 16) || !memcmp(g, guid_jamos, 16);
}

/* May partition `part` of d hold a FAT volume, by its type? ... an ext one? */
static bool part_fat(const struct disk *d, unsigned part)
{
    return d->scheme == STORAGE_SCHEME_GPT ? fat_guid(d->info[part].type_guid)
                                           : fat_type(d->type[part]);
}

static bool part_ext(const struct disk *d, unsigned part)
{
    return d->scheme == STORAGE_SCHEME_GPT ? ext_guid(d->info[part].type_guid)
                                           : d->type[part] == TYPE_LINUX;
}

/* Is another mount (of a test disk's if b's is) the Jam OS drive's /jam? */
static bool jam_taken(const struct vol *self)
{
    for (unsigned i = 0; i < nvols; i++)
        if (&vols[i] != self && vols[i].path && vols[i].jam && vols[i].test == self->test)
            return true;
    return false;
}

/* b's partition holds an ext volume: its service is bin/extfs, and where
 * and how it is mounted is decided (the rules in this file's header). The
 * Jam OS drive's data partition (an NVMe drive, by volumes' own mark
 * check, mark.c) is /jam, read-write unless its driver is read-only (the
 * `diskro` word); a test disk's ext volume is written too (utest drives
 * extfs through it); anyone else's never is. OK, or ERR_NOT_FOUND (said:
 * no bin/extfs), ERR_PEER_CLOSED (b went meanwhile). */
static status_t choose_ext(struct vol *b)
{
    struct disk *d = disk_of(b);
    if (!in_bootfs(EXTFS_PATH)) {
        say(false, "volumes: %s: %s is not in bootfs: no filesystem for its ext volume",
            disk_name(d), EXTFS_PATH);
        return ERR_NOT_FOUND;
    }
    b->ext = true;
    b->path = EXTFS_PATH;
    d->fstype[b->part] = VOLDISKS_FSTYPE_EXT;
    if (b->other) {
        uint32_t serial = b->serial;
        b->jam = b->part == PART_DATA && d->bus == DISKS_BUS_NVME && disk_jamos_gpt(d) &&
                 !jam_taken(b);
        b->dflt_rw = d->test;
        if (b->jam) {
            uint8_t mode, ro = 1, readable;
            status_t st = driver_mode(d, &mode, &ro, &readable);
            if (b->serial != serial || stopping)
                return ERR_PEER_CLOSED;
            b->dflt_rw = st == OK && !ro;
        }
        b->rw = b->rw || b->dflt_rw;
    }
    snprintf(b->name, sizeof(b->name), "extfs-%s", fs_mount_path(b) + 1);
    return OK;
}

/* In the start's task: which service b gets. A partition whose type can
 * hold ext, and the boot disk's data partition, have their superblock
 * looked at (probe.c); the others are FAT, as they always were. OK;
 * ERR_NOT_FOUND: nothing either service can read there (said): a Linux
 * type with no ext volume is left alone; ERR_PEER_CLOSED: b went
 * meanwhile. */
static status_t fs_choose(struct vol *b)
{
    struct disk *d = disk_of(b);
    unsigned part = b->part;
    bool boot_data = !b->other && part == PART_DATA;
    if (!d)
        return ERR_PEER_CLOSED;
    if (!boot_data && !part_ext(d, part)) {
        d->fstype[part] = VOLDISKS_FSTYPE_FAT;
        return OK;
    }
    bool is_ext = false;
    status_t st = fs_probe_ext(b, &is_ext);
    if (st == ERR_PEER_CLOSED)
        return st;
    if (st != OK)
        say(false, "volumes: %s partition %u: its first sectors can't be read (%s)", disk_name(d),
            part + 1, status_str(st));
    if (is_ext)
        return choose_ext(b);
    if (!boot_data && part_fat(d, part) && d->block_size && d->block_size != FAT_BLOCK) {
        d->badsize |= (uint16_t)(1u << part);   /* a FAT-or-ext type, and no ext: fat reads 512 only */
        say(false, "volumes: %s partition %u: %u-byte blocks: %s reads 512-byte blocks only: "
            "left alone", disk_name(d), part + 1, d->block_size, FAT_PATH);
        return ERR_NOT_FOUND;
    }
    if (boot_data || part_fat(d, part)) {
        d->fstype[part] = VOLDISKS_FSTYPE_FAT;
        return OK;
    }
    d->fstype[part] = VOLDISKS_FSTYPE_NONE;
    say(false, "volumes: %s partition %u (%s) holds no ext volume %s can read: left alone, "
        "nothing written to it", disk_name(d), part + 1,
        d->scheme == STORAGE_SCHEME_GPT ? "a Linux filesystem type" : "type 83", EXTFS_PATH);
    return ERR_NOT_FOUND;
}

/* fs_start's task: which service, the start, then what its caller wanted
 * done. */
static void start_main(void *arg)
{
    struct vol *b = arg;
    uint32_t serial = b->serial;
    struct disk *d = disk_of(b);
    unsigned part = b->part;
    bool other = b->other;
    void (*done)(struct disk *, unsigned, status_t) = b->on_started;
    status_t chose = fs_choose(b), st = chose;
    if (chose == OK)
        st = fs_run(b, false);
    b->last = st;
    if (b->serial != serial || stopping)
        return;   /* retired meanwhile: its disk moved on without it */
    b->starting = false;
    task_kick(tasks);   /* a kill may wait for this start (request.c) */
    if (chose == OK)
        say(false, "volumes: %s partition %u -> %s at %s%s (%s)", disk_name(d), part + 1,
            b->path, fs_mount_path(b), (other || part == PART_ESP) && !b->rw ? ", read-only" : "",
            status_str(st));
    if (st != OK)
        fs_retire(b);
    if (done)
        done(d, part, st);
}

status_t fs_start(struct disk *d, unsigned part, bool other,
                  void (*done)(struct disk *d, unsigned part, status_t st))
{
    if (!in_bootfs(FAT_PATH)) {
        say(false, "volumes: %s: %s is not in bootfs: no filesystem", disk_name(d), FAT_PATH);
        return ERR_NOT_FOUND;
    }
    struct vol *b = fs_slot(d, part, other);
    if (!b) {
        say(false, "volumes: %s partition %u: no room for another mount", disk_name(d), part + 1);
        return ERR_NO_RESOURCES;
    }
    b->starting = true;
    b->on_started = done;
    if (!task_start(tasks, start_main, b)) {
        say(false, "volumes: %s partition %u: no task for its start", disk_name(d), part + 1);
        fs_retire(b);
        return ERR_NO_RESOURCES;
    }
    return OK;
}

/* d moves to `state`, one in which nothing of it is mounted, and its
 * services go: killed (their `block` channels are dead, or nobody needs
 * them) and their bindings freed. The state first, so the mounts change
 * once, not once per service. */
/* The desktop's notice of another stick mounted: once a mount, not again
 * when `mount -w` or a crash restarts its service; a test disk's never. */
void disk_told_added(struct disk *d, const struct vol *b)
{
    unsigned bit = 1u << b->part;
    if (d->test || b->nvme || b->part >= MAX_PARTS || (d->told & bit))
        return;   /* an internal drive is no news at every boot */
    d->told |= (uint16_t)bit;
    snprintf(d->at[b->part], sizeof(d->at[0]), "%s", fs_mount_path(b));
    char body[96];
    if (d->mib >= 1024)
        snprintf(body, sizeof(body), "%lu.%lu GB at %s, %s", (unsigned long)(d->mib / 1024),
                 (unsigned long)(d->mib % 1024 * 10 / 1024), d->at[b->part],
                 b->rw ? "read-write" : "read-only");
    else
        snprintf(body, sizeof(body), "%lu MB at %s, %s", (unsigned long)d->mib, d->at[b->part],
                 b->rw ? "read-write" : "read-only");
    notice_post(&notices, "USB stick added", body, 'U', NOTICE_APRICOT);
}

/* The desktop's notice of the boot disk gone (pulled out: /data and /esp
 * with it) or back (came: it is the boot disk again after one went). Not
 * at a reboot, nor when devmgr ended (quiet), never a test disk's. */
void disk_told_boot(const struct disk *d, bool came)
{
    static bool gone;   /* the boot disk went and hasn't come back */
    if (d->test || stopping || quiet || came != gone)
        return;
    gone = !came;
    if (came)
        notice_post(&notices, "Jam OS stick is back", "Your files are saved again.", 'U',
                    NOTICE_APRICOT);
    else
        notice_post(&notices, "Jam OS stick removed",
                    "Nothing is saved until it's back.", 'U', NOTICE_RASPBERRY);
}

/* Its mounts the desktop was told of have gone with it. */
static void told_removed(struct disk *d)
{
    char body[96] = "";
    size_t o = 0;
    for (unsigned part = 0; part < MAX_PARTS; part++)
        if (d->told & (1u << part))
            o += (size_t)snprintf(body + o, o < sizeof(body) ? sizeof(body) - o : 0, "%s%s",
                                  o ? " and " : "", d->at[part]);
    d->told = 0;
    if (o && !stopping && !quiet)   /* a reboot, or devmgr ended: no news */
        notice_post(&notices, "USB stick removed", body, 'U', NOTICE_BLACKCURRANT);
}

void drop_services(struct disk *d, enum disk_state state)
{
    if (state == DISK_FREE || state == DISK_DOWN)
        told_removed(d);   /* pulled out, or its driver gone: not a remount */
    /* The boot disk gone: pulled out (its driver stops, DISK_DOWN, then
     * the disk is forgotten, DISK_FREE), not its driver restarting. */
    if (state == DISK_DOWN && d->state != DISK_DOWN)
        d->boot_down = d->state == DISK_BOOT;
    if (state == DISK_FREE && (d->state == DISK_BOOT || d->boot_down))
        disk_told_boot(d, false);
    if (state == DISK_FREE)
        d->boot_down = false;
    d->state = state;
    d->want = 0;
    for (unsigned part = 0; part < MAX_PARTS; part++) {
        if (!d->fs[part])
            continue;
        struct vol *b = &vols[d->fs[part] - 1];
        if (b->proc)
            fs_stop(b, true, true);
        fs_retire(b);
    }
    mounts_update();
}

/* A /usbN service's start went: one that failed lets the next one go. */
static void other_started(struct disk *d, unsigned part, status_t st)
{
    (void)d;
    (void)part;
    if (st != OK)
        others_pump();
}

/* Start the next /usbN service that is waited for, unless one is still
 * finding out whether its partition holds a volume. One at a time, so the
 * numbers go to the volumes in the order they were found, with no gap
 * where a partition turned out to hold none. */
void others_pump(void)
{
    for (unsigned i = 0; i < nvols; i++) {
        const struct vol *b = &vols[i];
        bool alive = b->state == VOLUMES_SUP_RUNNING || b->state == VOLUMES_SUP_RESTARTING;
        if (b->path && b->other && !b->ready && (alive || b->starting))
            return;
    }
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        struct disk *d = &disks[i];
        for (unsigned part = 0; part < MAX_PARTS && d->state == DISK_OTHER && !d->releasing;
             part++) {
            if (!(d->want & 1u << part))
                continue;
            d->want &= (uint16_t)~(1u << part);
            if (fs_start(d, part, true, other_started) == OK)
                return;
        }
    }
}

/* Step 4: d is not the boot disk. Each of its FAT and ext partitions gets
 * a filesystem service (read-only, but for an ext volume that is Jam OS's)
 * and a /usbN (others_pump starts them). */
void mount_others(struct disk *d)
{
    d->state = DISK_OTHER;
    d->want = d->nofat = d->badsize = 0;
    memset(d->fstype, 0, sizeof(d->fstype));
    for (unsigned i = 0; i < d->nparts && i < MAX_PARTS; i++) {
        bool fat = part_fat(d, i), ext = part_ext(d, i);
        if (!fat && !ext && d->scheme == STORAGE_SCHEME_GPT)
            say(false, "volumes: %s partition %u: its GPT type is not FAT: left alone",
                disk_name(d), i + 1);
        else if (!fat && !ext)
            say(false, "volumes: %s partition %u: type %02x is not FAT: left alone", disk_name(d),
                i + 1, d->type[i]);
        else if (!ext && d->block_size && d->block_size != FAT_BLOCK)
            d->badsize |= (uint16_t)(1u << i);   /* said below, once */
        else
            d->want |= (uint16_t)(1u << i);
    }
    if (d->badsize)
        say(false, "volumes: %s: %u-byte blocks: %s reads 512-byte blocks only: its FAT-typed "
            "partitions are left alone", disk_name(d), d->block_size, FAT_PATH);
    if (!d->want)
        say(false, "volumes: %s: no FAT partition to mount: left alone", disk_name(d));
    others_pump();
}

/* An fs.stat of `path` written to b's service without waiting for the
 * answer (fs_answers takes it). Returns its transaction id. */
uint32_t ask_stat(const struct vol *b, const char *path, status_t *st)
{
    txids = (txids + 1) & ~CHANNEL_TXID_KERNEL;
    if (txids == 0)
        txids++;
    *st = fs_stat_send(fs_asker(b), txids, path, (uint32_t)strlen(path));
    return txids;
}

bool fs_check_ended(struct vol *b, bool no_volume)
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
    say(false, "volumes: %s partition %u (type %02x) holds no %s volume %s can read: left alone, "
        "nothing written to it", disk_name(d), b->part + 1, d->type[b->part],
        b->ext ? "ext" : "FAT", b->path);
    problems += !sup_job_empty(b->job, b->path);
    d->nofat |= (uint16_t)(1u << b->part);   /* the listing says so */
    fs_forget(b);
    fs_retire(b);
    return true;
}

status_t fs_restart(struct vol *b, bool writable)
{
    uint64_t t0 = now();
    uint32_t serial = b->serial;
    char path[16];
    snprintf(path, sizeof(path), "%s", fs_mount_path(b));
    /* The service stops in order (fsctl.stop): its files closed, what was
     * written on the stick, the volume clean; a write that comes later
     * fails instead of being lost. Only one that doesn't answer is killed
     * (said, with what that loses). */
    status_t stopped = fs_stop_in_order(b);
    if (stopped == ERR_PEER_CLOSED || b->serial != serial || stopping)
        return ERR_PEER_CLOSED;   /* its disk went meanwhile, or volumes stops */
    b->state = VOLUMES_SUP_NONE;
    sup_reset(b);
    /* Stopped in order: it kept nothing, and the next one starts fresh on a
     * new `fs` channel (the old one's clients see ERR_PEER_CLOSED). */
    fs_kept_release(b);
    b->rw = writable;
    b->last = fs_run(b, false);   /* a new `block` channel, opened the new way */
    if (b->serial != serial || stopping)
        return ERR_PEER_CLOSED;
    status_t st = b->last;
    say(false, "volumes: %s: its filesystem service started again, %s (%s), %lu ms after "
        "the remount was asked for", path, writable ? "read-write" : "read-only",
        status_str(st), (unsigned long)((now() - t0) / NS_PER_MS));
    if (st != OK) {
        fs_retire(b);
        mounts_update();
    }
    return st;
}

status_t disk_can_restart(const struct vol *b, bool writable)
{
    if (b->busy || b->stopping || b->starting)
        return ERR_BAD_STATE;   /* something else is under way on it */
    if (b->state != VOLUMES_SUP_RUNNING || !b->proc || !b->client || (b->other && !b->ready))
        return ERR_BAD_STATE;   /* not serving: not mounted (yet, or no longer), restarting */
    return b->rw == writable ? OK : IDL_LATER;
}

struct vol *disk_esp(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state == DISK_BOOT && !disks[i].test && disks[i].fs[PART_ESP])
            return &vols[disks[i].fs[PART_ESP] - 1];
    return NULL;
}
