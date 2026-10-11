/* volumes: making an NVMe drive a Jam OS disk (volinstall.prepare,
 * abi/idl/volumes.idl; M12.7 stage 2, docs/M12.7-PLAN.md "Preparing a blank
 * disk"). One task, disk_prepare, for the whole of it, so the loop serves
 * everything else meanwhile:
 *   1. the checks: an NVMe drive with 512-byte blocks that is settled
 *      (DISK_OTHER) and that nothing else is doing anything to; and what it
 *      is: no partition table found on it (a blank drive, or a prepare cut
 *      short before its table was whole: the driver decides which, and
 *      refuses a drive that is neither), or Jam OS's by our own check
 *      (mark.c) with a partition that has no service (a prepare cut short
 *      after the table). Anything else is ERR_ACCESS_DENIED: nothing is
 *      asked of the driver;
 *   2. a blank drive: three random GUIDs and storage.create_table, which
 *      writes the table, reads it back and answers once the drive reads as
 *      Jam OS's (drivers/nvme/table.c). The driver refuses anything but a
 *      blank drive with ERR_ACCESS_DENIED, whatever volumes thought;
 *   3. the disk is read again (disk_relearn: storage.info, .partition, a
 *      Jam OS disk by our check now) and its two partitions are started
 *      like any other disk's (fsvc.c's mount_others), except that each is
 *      opened WRITABLE and given FAT_ARG_FORMAT (`fresh`, fs_format_arg):
 *      fat formats a partition whose first sector holds no boot signature,
 *      and only then, so a partition that already holds a volume is mounted
 *      as it is. `fresh` is set here and nowhere else;
 *   4. once both are mounted, each is stopped in order and started again
 *      read-only (fs_restart), as `mount -r` does: a prepare leaves the
 *      mounts read-only, like every other disk's, and `mount -w` makes one
 *      writable. The answer is OK when both are mounted again.
 * A cut at any point leaves the drive blank, blank-in-progress (the driver
 * says so: <jam/parttable.h>), or a Jam OS disk with a partition that has no
 * volume yet: the next prepare finishes it. */
#include <idl/storage.h>
#include "disk.h"

#define TABLE_WAIT  (60 * NS_PER_S)    /* create_table: its writes and the read-back */
#define MOUNT_WAIT  (600 * NS_PER_S)   /* the disk read again, both partitions formatted, mounted */
#define BACK_WAIT   (60 * NS_PER_S)    /* both mounted again, read-only */
#define STEP        (100 * NS_PER_MS)
#define PARTS       2u                 /* a Jam OS disk's partitions */
#define BOTH        ((1u << PARTS) - 1)

/* A random (version 4) GUID, as its 16 bytes on disk: the version nibble
 * is the top of the seventh byte, the variant the top of the ninth. */
static void new_guid(uint8_t g[16])
{
    os_random(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0f) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);
}

/* d is as the task found it: its driver the same, volumes not stopping. */
static bool still(const struct disk *d, uint16_t gen)
{
    return !stopping && d->state != DISK_FREE && d->gen == gen && d->ch &&
           d->state != DISK_DOWN;
}

/* The partitions of d (`mask`) have settled: nothing waits for a service
 * to start, and each one that has a service has mounted its volume. */
static bool settled(const struct disk *d, unsigned mask)
{
    if (d->state != DISK_OTHER || d->want)
        return false;
    for (unsigned p = 0; p < PARTS; p++)
        if ((mask >> p & 1) && d->fs[p] && !vols[d->fs[p] - 1].ready)
            return false;
    return true;
}

/* Wait until the partitions in mask have settled (OK), the disk has gone
 * (ERR_BAD_STATE), its driver gave up on answering (ERR_IO) or deadline
 * (ERR_TIMED_OUT). */
static status_t wait_settled(const struct disk *d, uint16_t gen, unsigned mask, uint64_t deadline)
{
    while (now() < deadline) {
        if (!still(d, gen))
            return ERR_BAD_STATE;
        if (d->state == DISK_ALONE)
            return ERR_IO;
        if (settled(d, mask))
            return OK;
        task_wait(tasks, now() + STEP);
    }
    return ERR_TIMED_OUT;
}

/* Which partitions have no service yet (bits), when d is Jam OS's already. */
static unsigned unmounted(const struct disk *d)
{
    unsigned todo = 0;
    for (unsigned p = 0; p < PARTS; p++)
        if (!d->fs[p])
            todo |= 1u << p;
    return todo;
}

/* The checks (step 1). OK with *todo the partitions to format (a blank
 * drive: BOTH, once its table is made); *table: its table is to be made. */
