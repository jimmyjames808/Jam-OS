/* volumes: the answers to its protocols' requests (abi/idl/volumes.idl):
 * `volumes` on the control channels, `volesp` on init's ESP channel,
 * `voldisks` on the list channels, `volinstall` on init's install channel.
 * chans.c reads the requests and decides which protocol a channel speaks.
 * A mount is named by its path ("/data", "/usb0-test"): fs_find_mount.
 *
 * The `later` methods (remount, release_disk, sync, stop, volesp's two,
 * digest, health, prepare_check and volinstall's prepare) wait for
 * filesystem services and disks: each is checked here and
 * answered at once when there is nothing to wait for (an error, or the
 * mount is that way already); otherwise its handler starts a task (an op,
 * calls.c) and returns IDL_LATER, and the task answers when it is done.
 * The loop serves every other request meanwhile. A task answers on a
 * duplicate of the request's channel, so an opener's channel that goes
 * meanwhile (chans.c drops it once its clients are gone) is never written
 * through a handle that may name something else by then. */
#include "disk.h"

/* A `later` request under way. */
struct op {
    struct idl_txn txn;      /* where the answer goes: a duplicate of the channel (0: nowhere) */
    struct vol    *b;        /* remount, the ESP: its filesystem service */
    bool           writable; /* ... made writable (or read-only) */
    uint32_t       id;       /* release_disk, digest, health, the prepares, the installer's: the disk */
    unsigned       part;     /* the installer's: PART_ESP or PART_DATA */
    uint32_t       index;    /* open_partition: the partition's place in the table */
    uint64_t       lba, count;   /* digest: the blocks */
    char           tail[20], word[8];   /* prepare: the owner's confirmation of an erase;
                                         * format_data: the tail */
    bool           fat;      /* format_data: FAT32, not ext */
    uint32_t       tail_len, word_len;
};

/* Start fn(o) for the request txn: IDL_LATER (fn answers), or why not
 * (then nothing started: the handler's status is the answer). */
static status_t later(struct idl_txn txn, struct op *proto, void (*fn)(void *))
{
    struct op *o = malloc(sizeof(*o));
    if (!o)
        return ERR_NO_MEMORY;
    *o = *proto;
    o->txn.txid = txn.txid;
    o->txn.ch = HANDLE_INVALID;
    if (txn.ch && jam_handle_duplicate(txn.ch, RIGHT_SAME, &o->txn.ch) != OK) {
        free(o);
        return ERR_NO_RESOURCES;
    }
    if (!task_start(tasks, fn, o)) {   /* from the loop it runs at once, until it waits */
        if (o->txn.ch)
            jam_handle_close(o->txn.ch);
        free(o);
        return ERR_NO_RESOURCES;
    }
    return IDL_LATER;
}

/* The op is over: its channel's duplicate goes, and o. */
static void op_done(struct op *o)
{
    if (o->txn.ch)
        jam_handle_close(o->txn.ch);
    free(o);
}

/* ---- volumes ------------------------------------------------------------------------- */

static status_t v_watch(void *ctx, handle_t *out_events)
{
    (void)ctx;
    return mounts_watch(out_events);
}

/* A remount's task: the service stopped in order and started again. */
static void remount_main(void *arg)
{
    struct op *o = arg;
    struct vol *b = o->b;
    uint32_t serial = b->serial;
    b->busy = true;
    status_t st = fs_restart(b, o->writable);
    if (b->serial == serial)
        b->busy = false;
    if (o->txn.ch)
        (void)volumes_reply_remount(o->txn, st);
    op_done(o);
}

/* /usbN, /usbN-test, an NVMe drive's /nvmeN if its disk is Jam OS's, and
 * /jam: the mounts remount may change. An ext volume that isn't Jam OS's or
 * a test disk's is never made writable (Q9: someone else's volume is only
 * read). */
static status_t v_remount(void *ctx, struct idl_txn txn, const char *mount, uint32_t len,
                          uint8_t writable)
{
    (void)ctx;
    if (writable > 1)
        return ERR_INVALID_ARGS;
    struct vol *b = fs_find_mount(mount, len);
    if (!b || !b->other)
        return ERR_NOT_FOUND;   /* /esp and /data are what they are */
    if (b->held)
        return ERR_BUSY;   /* the installer's (install.c) */
    if (b->nvme && writable && (!disk_of(b) || !disk_jamos_gpt(disk_of(b))))
        return ERR_ACCESS_DENIED;   /* only on a Jam OS disk, by our own check (mark.c) */
    if (b->ext && writable && !b->dflt_rw)
        return ERR_ACCESS_DENIED;   /* someone else's ext volume: read-only always */
    status_t st = disk_can_restart(b, writable);
    if (st != IDL_LATER)
        return st;
    return later(txn, &(struct op){ .b = b, .writable = writable != 0 }, remount_main);
}

