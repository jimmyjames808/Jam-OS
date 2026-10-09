/* volumes: the answers to its protocols' requests (abi/idl/volumes.idl):
 * `volumes` on the control channels, `volesp` on init's ESP channel.
 * chans.c reads the requests and decides which protocol a channel speaks.
 * A mount is named by its path ("/data", "/usb0-test"): fs_find_mount.
 *
 * The `later` methods (remount, release_disk, sync, stop, and volesp's
 * two) wait for filesystem services and disks: each is checked here and
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
    uint32_t       id;       /* release_disk: the disk */
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

/* /usbN or /usbN-test: the mounts remount may change. */
static status_t v_remount(void *ctx, struct idl_txn txn, const char *mount, uint32_t len,
                          uint8_t writable)
{
    (void)ctx;
    if (writable > 1)
        return ERR_INVALID_ARGS;
    struct vol *b = fs_find_mount(mount, len);
    if (!b || !b->other)
        return ERR_NOT_FOUND;   /* /esp and /data are what they are */
    status_t st = disk_can_restart(b, writable);
    if (st != IDL_LATER)
        return st;
    return later(txn, &(struct op){ .b = b, .writable = writable != 0 }, remount_main);
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
    struct vol *b = fs_find_mount(mount, len);
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