static status_t check(const struct disk *d, unsigned *todo, bool *table)
{
    if (!d)
        return ERR_NOT_FOUND;
    if (d->bus != DISKS_BUS_NVME)
        return ERR_NOT_SUPPORTED;
    if (!d->known || !d->ch || d->state != DISK_OTHER || d->preparing || d->releasing)
        return ERR_BAD_STATE;
    if (d->block_size != FAT_BLOCK)
        return ERR_NOT_SUPPORTED;   /* fat reads 512-byte blocks only: nothing would mount */
    *table = d->scheme == STORAGE_SCHEME_NONE && d->partitions == 0;
    if (*table) {
        *todo = BOTH;
        return OK;
    }
    if (!disk_jamos_gpt(d))
        return ERR_ACCESS_DENIED;   /* someone else's: not blank, not Jam OS's */
    *todo = unmounted(d);
    if (*todo)
        return OK;
    for (unsigned p = 0; p < PARTS; p++)
        if (!vols[d->fs[p] - 1].ready)
            return ERR_BAD_STATE;   /* still starting: ask again in a moment */
    return ERR_ALREADY_EXISTS;
}

/* Step 2: Jam OS's table onto a blank drive. */
static status_t make_table(struct disk *d)
{
    uint8_t disk_guid[16], esp_guid[16], data_guid[16];
    new_guid(disk_guid);
    new_guid(esp_guid);
    new_guid(data_guid);
    struct call c;
    call_begin(&c, d->ch);
    status_t st = storage_create_table_send(d->ch, c.txid, disk_guid, esp_guid, data_guid);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + TABLE_WAIT);
    return st == OK ? storage_create_table_result(c.rep, &c.m) : st;
}

/* Step 4: the partitions in mask, read-write since their format, stopped in
 * order and started again read-only. */
static status_t back_to_read_only(struct disk *d, uint16_t gen, unsigned mask)
{
    for (unsigned p = 0; p < PARTS; p++) {
        if (!(mask >> p & 1) || !d->fs[p])
            continue;
        d->fresh &= (uint16_t)~(1u << p);
        struct vol *b = &vols[d->fs[p] - 1];
        uint32_t serial = b->serial;
        uint64_t until = now() + BACK_WAIT;
        status_t st = disk_can_restart(b, false);
        while (st == ERR_BAD_STATE && now() < until && still(d, gen) && b->serial == serial) {
            task_wait(tasks, now() + STEP);   /* its start is not over yet */
            st = disk_can_restart(b, false);
        }
        if (b->serial != serial)
            return ERR_BAD_STATE;   /* retired meanwhile */
        if (st == OK)
            continue;   /* read-only already */
        if (st != IDL_LATER)
            return st;
        b->busy = true;
        st = fs_restart(b, false);
        if (b->serial == serial)
            b->busy = false;
        if (st != OK)
            return st;
    }
    return OK;
}

/* Steps 2 to 4 on d, which the checks passed. */
static status_t steps(struct disk *d, uint16_t gen, unsigned todo, bool table)
{
    status_t st;
    if (table) {
        st = make_table(d);
        if (st != OK) {
            say(true, "volumes: %s: prepare: the driver's create_table: %s", disk_name(d),
                status_str(st));
            return st;
        }
        say(false, "volumes: %s: Jam OS's table is on the drive: reading it again", disk_name(d));
        d->fresh = BOTH;
        disk_relearn(d);
    } else {
        d->fresh |= (uint16_t)todo;   /* a Jam OS disk's partitions with no volume yet */
        d->nofat &= (uint16_t)~todo;
        d->want |= (uint16_t)todo;
        others_pump();
    }
    st = wait_settled(d, gen, todo, now() + MOUNT_WAIT);
    if (st == OK && !disk_jamos_gpt(d))
        st = ERR_IO;   /* what the driver wrote doesn't read as Jam OS's (volumes' check) */
    for (unsigned p = 0; p < PARTS && st == OK; p++)
        if ((todo >> p & 1) && !d->fs[p]) {
            say(true, "volumes: %s partition %u: not mounted after its format", disk_name(d),
                p + 1);
            st = ERR_IO;
        }
    if (st == OK)
        st = back_to_read_only(d, gen, todo);
    if (st == OK)
        st = wait_settled(d, gen, BOTH, now() + BACK_WAIT);
    return st;
}

status_t disk_prepare(uint32_t id)
{
    struct disk *d = disk_named(id);
    unsigned todo = 0;
    bool table = false;
    status_t st = check(d, &todo, &table);
    if (st != OK)
        return st;
    uint16_t gen = d->gen;
    d->preparing = true;
    st = steps(d, gen, todo, table);
    d = disk_named(id);   /* its slot may have been given up and used again meanwhile */
    if (d) {
        d->preparing = false;
        d->fresh = 0;
    }
    if (st == OK && d) {
        char esp[16] = "?", data[16] = "?";   /* fs_mount_path's buffer is static */
        if (d->fs[0])
            snprintf(esp, sizeof(esp), "%s", fs_mount_path(&vols[d->fs[0] - 1]));
        if (d->fs[1])
            snprintf(data, sizeof(data), "%s", fs_mount_path(&vols[d->fs[1] - 1]));
        say(true, "volumes: %s is a Jam OS disk now: %s and %s, read-only", disk_name(d), esp,
            data);
    } else if (st != OK) {
        say(true, "volumes: prepare of disk %08x: %s", id, status_str(st));
    }
    return st;
}