/* The filesystem service volumes.kill names: a mount's path ("/data"),
 * or a process name: a mount's service's own ("fat-data", whatever its
 * process is called now), or "fat-spare", the first service whose
 * process is the warm spare promoted (the name it keeps). */
static struct vol *kill_target(const char *name, uint32_t len)
{
    if (len && name[0] == '/')
        return fs_find_mount(name, len);
    for (unsigned i = 0; i < nvols; i++)
        if (vols[i].path && strlen(vols[i].name) == len && !memcmp(vols[i].name, name, len))
            return &vols[i];
    static const char spare[] = "fat-spare";
    if (len != sizeof(spare) - 1 || memcmp(name, spare, len))
        return NULL;
    for (unsigned i = 0; i < nvols; i++)
        if (vols[i].path && vols[i].proc && vols[i].spare_now)
            return &vols[i];
    return NULL;
}

/* A kill that came while its restart was under way: once the new
 * instance runs (or the restart failed), that one is killed. */
static void kill_main(void *arg)
{
    struct op *o = arg;
    struct vol *b = o->b;
    uint32_t serial = b->serial;
    uint64_t koid = 0;
    while (b->serial == serial && b->starting && !stopping)
        task_wait(tasks, DEADLINE_NEVER);   /* the restart's end kicks */
    status_t st = b->serial != serial || stopping ? ERR_NOT_FOUND
                : b->busy || b->stopping ? ERR_BAD_STATE : sup_kill(b, &koid);
    if (o->txn.ch)
        (void)volumes_reply_kill(o->txn, st, koid);
    op_done(o);
}

static status_t v_kill(void *ctx, struct idl_txn txn, const char *mount, uint32_t len,
                       uint64_t *out_koid)
{
    (void)ctx;
    struct vol *b = kill_target(mount, len);
    if (!b || (b->state == VOLUMES_SUP_NONE && !b->starting))
        return ERR_NOT_FOUND;
    if (b->busy || b->stopping)
        return ERR_BAD_STATE;   /* stopping in order: a remount, an ESP write, a release */
    if (b->starting)
        return later(txn, &(struct op){ .b = b }, kill_main);
    return sup_kill(b, out_koid);
}

static status_t v_supervision(void *ctx, const char *mount, uint32_t len, uint32_t *out_state,
                              uint32_t *out_restarts, uint32_t *out_backoff_ms,
                              uint32_t *out_promoted, uint8_t *out_spare_waits)
{
    (void)ctx;
    const struct vol *b = fs_find_mount(mount, len);
    if (!b)
        return ERR_NOT_FOUND;
    *out_state = b->state;
    *out_restarts = b->win.restarts;
    *out_backoff_ms = b->win.backoff_ms;
    *out_promoted = b->promoted;
    *out_spare_waits = spare_waits();
    return OK;
}

static status_t v_test_disk(void *ctx, handle_t storage, uint32_t *out_id)
{
    (void)ctx;
    return disk_test(storage, out_id);
}

static void release_main(void *arg)
{
    struct op *o = arg;
    status_t st = disk_release(o->id);
    if (o->txn.ch)
        (void)volumes_reply_release_disk(o->txn, st);
    op_done(o);
}

static status_t v_release_disk(void *ctx, struct idl_txn txn, uint32_t id)
{
    (void)ctx;
    if (!disk_exists(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id }, release_main);
}

static void sync_main(void *arg)
{
    struct op *o = arg;
    status_t st = disk_sync_all();
    if (o->txn.ch)
        (void)volumes_reply_sync(o->txn, st);
    op_done(o);
}

static status_t v_sync(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    return later(txn, &(struct op){ 0 }, sync_main);
}

/* ---- the stop ------------------------------------------------------------------------ */

static bool stop_ok = true;   /* stop_all's, once the stop's task is done */
static bool stop_begun;

/* The stop's task: every writable mount synced while its disk still
 * works, then everything stopped (stop_all), then the answer. */
