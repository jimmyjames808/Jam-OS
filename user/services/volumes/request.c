/* volumes: the answers to its protocols' requests (abi/idl/volumes.idl):
 * `volumes` on the control channels, `volesp` on init's ESP channel.
 * chans.c reads the requests and decides which protocol a channel speaks.
 * A mount is named by its path ("/data", "/usb0-test"): fs_find_mount.
 *
 * The `later` methods (remount, release_disk, sync, stop, and volesp's
 * two) are answered by their handlers before they return, for now: each
 * waits inside the request for the filesystem services it stops or syncs
 * (bounded: SYNC_WAIT, STOP_WAIT). */
#include "disk.h"

/* ---- volumes ------------------------------------------------------------------------- */

static status_t v_watch(void *ctx, handle_t *out_events)
{
    (void)ctx;
    return mounts_watch(out_events);
}

/* /usbN or /usbN-test: the mounts remount may change. */
static status_t v_remount(void *ctx, struct idl_txn txn, const char *mount, uint32_t len,
                          uint8_t writable)
{
    (void)ctx;
    (void)txn;
    if (writable > 1)
        return ERR_INVALID_ARGS;
    struct vol *b = fs_find_mount(mount, len);
    if (!b || !b->other)
        return ERR_NOT_FOUND;   /* /esp and /data are what they are */
    return disk_remount(b, writable);
}

static status_t v_kill(void *ctx, const char *mount, uint32_t len, uint64_t *out_koid)
{
    (void)ctx;
    struct vol *b = fs_find_mount(mount, len);
    if (!b || b->state == VOLUMES_SUP_NONE)
        return ERR_NOT_FOUND;
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

static status_t v_release_disk(void *ctx, struct idl_txn txn, uint32_t id)
{
    (void)ctx;
    (void)txn;
    return disk_release(id);
}

static status_t v_sync(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    (void)txn;
    return disk_sync_all();
}

static status_t v_stop(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    (void)txn;
    (void)stop_all();   /* what went wrong is said, and is the exit code's */
    return OK;
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

static status_t e_open(void *ctx, struct idl_txn txn, handle_t *out_fs)
{
    (void)ctx;
    (void)txn;
    return disk_esp_write(true, out_fs);
}

static status_t e_close(void *ctx, struct idl_txn txn)
{
    (void)ctx;
    (void)txn;
    return disk_esp_write(false, NULL);
}

const struct volesp_ops volesp_ops = {
    .open_writable = e_open,
    .close_writable = e_close,
};
