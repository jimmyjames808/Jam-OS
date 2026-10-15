/* volumes: the installer's writable partitions (volinstall.open_esp,
 * close_esp, open_data and close_data, abi/idl/volumes.idl; M12.7 stage 3,
 * docs/M12.7-PLAN.md "Stage 3: the installer and the boot path"). init
 * alone asks, on its install channel, to write the ESP and the data
 * partition of a Jam OS disk it is installing on, as it asks the boot
 * stick's ESP for `update` (volesp):
 *   - the disk must be Jam OS's by volumes' own check (mark.c), not the
 *     boot disk, with both partitions mounted and settled, and its driver
 *     not read-only (`diskro`; asked: storage.mode);
 *   - the partition's service stops in order and starts again on a `block`
 *     channel opened read-write (fs_restart), and `held` is set: it is no
 *     mount for anyone (disk_mounts leaves it out, every watch is told
 *     `unmounted`), so the writable channel is init's alone and never in a
 *     namespace;
 *   - at the end, failed or not, init asks for it read-only again (the
 *     service stops in order, the volume clean) and it is a mount again.
 * A partition `mount -w` made writable for everyone is not held: ERR_BUSY.
 * The driver still refuses every write outside Jam OS's table and
 * partitions, and everything on a disk that isn't Jam OS's: nothing here
 * decides what may be written. */
#include "disk.h"

#define INSTALL_PARTS 2u   /* a Jam OS disk's partitions: PART_ESP, PART_DATA */
#define SETTLE_WAIT   (20 * NS_PER_S)   /* both partitions mounted and settled */
#define STEP          (100 * NS_PER_MS)

/* Is d, as the task found it, still the same: its driver the same,
 * volumes not stopping? */
static bool same(const struct disk *d, uint16_t gen)
{
    return !stopping && d->state == DISK_OTHER && d->gen == gen && d->ch;
}

/* Hold b writable (or find it held), a channel to it into *fs. */
static status_t hold_it(struct disk *d, uint16_t gen, struct vol *b, handle_t *fs)
{
    if (b->rw && !b->held)
        return ERR_BUSY;   /* `mount -w`'s: in the namespaces */
    status_t st = disk_can_restart(b, true);
    if (st == IDL_LATER) {
        uint32_t serial = b->serial;
        b->busy = true;
        st = fs_restart(b, true);
        if (b->serial != serial)
            return ERR_BAD_STATE;   /* retired meanwhile */
        b->busy = false;
        if (st == OK && !same(d, gen))
            st = ERR_BAD_STATE;
        if (st != OK) {
            say(true, "volumes: %s: the installer's writable open: %s", fs_mount_path(b),
                status_str(st));
            return st;
        }
        b->held = true;
        mounts_update();   /* no mount while it is held */
        say(false, "volumes: %s is the installer's, read-write, and no mount", fs_mount_path(b));
        st = OK;
    }
    if (st != OK)
        return st;
    b->held = true;
    return b->client ? jam_handle_duplicate(b->client, RIGHT_SAME, fs) : ERR_BAD_STATE;
}

/* Back to read-only and a mount. */
static status_t let_go(struct disk *d, uint16_t gen, struct vol *b)
{
    if (!b->rw) {
        b->held = false;
        return OK;
    }
    status_t st = disk_can_restart(b, false);
    if (st == IDL_LATER) {
        uint32_t serial = b->serial;
        b->busy = true;
        st = fs_restart(b, false);
        if (b->serial != serial)
            return ERR_BAD_STATE;
        b->busy = false;
        b->held = false;
        if (st == OK && !same(d, gen))
            st = ERR_BAD_STATE;
        mounts_update();
        say(false, "volumes: %s is read-only and a mount again (%s)", fs_mount_path(b),
            status_str(st));
        return st;
    }
    if (st == OK)
        b->held = false;
    return st;
}

status_t disk_install_hold(uint32_t id, unsigned part, bool hold, handle_t *fs)
{
    struct disk *d = disk_named(id);
    if (!d)
        return ERR_NOT_FOUND;
    if (d->state == DISK_BOOT || (d->state == DISK_DOWN && d->boot_down))
        return ERR_ACCESS_DENIED;   /* what Jam OS runs from */
    if (d->state != DISK_OTHER || !d->ch || d->preparing || d->releasing || part >= INSTALL_PARTS)
        return ERR_BAD_STATE;
    if (!disk_jamos_gpt(d))
        return ERR_ACCESS_DENIED;   /* by our own check, whatever the driver says */
    uint16_t gen = d->gen;
    /* Both mounted and settled: a partition made read-only again a moment ago
     * (the last close) is still answering its first probe. */
    for (uint64_t until = now() + SETTLE_WAIT;; task_wait(tasks, now() + STEP)) {
        bool ready = true;
        for (unsigned p = 0; p < INSTALL_PARTS; p++)
            ready &= d->fs[p] && vols[d->fs[p] - 1].ready && !vols[d->fs[p] - 1].starting;
        if (ready)
            break;
        if (!same(d, gen) || now() >= until)
            return ERR_BAD_STATE;
    }
    d->preparing = true;   /* nothing else (a prepare, a release) begins on it meanwhile */
    uint8_t mode, ro = 1, readable;
    status_t st = hold ? driver_mode(d, &mode, &ro, &readable) : OK;
    d = disk_named(id);
    if (!d || !same(d, gen)) {
        if (d && d->gen == gen)
            d->preparing = false;
        return ERR_BAD_STATE;   /* gone or changed meanwhile */
    }
    if (st == OK && hold && ro)
        st = ERR_ACCESS_DENIED;   /* the `diskro` word */
    if (st == OK) {
        struct vol *b = &vols[d->fs[part] - 1];
        uint32_t serial = b->serial;
        st = hold ? hold_it(d, gen, b, fs) : let_go(d, gen, b);
        d = b->serial == serial ? disk_named(id) : NULL;
    }
    if (d)
        d->preparing = false;
    return st;
}