static void stop_main(void *arg)
{
    struct op *o = arg;
    (void)disk_sync_all();   /* what failed is said */
    stop_ok = stop_all();
    if (o->txn.ch)
        (void)volumes_reply_stop(o->txn, OK);
    op_done(o);
}

bool stop_start(struct idl_txn txn)
{
    if (stop_begun)
        return true;
    stop_begun = true;
    stopping = true;   /* nothing more is served, nothing restarts */
    if (later(txn, &(struct op){ 0 }, stop_main) == IDL_LATER)
        return true;
    say(true, "volumes: no task for the stop: the mounts are not synced");
    stop_ok = stop_all();
    return false;
}

bool stop_result(void)
{
    return stop_ok;
}

static status_t v_stop(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    if (stop_begun)
        return OK;   /* under way already */
    return stop_start(txn) ? IDL_LATER : OK;
}

static status_t v_set_disks(void *ctx, handle_t disks)
{
    (void)ctx;
    disks_set(disks);
    return OK;
}

static status_t v_list_channel(void *ctx, handle_t *out_list)
{
    (void)ctx;
    return chans_add_list(out_list);
}

const struct volumes_ops volumes_ops = {
    .watch = v_watch,
    .remount = v_remount,
    .kill = v_kill,
    .supervision = v_supervision,
    .test_disk = v_test_disk,
    .release_disk = v_release_disk,
    .sync = v_sync,
    .stop = v_stop,
    .set_disks = v_set_disks,
    .list_channel = v_list_channel,
};

/* ---- voldisks ------------------------------------------------------------------------- */

static status_t l_disk(void *ctx, uint32_t index, uint32_t *out_id, uint8_t *out_bus,
                       uint8_t *out_ready, uint8_t *out_kind, uint8_t *out_scheme,
                       uint8_t *out_read_only, uint32_t *out_block_size, uint64_t *out_blocks,
                       uint32_t *out_partitions, char *out_name, uint32_t *out_name_len,
                       char *out_model, uint32_t *out_model_len, char *out_serial,
                       uint32_t *out_serial_len, char *out_firmware, uint32_t *out_firmware_len)
{
    (void)ctx;
    struct list_disk o;
    status_t st = list_disk(index, &o);
    if (st != OK)
        return st;
    *out_id = o.id;
    *out_bus = o.bus;
    *out_ready = o.ready;
    *out_kind = o.kind;
    *out_scheme = o.scheme;
    *out_read_only = o.read_only;
    *out_block_size = o.block_size;
    *out_blocks = o.blocks;
    *out_partitions = o.partitions;
    memcpy(out_name, o.name, o.name_len);
    *out_name_len = o.name_len;
    memcpy(out_model, o.model, o.model_len);
    *out_model_len = o.model_len;
    memcpy(out_serial, o.serial, o.serial_len);
    *out_serial_len = o.serial_len;
    memcpy(out_firmware, o.firmware, o.firmware_len);
    *out_firmware_len = o.firmware_len;
    return OK;
}

static status_t l_partition(void *ctx, uint32_t id, uint32_t index, uint8_t *out_mbr_type,
                            uint8_t out_type_guid[16], uint64_t *out_start, uint64_t *out_blocks,
                            uint8_t *out_fs, uint8_t *out_fstype, char *out_label,
                            uint32_t *out_label_len, char *out_mount, uint32_t *out_mount_len)
{
    (void)ctx;
    struct list_part o;
    status_t st = list_part(id, index, &o);
    if (st != OK)
        return st;
    *out_mbr_type = o.mbr_type;
    memcpy(out_type_guid, o.type_guid, sizeof(o.type_guid));
    *out_start = o.start;
    *out_blocks = o.blocks;
    *out_fs = o.fs;
    *out_fstype = o.fstype;
    memcpy(out_label, o.label, o.label_len);
    *out_label_len = o.label_len;
    memcpy(out_mount, o.mount, o.mount_len);
    *out_mount_len = o.mount_len;
    return OK;
}

/* A digest's task: asked of the driver (in the task, so the loop goes on),
 * then answered. */
static void digest_main(void *arg)
{
    struct op *o = arg;
    uint8_t sha[32] = { 0 };
    status_t st = disk_digest(o->id, o->lba, o->count, sha);
    if (o->txn.ch)
        (void)voldisks_reply_digest(o->txn, st, sha);
    op_done(o);
}

