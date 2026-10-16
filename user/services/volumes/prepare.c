/* volumes: making an NVMe drive a Jam OS disk (volinstall.prepare,
 * abi/idl/volumes.idl; M12.7 stage 2, docs/M12.7-PLAN.md "Preparing a
 * blank disk" and "NV-warn notes"). One task, disk_prepare, for the whole of it, so the loop serves
 * everything else meanwhile:
 *   1. the plan (plan_of, also voldisks.prepare_check's, which the shell
 *      asks before the owner types anything): refused outright for the
 *      boot disk, a drive that isn't NVMe or hasn't 512-byte blocks, the
 *      `diskro` word, a Jam OS disk with both partitions mounted, one too
 *      small, a volume mounted read-write, a table its driver couldn't
 *      read; busy (ERR_BAD_STATE) while it isn't settled or something else
 *      is under way on it. Otherwise one of three plans: PLAN_TABLE, an
 *      empty drive (the driver decides with its empty rules: storage.mode
 *      says MODE_BLANK); PLAN_FINISH, Jam OS's by our own check (mark.c)
 *      and by its driver's (storage.mode) with a partition that has no
 *      service (a prepare cut short after the table); PLAN_ERASE, anything
 *      else: a drive that isn't empty (or Jam OS's by our check alone, a
 *      header damaged, whether its volumes mount or not), erased
 *      only with the owner's confirmation, the end of its serial number and
 *      "erase" (checked here against the serial its driver gave, then by
 *      the driver again: nothing here decides what may be written);
 *   2. an erase: the drive's read-only mounts go (drop_services: a drive
 *      that isn't Jam OS's has no other), then as PLAN_TABLE;
 *   3. PLAN_TABLE: three random GUIDs and storage.create_table, which
 *      writes the table (an erase: zeros where the partitions start first),
 *      reads it back and answers once the drive reads as Jam OS's
 *      (drivers/nvme/table.c). The driver refuses anything but a blank
 *      drive, or a confirmed erase, with ERR_ACCESS_DENIED, whatever volumes
 *      thought;
 *   4. the disk is read again (disk_relearn: storage.info, .partition, a
 *      Jam OS disk by our check now) and its two partitions are started
 *      like any other disk's (fsvc.c's mount_others), except that each is
 *      opened WRITABLE and given its service's format word (`fresh`,
 *      fs_format_arg): the ESP goes to fat, which formats a partition whose
 *      first sector holds no boot signature; the data partition, blank,
 *      goes to extfs, which makes an ext volume on a partition whose first
 *      4 KiB are zeros (M13.6's "Ext by default"); each only then, so a
 *      partition that already holds a volume is mounted as it is. `fresh`
 *      is set here and nowhere else;
 *   5. once both are mounted, each goes back to its mount's own way: the
 *      ESP stopped in order and started again read-only (fs_restart), as
 *      `mount -r` does, like every other disk's (`mount -w` makes it
 *      writable); the data partition is /jam, read-write (the first Jam OS
 *      drive's: a second one's is /nvmeN, read-only). The answer is OK when
 *      both are mounted again.
 * A cut at any point leaves the drive blank, blank-in-progress (the driver
 * says so: <jam/parttable.h>), or a Jam OS disk with a partition that has no
 * volume yet (both formats write their volume's first blocks last): the
 * next prepare finishes it. An erase cut short leaves a drive that isn't
 * empty: the next prepare shows it and asks again.
 *
 * disk_format_data (volinstall.format_data, the shell's `disk format <disk>
 * data [fat]`) formats a Jam OS disk's data partition again, everything on
 * it lost: the owner typed the end of its serial number (checked here
 * again); its mount, if any, stops in order; the partition's first MiB is
 * written with zeros (part_wipe) and steps 4 and 5 run for it alone, as ext
 * or, when asked, FAT32. A cut after the zeros leaves a partition with no
 * volume, which the next prepare (or format) finishes. */
#include <idl/storage.h>
#include "disk.h"

#define TABLE_WAIT  (60 * NS_PER_S)    /* create_table: its writes and the read-back */
#define MOUNT_WAIT  (600 * NS_PER_S)   /* the disk read again, both partitions formatted, mounted */
#define BACK_WAIT   (60 * NS_PER_S)    /* both mounted again, read-only */
#define STEP        (100 * NS_PER_MS)
#define PARTS       2u                 /* a Jam OS disk's partitions */
#define BOTH        ((1u << PARTS) - 1)
/* The smallest disk Jam OS's layout fits (pt_jamos_plan: 1 MiB, the ESP's
 * 512 MiB, 64 MiB of data, 1 MiB; the driver has the last word, a few KiB
 * more for the backup table). */
