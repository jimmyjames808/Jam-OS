/* fat: writes held back (FS_GATHER, <os.h>), under disk.c's disk_write.
 *
 * Why. FatFs writes a file's data a cluster at a time, and on the ESP a
 * cluster is one sector: a 6 MiB boot image was 13,000 one-sector WRITE
 * commands, plus each FAT copy's sector every 128 clusters. A cheap USB 2
 * stick takes milliseconds for every small write, however small, so
 * `update -w`'s 20 MiB took more than seven minutes on the owner's PC.
 *
 * What. While a file opened FS_GATHER is written (disk_hold, fileops.c),
 * disk_write hands its sectors here instead of to the disk, up to
 * HOLD_MAX. An unlink's are held too (fsops.c), and sent before it is
 * answered: freeing a file changes a FAT sector per 128 clusters in each
 * FAT copy, two writes per 64 KiB of the file otherwise. What is held goes
 * out (disk_release) when anything needs it on the medium: any other
 * write (so that one keeps its place after it), a flush, a full hold, or
 * the file's sync or close. A read from the disk meanwhile gets the held
 * sectors laid over what it read (hold_overlay), and the writes a file
 * read makes FatFs do while writes are held (its window moving off a
 * changed FAT sector: fileops.c) join them.
 *
 * Order. Held sectors are kept as runs of consecutive sectors (a sector
 * right after a run's end joins it), and go out run by run in the order
 * the runs began, a block buffer's worth per write: the file's data in
 * 64 KiB writes, then each FAT copy's changed sectors in one. So the data
 * goes out before the FAT sectors that chain it, as FatFs wrote them, and
 * the directory entry that reaches both is written later still, by the
 * sync (which releases first); a later write to a sector already held
 * changes it in place.
 *
 * What is held is copied into the cache at once (reads see it), and
 * dropped from it if it fails to go out. Such a failure leaves FatFs's
 * idea of the volume (its FAT window, its free count) ahead of the disk,
 * so from then on fat writes nothing (vol.hold_failed: disk_write refuses,
 * the volume is never marked clean), and the FS_GATHER file's sync fails.
 * A new fat (a remount, the next boot) reads the disk as it is: at worst
 * clusters no file reaches. */
#include "fat.h"

#define HOLD_MAX  2304u   /* sectors held at most: a MiB of a file, the FAT sectors
                           * that chain it (on one-sector clusters, 16 per copy) */
#define HOLD_RUNS 32u     /* runs of consecutive sectors at most */

static uint8_t *data;            /* HOLD_MAX sectors (malloc, at the first hold) */
static uint64_t lba[HOLD_MAX];   /* data's sector i is for this sector */
static uint8_t  run_of[HOLD_MAX];/* ... and belongs to this run */
static uint32_t held;            /* sectors held */
static uint64_t run_first[HOLD_RUNS];   /* run r holds run_first[r] .. + run_len[r] - 1 */
static uint32_t run_len[HOLD_RUNS];
static uint32_t runs;            /* runs begun, in order */
static bool     holding;         /* disk_write holds instead of writing */
static uint64_t out_sectors;     /* sectors that went out held (fat's last line) */
static uint64_t out_writes;      /* ... in this many block writes */

void disk_hold(bool on)
{
    if (on && !data)
        data = malloc((size_t)HOLD_MAX * FAT_SECTOR);   /* none: written through, as ever */
    holding = on && data != NULL;
}

bool hold_active(void)
{
    return holding;
}

bool hold_pending(void)
{
    return held > 0;
}

/* The k sectors gathered in the block buffer, written at *first. */
static status_t put_out(uint64_t *first, uint32_t *k)
{
    status_t st = disk_block_write(*first, *k);
    if (st == OK) {
        *first += *k;
        out_writes++;
    }
    *k = 0;
    return st;
}

/* Run r's sectors, a block buffer's worth per write, in the order they
 * were held: the run's sector order. */
static status_t release_run(uint32_t r)
{
    uint32_t per = vol.bbuf_size / FAT_SECTOR, k = 0;
    uint64_t first = run_first[r];
    status_t st = OK;
    for (uint32_t i = 0; i < held && st == OK; i++) {
        if (run_of[i] != r)
            continue;
        memcpy(vol.bbuf + (size_t)k * FAT_SECTOR, data + (size_t)i * FAT_SECTOR, FAT_SECTOR);
        if (++k == per)
            st = put_out(&first, &k);
    }
    return st == OK && k ? put_out(&first, &k) : st;
}

status_t disk_release(void)
{
    if (!held)
        return OK;
    status_t st = OK;
    for (uint32_t r = 0; r < runs; r++) {
        if (st == OK)
            st = release_run(r);
        if (st == OK)
            out_sectors += run_len[r];
        else
            cache_forget(run_first[r], run_len[r]);   /* (part of) it never reached the disk */
    }
    held = runs = 0;
    if (st != OK && !vol.hold_failed) {
        vol.hold_failed = true;
        printf("fat %s: a held write didn't reach the disk: nothing more is written until fat "
               "starts again\n", vol.name);
    }
    return st;
}

/* Does a run hold any of count sectors at `sector`? */
static bool overlaps(uint64_t sector, uint32_t count)
{
    for (uint32_t r = 0; r < runs; r++)
        if (run_first[r] < sector + count && sector < run_first[r] + run_len[r])
            return true;
    return false;
}

void hold_overlay(uint64_t sector, uint32_t count, uint8_t *buf)
{
    if (!overlaps(sector, count))
        return;
    for (uint32_t i = 0; i < held; i++)
        if (lba[i] >= sector && lba[i] - sector < count)
            memcpy(buf + (size_t)(lba[i] - sector) * FAT_SECTOR, data + (size_t)i * FAT_SECTOR,
                   FAT_SECTOR);
}

/* Where sector is held (an index into data), or HOLD_MAX if it isn't. */
static uint32_t find(uint64_t sector)
{
    if (!overlaps(sector, 1))
        return HOLD_MAX;
    for (uint32_t i = held; i-- > 0;)
        if (lba[i] == sector)
            return i;
    return HOLD_MAX;
}

/* A new place for sector: at the end of the run it follows, else a new
 * run. Everything goes out first when the hold is full. */
static status_t place(uint64_t sector, uint32_t *at)
{
    status_t st = held == HOLD_MAX ? disk_release() : OK;
    uint32_t r = runs;
    while (st == OK && r > 0 && run_first[r - 1] + run_len[r - 1] != sector)
        r--;
    if (st == OK && r == 0) {   /* follows no run */
        if (runs == HOLD_RUNS)
            st = disk_release();
        r = runs++;
        run_first[r] = sector;
        run_len[r] = 0;
    } else if (st == OK) {
        r--;
    }
    if (st != OK)
        return st;
    run_len[r]++;
    lba[held] = sector;
    run_of[held] = (uint8_t)r;
    *at = held++;
    return OK;
}

status_t hold_put(const uint8_t *buff, uint64_t sector, uint32_t count)
{
    for (uint32_t c = 0; c < count; c++, sector++, buff += FAT_SECTOR) {
        uint32_t i = find(sector);
        if (i == HOLD_MAX) {
            status_t st = place(sector, &i);
            if (st != OK)
                return st;
        }
        uint8_t *d = data + (size_t)i * FAT_SECTOR;
        memcpy(d, buff, FAT_SECTOR);
        disk_patch_dirty(d, sector);
        cache_wrote(sector, 1, d);
    }
    return OK;
}

void disk_hold_stats(uint64_t *sectors, uint64_t *writes)
{
    *sectors = out_sectors;
    *writes = out_writes;
}