static status_t l_digest(void *ctx, struct idl_txn txn, uint32_t id, uint64_t lba, uint64_t count,
                         uint8_t out_sha256[32])
{
    (void)ctx;
    (void)out_sha256;   /* answered later */
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id, .lba = lba, .count = count }, digest_main);
}

static void health_main(void *arg)
{
    struct op *o = arg;
    struct list_health h = { 0 };
    status_t st = disk_health(o->id, &h);
    if (o->txn.ch)
        (void)voldisks_reply_health(o->txn, st, h.units_read, h.units_written, h.host_reads,
                                    h.host_writes, h.power_cycles, h.unsafe_shutdowns,
                                    h.percent_used, h.critical);
    op_done(o);
}

static status_t l_health(void *ctx, struct idl_txn txn, uint32_t id, uint64_t *out_units_read,
                         uint64_t *out_units_written, uint64_t *out_host_reads,
                         uint64_t *out_host_writes, uint64_t *out_power_cycles,
                         uint64_t *out_unsafe_shutdowns, uint8_t *out_percent_used,
                         uint8_t *out_critical)
{
    (void)ctx;
    (void)out_units_read, (void)out_units_written, (void)out_host_reads, (void)out_host_writes;
    (void)out_power_cycles, (void)out_unsafe_shutdowns, (void)out_percent_used, (void)out_critical;
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id }, health_main);
}

static void check_main(void *arg)
{
    struct op *o = arg;
    uint8_t plan = 0, refused = 0;
    status_t st = disk_prepare_check(o->id, &plan, &refused);
    if (o->txn.ch)
        (void)voldisks_reply_prepare_check(o->txn, st, plan, refused);
    op_done(o);
}

static status_t l_prepare_check(void *ctx, struct idl_txn txn, uint32_t id, uint8_t *out_plan,
                                uint8_t *out_refused)
{
    (void)ctx;
    (void)out_plan, (void)out_refused;   /* answered later: the driver is asked */
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id }, check_main);
}

/* A read-only block channel on a partition, asked of the driver in a task. */
static void open_main(void *arg)
{
    struct op *o = arg;
    handle_t blk = HANDLE_INVALID;
    status_t st = disk_open_ro(o->id, o->index, &blk);
    if (o->txn.ch)
        (void)voldisks_reply_open_partition(o->txn, st, blk);   /* takes the handle in any case */
    else if (blk != HANDLE_INVALID)
        jam_handle_close(blk);   /* nobody to give it to */
    op_done(o);
}

static status_t l_open_partition(void *ctx, struct idl_txn txn, uint32_t id, uint32_t index,
                                 handle_t *out_block)
{
    (void)ctx;
    (void)out_block;   /* answered later */
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id, .index = index }, open_main);
}

const struct voldisks_ops voldisks_ops = {
    .disk = l_disk, .partition = l_partition, .digest = l_digest, .health = l_health,
    .prepare_check = l_prepare_check, .open_partition = l_open_partition,
};

/* ---- volinstall ------------------------------------------------------------------------- */

/* A prepare's task (prepare.c does the work, waiting where it must). */
static void prepare_main(void *arg)
{
    struct op *o = arg;
    status_t st = disk_prepare(o->id, o->tail, o->tail_len, o->word, o->word_len);
    if (o->txn.ch)
        (void)volinstall_reply_prepare(o->txn, st);
    op_done(o);
}

static status_t i_prepare(void *ctx, struct idl_txn txn, uint32_t id, const char *tail,
                          uint32_t tail_len, const char *word, uint32_t word_len)
{
    (void)ctx;
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    struct op o = { .id = id, .tail_len = tail_len, .word_len = word_len };
    if (tail_len > sizeof(o.tail) || word_len > sizeof(o.word))
        return ERR_INVALID_ARGS;   /* past the IDL's bounds: the dispatcher refuses first */
    memcpy(o.tail, tail, tail_len);
    memcpy(o.word, word, word_len);
    return later(txn, &o, prepare_main);
}

/* The installer's task (install.c): partition `part` of disk `id` made
 * writable for init (and a channel to it), or read-only again. */