#define PREPARE_MIN (578ull << 20)
#define ERASE_TAIL  4                  /* the serial's characters the owner types */
#define ERASE_WORD  "erase"            /* and the word (storage.idl, create_table) */

/* What a prepare of a disk would do: voldisks.prepare_check's answer. */
struct plan {
    uint8_t  what;       /* VOLDISKS_PLAN_* */
    uint8_t  refused;    /* VOLDISKS_REFUSED_* (NONE: it may go on) */
    unsigned todo;       /* the partitions to format */
};

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

/* Is a volume of d mounted read-write? */
static bool mounted_rw(const struct disk *d)
{
    for (unsigned p = 0; p < MAX_PARTS; p++)
        if (d->fs[p] && vols[d->fs[p] - 1].rw)
            return true;
    return false;
}

/* What d's driver found the drive to be (storage.mode), asked in the task. */
status_t driver_mode(struct disk *d, uint8_t *mode, uint8_t *ro, uint8_t *readable)
{
    struct call c;
    call_begin(&c, d->ch);
    status_t st = storage_mode_send(d->ch, c.txid);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + CALL_WAIT);
    return st == OK ? storage_mode_result(c.rep, &c.m, mode, ro, readable) : st;
}

/* Step 1, in a task (the driver is asked its mode). OK with *pl set (its
 * `refused` may say no), or ERR_NOT_FOUND / ERR_BAD_STATE / the driver's
 * error. */
static status_t plan_of(struct disk *d, struct plan *pl)
{
    *pl = (struct plan){ .refused = VOLDISKS_REFUSED_NONE };
    if (!d)
        return ERR_NOT_FOUND;
    if (d->state == DISK_BOOT || (d->state == DISK_DOWN && d->boot_down)) {
        pl->refused = VOLDISKS_REFUSED_BOOT_DISK;   /* what Jam OS runs from */
        return OK;
    }
    if (d->bus != DISKS_BUS_NVME) {
        pl->refused = VOLDISKS_REFUSED_NOT_NVME;
        return OK;
    }
    if (!d->known || !d->ch || d->state != DISK_OTHER || d->preparing || d->releasing)
        return ERR_BAD_STATE;
    if (d->block_size != FAT_BLOCK) {
        pl->refused = VOLDISKS_REFUSED_BLOCK_SIZE;   /* fat reads 512-byte blocks only */
        return OK;
    }
    uint16_t gen = d->gen;
    uint8_t mode = STORAGE_MODE_OTHER, ro = 1, readable = 0;
    status_t st = driver_mode(d, &mode, &ro, &readable);
    if (!still(d, gen) || d->state != DISK_OTHER)
        return ERR_BAD_STATE;   /* gone, or its driver restarted, meanwhile */
    if (st != OK)
        return st;
    if (ro) {
        pl->refused = VOLDISKS_REFUSED_READ_ONLY;
        return OK;
    }
    if (disk_jamos_gpt(d)) {
        pl->todo = unmounted(d);
        if (pl->todo && mode == STORAGE_MODE_JAMOS) {
            pl->what = VOLDISKS_PLAN_FINISH;
            return OK;
        }
        if (!pl->todo) {
            for (unsigned p = 0; p < PARTS; p++)
                if (!vols[d->fs[p] - 1].ready)
                    return ERR_BAD_STATE;   /* still starting: ask again in a moment */
        }
        if (!pl->todo && mode == STORAGE_MODE_JAMOS) {
            pl->refused = VOLDISKS_REFUSED_JAMOS;
            return OK;
        }
        /* Jam OS's by our mark but not by the driver's (a header damaged:
         * an erase cut after its primary entry array leaves its old header
         * and Jam OS's backup; a byte changed in it later), whether its
         * volumes mount from the backup table or not: it can't be
         * finished, installed on or written at all, as the driver writes
         * nothing on it, so it is shown and offered for erase as any drive
         * that isn't empty (R3 stage 3's design question 3, decided yes):
         * the strong confirmation, the serial's end and `erase`. */
    }
    pl->todo = BOTH;
    if (d->blocks < PREPARE_MIN / d->block_size) {
        pl->refused = VOLDISKS_REFUSED_TOO_SMALL;
        return OK;
    }
    if (mode == STORAGE_MODE_BLANK) {
        pl->what = VOLDISKS_PLAN_TABLE;   /* empty, by the driver's rules */
        return OK;
    }
    if (mounted_rw(d))
        pl->refused = VOLDISKS_REFUSED_IN_USE;
    else if (!readable)
        pl->refused = VOLDISKS_REFUSED_UNREADABLE;
    pl->what = VOLDISKS_PLAN_ERASE;
    return OK;
}

status_t disk_prepare_check(uint32_t id, uint8_t *plan, uint8_t *refused)
{
    struct plan pl;
    status_t st = plan_of(disk_named(id), &pl);
    *plan = pl.what;
    *refused = pl.refused;
    return st;
}

/* A refusal as volinstall.prepare's answer. */
static status_t refusal(uint8_t refused)
{
    switch (refused) {
    case VOLDISKS_REFUSED_NONE: return OK;
    case VOLDISKS_REFUSED_IN_USE: return ERR_BUSY;
    case VOLDISKS_REFUSED_JAMOS: return ERR_ALREADY_EXISTS;
    case VOLDISKS_REFUSED_NOT_NVME:
    case VOLDISKS_REFUSED_BLOCK_SIZE: return ERR_NOT_SUPPORTED;
    case VOLDISKS_REFUSED_TOO_SMALL: return ERR_NO_SPACE;
    case VOLDISKS_REFUSED_UNREADABLE: return ERR_IO;
    default: return ERR_ACCESS_DENIED;   /* the boot disk, `diskro` */
    }
}

static char upper(char ch)
{
    return ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
}

/* Is `tail` the last ERASE_TAIL characters of the serial number the drive
 * gave (all of it if shorter), in any case? A drive with no serial can't
 * be confirmed. */
static bool tail_is(const struct disk *d, const char *tail, uint32_t tail_len)
{
    size_t n = strlen(d->serial), want = n < ERASE_TAIL ? n : ERASE_TAIL;
    if (!want || tail_len != want)
        return false;
    for (size_t i = 0; i < want; i++)
        if (upper(d->serial[n - want + i]) != upper(tail[i]))
            return false;
    return true;
}

/* The owner's confirmation of an erase (volumes.idl, prepare): the
 * serial's end (tail_is) and `word` exactly ERASE_WORD. The driver checks
 * the same again. */
static bool confirmed(const struct disk *d, const char *tail, uint32_t tail_len,
                      const char *word, uint32_t word_len)
{
    return word_len == strlen(ERASE_WORD) && !memcmp(word, ERASE_WORD, word_len) &&
           tail_is(d, tail, tail_len);
}

/* Step 3: Jam OS's table onto the drive (the owner's confirmation passed
 * on for the driver's own check). */
static status_t make_table(struct disk *d, const char *tail, uint32_t tail_len, const char *word,
                           uint32_t word_len)
{
    uint8_t disk_guid[16], esp_guid[16], data_guid[16];
    new_guid(disk_guid);
    new_guid(esp_guid);
    new_guid(data_guid);
    struct call c;
    call_begin(&c, d->ch);
    status_t st = storage_create_table_send(d->ch, c.txid, disk_guid, esp_guid, data_guid, tail,
                                            tail_len, word, word_len);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + TABLE_WAIT);
    return st == OK ? storage_create_table_result(c.rep, &c.m) : st;
}

/* Step 5: the partitions in mask, read-write since their format, back to
 * their mount's own way (the vol's dflt_rw): a FAT volume stopped in order
 * and started again read-only; /jam, an ext data partition, stays as it is,
 * read-write. */
static status_t back_to_own_way(struct disk *d, uint16_t gen, unsigned mask)
{
    for (unsigned p = 0; p < PARTS; p++) {
        if (!(mask >> p & 1) || !d->fs[p])
            continue;
        d->fresh &= (uint16_t)~(1u << p);
        d->fresh_fat &= (uint16_t)~(1u << p);
        struct vol *b = &vols[d->fs[p] - 1];
        uint32_t serial = b->serial;
        uint64_t until = now() + BACK_WAIT;
        bool want = b->dflt_rw;
        status_t st = disk_can_restart(b, want);
        while (st == ERR_BAD_STATE && now() < until && still(d, gen) && b->serial == serial) {
            task_wait(tasks, now() + STEP);   /* its start is not over yet */
            st = disk_can_restart(b, want);
        }
        if (b->serial != serial)
            return ERR_BAD_STATE;   /* retired meanwhile */
        if (st == OK)
            continue;   /* that way already */
        if (st != IDL_LATER)
            return st;
        b->busy = true;
        st = fs_restart(b, want);
        if (b->serial == serial)
            b->busy = false;
        if (st != OK)
            return st;
    }
    return OK;
}