static void hold_main(void *arg)
{
    struct op *o = arg;
    handle_t fs = HANDLE_INVALID;
    status_t st = disk_install_hold(o->id, o->part, o->writable, o->writable ? &fs : NULL);
    if (!o->txn.ch) {
        if (fs)
            jam_handle_close(fs);
    } else if (o->part == PART_ESP) {
        if (o->writable)
            (void)volinstall_reply_open_esp(o->txn, st, fs);   /* fs goes either way */
        else
            (void)volinstall_reply_close_esp(o->txn, st);
    } else if (o->writable) {
        (void)volinstall_reply_open_data(o->txn, st, fs);
    } else {
        (void)volinstall_reply_close_data(o->txn, st);
    }
    op_done(o);
}

static status_t hold_start(struct idl_txn txn, uint32_t id, unsigned part, bool writable)
{
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    return later(txn, &(struct op){ .id = id, .part = part, .writable = writable }, hold_main);
}

static status_t i_open_esp(void *ctx, struct idl_txn txn, uint32_t id, handle_t *out_fs)
{
    (void)ctx, (void)out_fs;
    return hold_start(txn, id, PART_ESP, true);
}

static status_t i_close_esp(void *ctx, struct idl_txn txn, uint32_t id)
{
    (void)ctx;
    return hold_start(txn, id, PART_ESP, false);
}

static status_t i_open_data(void *ctx, struct idl_txn txn, uint32_t id, handle_t *out_fs)
{
    (void)ctx, (void)out_fs;
    return hold_start(txn, id, PART_DATA, true);
}

static status_t i_close_data(void *ctx, struct idl_txn txn, uint32_t id)
{
    (void)ctx;
    return hold_start(txn, id, PART_DATA, false);
}

/* A format's task: the data partition formatted again (prepare.c). */
static void format_main(void *arg)
{
    struct op *o = arg;
    status_t st = disk_format_data(o->id, o->tail, o->tail_len, o->fat);
    if (o->txn.ch)
        (void)volinstall_reply_format_data(o->txn, st);
    op_done(o);
}

static status_t i_format_data(void *ctx, struct idl_txn txn, uint32_t id, const char *tail,
                              uint32_t tail_len, uint8_t fat)
{
    (void)ctx;
    if (!disk_named(id))
        return ERR_NOT_FOUND;
    struct op o = { .id = id, .tail_len = tail_len, .fat = fat != 0 };
    if (tail_len > sizeof(o.tail) || fat > 1)
        return ERR_INVALID_ARGS;
    memcpy(o.tail, tail, tail_len);
    return later(txn, &o, format_main);
}

const struct volinstall_ops volinstall_ops = {
    .prepare = i_prepare, .open_esp = i_open_esp, .close_esp = i_close_esp,
    .open_data = i_open_data, .close_data = i_close_data, .format_data = i_format_data,
};

/* ---- volesp ------------------------------------------------------------------------------ */

/* The ESP's task: read-write (and a channel to it for init) or read-only
 * again. */
static void esp_main(void *arg)
{
    struct op *o = arg;
    struct vol *b = o->b;
    uint32_t serial = b->serial;
    b->busy = true;
    status_t st = fs_restart(b, o->writable);
    if (b->serial == serial)
        b->busy = false;
    if (o->writable) {
        handle_t fs = HANDLE_INVALID;
        if (st == OK && (!b->client || jam_handle_duplicate(b->client, RIGHT_SAME, &fs) != OK))
            st = ERR_BAD_STATE;
        if (o->txn.ch)
            (void)volesp_reply_open_writable(o->txn, st, fs);   /* fs goes either way */
        else if (fs)
            jam_handle_close(fs);
    } else if (o->txn.ch) {
        (void)volesp_reply_close_writable(o->txn, st);
    }
    op_done(o);
}

/* volesp's two: OK at once if the ESP is that way already (*out_fs, when
 * it is writable), IDL_LATER if its task answers, or why not. */
static status_t esp_write(struct idl_txn txn, bool writable, handle_t *out_fs)
{
    struct vol *b = disk_esp();
    if (!b)
        return ERR_NOT_FOUND;
    status_t st = disk_can_restart(b, writable);
    if (st == IDL_LATER)
        return later(txn, &(struct op){ .b = b, .writable = writable }, esp_main);
    if (st == OK && writable)
        st = jam_handle_duplicate(b->client, RIGHT_SAME, out_fs);
    return st;
}

static status_t e_open(void *ctx, struct idl_txn txn, handle_t *out_fs)
{
    (void)ctx;
    return esp_write(txn, true, out_fs);
}

static status_t e_close(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    return esp_write(txn, false, NULL);
}

const struct volesp_ops volesp_ops = {
    .open_writable = e_open,
    .close_writable = e_close,
};