/* Where partition p of d is mounted, and how ("/nvme1, read-only"), into out. */
static void mounted_as(const struct disk *d, unsigned p, char *out, size_t cap)
{
    if (!d->fs[p]) {
        snprintf(out, cap, "?");
        return;
    }
    const struct vol *b = &vols[d->fs[p] - 1];
    snprintf(out, cap, "%s (%s), %s", fs_mount_path(b), b->ext ? "ext" : "FAT",
             b->rw ? "read-write" : "read-only");
}

/* Steps 2 to 5 on d, which the plan allows. */
static status_t steps(struct disk *d, uint16_t gen, const struct plan *pl, const char *tail,
                      uint32_t tail_len, const char *word, uint32_t word_len)
{
    status_t st;
    unsigned todo = pl->todo;
    if (pl->what == VOLDISKS_PLAN_ERASE) {
        say(true, "volumes: %s is not empty: erasing it, as the owner confirmed; its read-only "
            "mounts go first", disk_name(d));
        drop_services(d, DISK_OTHER);   /* read-only: nothing to write back */
    }
    if (pl->what != VOLDISKS_PLAN_FINISH) {
        st = make_table(d, tail, tail_len, word, word_len);
        if (st != OK) {
            say(true, "volumes: %s: prepare: the driver's create_table: %s", disk_name(d),
                status_str(st));
            if (pl->what == VOLDISKS_PLAN_ERASE && still(d, gen))
                disk_relearn(d);   /* what is on it now, mounted again as any disk's */
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
        st = back_to_own_way(d, gen, todo);
    if (st == OK)
        st = wait_settled(d, gen, BOTH, now() + BACK_WAIT);
    return st;
}

status_t disk_prepare(uint32_t id, const char *tail, uint32_t tail_len, const char *word,
                      uint32_t word_len)
{
    struct disk *d = disk_named(id);
    struct plan pl;
    status_t st = plan_of(d, &pl);
    if (st == OK)
        st = refusal(pl.refused);
    /* plan_of waited for the driver: nothing else may have begun on d since. */
    if (st == OK && (d->preparing || d->releasing))
        st = ERR_BAD_STATE;
    if (st == OK && pl.what == VOLDISKS_PLAN_ERASE &&
        !confirmed(d, tail, tail_len, word, word_len)) {
        say(true, "volumes: %s is not empty and its erase wasn't confirmed (the end of its "
            "serial number and \"%s\"): nothing done", disk_name(d), ERASE_WORD);
        st = ERR_ACCESS_DENIED;
    }
    if (st != OK) {
        say(true, "volumes: prepare of disk %08x: %s", id, status_str(st));
        return st;
    }
    uint16_t gen = d->gen;
    d->preparing = true;
    st = steps(d, gen, &pl, tail, tail_len, word, word_len);
    d = disk_named(id);   /* its slot may have been given up and used again meanwhile */
    if (d) {
        d->preparing = false;
        d->fresh = d->fresh_fat = 0;
    }
    if (st == OK && d) {
        char esp[48], data[48];   /* fs_mount_path's buffer is static */
        mounted_as(d, PART_ESP, esp, sizeof(esp));
        mounted_as(d, PART_DATA, data, sizeof(data));
        say(true, "volumes: %s is a Jam OS disk now: %s and %s", disk_name(d), esp, data);
    } else if (st != OK) {
        say(true, "volumes: prepare of disk %08x: %s", id, status_str(st));
    }
    return st;
}

/* ---- the data partition formatted again ------------------------------------------ */

/* Why d's data partition can't be formatted, by what volumes knows (the
 * driver is asked next); OK if it may be. */
static status_t format_refused(const struct disk *d, bool fat)
{
    if (!d)
        return ERR_NOT_FOUND;
    if (d->state == DISK_BOOT || (d->state == DISK_DOWN && d->boot_down))
        return ERR_ACCESS_DENIED;   /* what Jam OS runs from */
    if (d->bus != DISKS_BUS_NVME || (fat && d->block_size != FAT_BLOCK))
        return ERR_NOT_SUPPORTED;
    if (!d->known || !d->ch || d->state != DISK_OTHER || d->preparing || d->releasing)
        return ERR_BAD_STATE;
    if (!disk_jamos_gpt(d) || d->nparts < PARTS)
        return ERR_ACCESS_DENIED;   /* not a Jam OS disk, by our own check */
    if (d->fs[PART_DATA] && vols[d->fs[PART_DATA] - 1].held)
        return ERR_BUSY;   /* the installer writes it (install.c) */
    return OK;
}

/* The format on d, which the checks allow: the data partition's mount
 * stopped in order, its first MiB zeros, then a service that formats it
 * (prepare's steps 4 and 5 for it alone). */
static status_t format_steps(struct disk *d, uint16_t gen, bool fat)
{
    const unsigned bit = 1u << PART_DATA;
    if (d->fs[PART_DATA]) {
        struct vol *b = &vols[d->fs[PART_DATA] - 1];
        if (b->busy || b->starting || b->stopping)
            return ERR_BAD_STATE;   /* a remount or a restart is under way on it */
        uint32_t serial = b->serial;
        say(true, "volumes: %s: its data partition is formatted again, as the owner confirmed: "
            "%s stops first", disk_name(d), fs_mount_path(b));
        b->busy = true;
        (void)fs_stop_in_order(b);   /* said if it had to be killed */
        if (b->serial == serial) {
            b->busy = false;
            fs_retire(b);
        }
        mounts_update();
        if (!still(d, gen))
            return ERR_BAD_STATE;
    }
    d->fstype[PART_DATA] = VOLDISKS_FSTYPE_NONE;
    status_t st = part_wipe(d, PART_DATA);
    if (!still(d, gen))
        return ERR_BAD_STATE;
    if (st != OK) {
        say(true, "volumes: %s partition %u: its first MiB couldn't be written with zeros (%s)",
            disk_name(d), PART_DATA + 1, status_str(st));
        d->want |= (uint16_t)bit;   /* mounted again as it is now, if it holds a volume */
        others_pump();
        return ERR_IO;
    }
    d->fresh |= (uint16_t)bit;
    d->fresh_fat = (uint16_t)(fat ? bit : 0);
    d->nofat &= (uint16_t)~bit;
    d->want |= (uint16_t)bit;
    others_pump();
    st = wait_settled(d, gen, bit, now() + MOUNT_WAIT);
    if (st == OK && !d->fs[PART_DATA]) {
        say(true, "volumes: %s partition %u: not mounted after its format", disk_name(d),
            PART_DATA + 1);
        st = ERR_IO;
    }
    if (st == OK)
        st = back_to_own_way(d, gen, bit);
    if (st == OK)
        st = wait_settled(d, gen, bit, now() + BACK_WAIT);
    return st;
}

status_t disk_format_data(uint32_t id, const char *tail, uint32_t tail_len, bool fat)
{
    struct disk *d = disk_named(id);
    status_t st = format_refused(d, fat);
    if (st == OK && !tail_is(d, tail, tail_len)) {
        say(true, "volumes: %s: the format of its data partition wasn't confirmed (the end of "
            "its serial number): nothing done", disk_name(d));
        st = ERR_ACCESS_DENIED;
    }
    if (st != OK) {
        say(true, "volumes: format of disk %08x's data partition: %s", id, status_str(st));
        return st;
    }
    uint16_t gen = d->gen;
    d->preparing = true;   /* nothing else (a prepare, an install's hold, a release) meanwhile */
    uint8_t mode = STORAGE_MODE_OTHER, ro = 1, readable = 0;
    st = driver_mode(d, &mode, &ro, &readable);
    d = disk_named(id);
    if (!d || !still(d, gen) || d->state != DISK_OTHER)
        st = ERR_BAD_STATE;   /* gone, or its driver restarted, meanwhile */
    else if (st == OK && (ro || mode != STORAGE_MODE_JAMOS))
        st = ERR_ACCESS_DENIED;   /* `diskro`, or a table the driver doesn't read as Jam OS's:
                                   * it opens nothing writable, so nothing is stopped */
    else if (st == OK)
        st = format_steps(d, gen, fat);
    d = disk_named(id);   /* its slot may have been given up and used again meanwhile */
    if (d) {
        d->preparing = false;
        d->fresh = d->fresh_fat = 0;
    }
    if (st == OK && d) {
        char data[48];
        mounted_as(d, PART_DATA, data, sizeof(data));
        say(true, "volumes: %s's data partition is formatted: %s", disk_name(d), data);
    } else {
        say(true, "volumes: format of disk %08x's data partition: %s", id, status_str(st));
    }
    return st;
}
