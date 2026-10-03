/* fat: the hold. Every sector a request writes is held back here until
 * the request is committed (docs/M11.6-PLAN.md, "Held writes become the
 * write-ahead buffer"), and an FS_GATHER file's (<os.h>) stay held across
 * requests until its sync or close.
 *
 * Why, twice. Exactness: nothing of a request reaches the stick before its
 * commit (request.c), so a successor that finds a request uncommitted
 * finds the disk exactly as the request before left it, puts memory back
 * (undo.c) and runs it again. Speed: FatFs writes a file's data a cluster
 * at a time, and on the ESP a cluster is one sector: a 6 MiB boot image
 * was 13,000 one-sector WRITE commands, plus each FAT copy's sector every
 * 128 clusters, and a cheap USB 2 stick takes milliseconds for every small
 * write; held, they go out in 64 KiB writes.
 *
 * What. While an operation runs (op_running) disk_write hands every
 * sector here. After its commit, request.c's send puts them out
 * (disk_release), unless they may stay held: an FS_GATHER file's writes
 * (fileops.c), and the ones a read made while such writes were held
 * (moving FatFs's window off a changed FAT sector), wait for any other
 * request that writes, a flush, a full hold, or the file's sync or close.
 * A read from the disk meanwhile gets the held sectors laid over what it
 * read (hold_overlay).
 *
 * Undo. A request that writes a sector already held overwrites it in
 * place. If an earlier, committed request held it, its bytes are saved
 * first in the undo copy (undo_sector); when the copy has no room for
 * more, the new bytes are held anew instead, after the old ones (the
 * sector then goes out twice, in order, the newer last). What a request
 * appends is undone by putting the counts back (undo.c).
 *
 * Room. A request starts with room for a whole request (FAT_OP_ROOM
 * sectors, FAT_OP_RUNS runs): if the hold lacks it, what it holds (all
 * committed) goes out first (hold_make_room). A request that still fills
 * it goes out in steps: what is held is sent as the hold fills, said in
 * the log (op_steps). Only two kinds can: an unlink or truncate of a file
 * whose freed chain spans more FAT sectors than the hold (on /data's
 * 32 KiB clusters, over about 2 GiB), and a write or truncate that grows a
 * file by more than the hold (up to FAT_GROW_MAX). A death between steps
 * can leave clusters that no file reaches (lost space, never a damaged
 * file), so such a request can't be undone (undo_spend).
 *
 * Where. The hold is part of fat's state (struct fat_hold), so that what
 * it holds can outlive fat. Its data's pages are committed a chunk
 * (FAT_HOLD_CHUNK) at a time as it grows; with no memory for one, the
 * write fails.
 *
 * Order. Held sectors are kept as runs of consecutive sectors (a sector
 * right after a run's end joins it), and go out run by run in the order
 * the runs began, a block buffer's worth per write: a file's data in
 * 64 KiB writes, then each FAT copy's changed sectors in one. So the data
 * goes out before the FAT sectors that chain it, as FatFs wrote them, and
 * the directory entry that reaches both, written by the sync, later still.
 *
 * What is held is copied into the cache at once (reads see it), and
 * dropped from it if it fails to go out. Such a failure leaves FatFs's
 * idea of the volume (its FAT window, its free count) ahead of the disk,
 * so from then on fat writes nothing (kept->disk.hold_failed: disk_write
 * refuses, the volume is never marked clean), and the request whose send
 * failed is answered with the failure. A fresh fat (a remount, the next
 * boot) reads the disk as it is: at worst clusters no file reaches. */
#include "fat.h"

#define NO_RUN FAT_HOLD_RUNS   /* run_ending_at: no run ends there */

bool hold_pending(void)
{
    return kept->hold.held > 0;
}

/* The k sectors gathered in the block buffer, written at *first. */
static status_t put_out(uint64_t *first, uint32_t *k)
{
    status_t st = disk_block_write(*first, *k);
    if (st == OK) {
        *first += *k;
        kept->hold.out_writes++;
    }
    *k = 0;
    return st;
}

/* Run r's sectors, a block buffer's worth per write, in the order they
 * were held: the run's sector order. */
static status_t release_run(uint32_t r)
{
    const struct fat_hold *h = &kept->hold;
    uint32_t per = vol.bbuf_size / FAT_SECTOR, k = 0;
    uint64_t first = h->run_first[r];
    status_t st = OK;
    for (uint32_t i = 0; i < h->held && st == OK; i++) {
        if (h->run_of[i] != r)
            continue;
        memcpy(vol.bbuf + (size_t)k * FAT_SECTOR, h->data + (size_t)i * FAT_SECTOR, FAT_SECTOR);
        if (++k == per)
            st = put_out(&first, &k);
    }
    return st == OK && k ? put_out(&first, &k) : st;
}

status_t disk_release(void)
{
    struct fat_hold *h = &kept->hold;
    if (!h->held)
        return OK;
    status_t st = OK;
    for (uint32_t r = 0; r < h->runs; r++) {
        if (st == OK)
            st = release_run(r);
        if (st == OK)
            h->out_sectors += h->run_len[r];
        else   /* (part of) it never reached the disk */
            cache_forget(h->run_first[r], h->run_len[r]);
    }
    h->held = h->runs = 0;
    if (st != OK && !kept->disk.hold_failed) {
        kept->disk.hold_failed = true;
        printf("fat %s: a held write didn't reach the disk: nothing more is written until fat "
               "starts again\n", vol.name);
    }
    return st;
}

status_t hold_make_room(void)
{
    const struct fat_hold *h = &kept->hold;
    if (h->held + FAT_OP_ROOM <= FAT_HOLD_MAX && h->runs + FAT_OP_RUNS <= FAT_HOLD_RUNS)
        return OK;
    return disk_release();
}

/* Does a run hold any of count sectors at `sector`? */
static bool overlaps(uint64_t sector, uint32_t count)
{
    const struct fat_hold *h = &kept->hold;
    for (uint32_t r = 0; r < h->runs; r++)
        if (h->run_first[r] < sector + count && sector < h->run_first[r] + h->run_len[r])
            return true;
    return false;
}

void hold_overlay(uint64_t sector, uint32_t count, uint8_t *buf)
{
    const struct fat_hold *h = &kept->hold;
    if (!overlaps(sector, count))
        return;
    for (uint32_t i = 0; i < h->held; i++)   /* in order: a sector held twice, the newer last */
        if (h->lba[i] >= sector && h->lba[i] - sector < count)
            memcpy(buf + (size_t)(h->lba[i] - sector) * FAT_SECTOR,
                   h->data + (size_t)i * FAT_SECTOR, FAT_SECTOR);
}

/* Where sector is held (its newest copy: an index into data), or
 * FAT_HOLD_MAX if it isn't. */
static uint32_t find(uint64_t sector)
{
    const struct fat_hold *h = &kept->hold;
    if (!overlaps(sector, 1))
        return FAT_HOLD_MAX;
    for (uint32_t i = h->held; i-- > 0;)
        if (h->lba[i] == sector)
            return i;
    return FAT_HOLD_MAX;
}

/* The run that sector would continue (the newest whose end it is), or
 * NO_RUN. */
static uint32_t run_ending_at(uint64_t sector)
{
    const struct fat_hold *h = &kept->hold;
    for (uint32_t r = h->runs; r-- > 0;)
        if (h->run_first[r] + h->run_len[r] == sector)
            return r;
    return NO_RUN;
}

/* Room for one more sector (in a new run, if new_run): the data's pages
 * committed a chunk ahead. */
static bool has_room(bool new_run)
{
    struct fat_hold *h = &kept->hold;
    if (h->held == FAT_HOLD_MAX || (new_run && h->runs == FAT_HOLD_RUNS))
        return false;
    if (h->held < h->ready)
        return true;
    uint32_t n = FAT_HOLD_MAX - h->ready < FAT_HOLD_CHUNK ? FAT_HOLD_MAX - h->ready
                                                           : FAT_HOLD_CHUNK;
    if (state_commit(h->data + (size_t)h->ready * FAT_SECTOR, (size_t)n * FAT_SECTOR) != OK)
        return false;
    h->ready += n;
    return true;
}

/* A new place for sector, at *at: at the end of the run it follows, else
 * (or if `again`: the sector is held already, and its new copy must go
 * out after the old one, so in a run begun after every other) a new run.
 * A full hold goes out first, in the middle of the operation: its steps
 * (the header's "Room"). */
static status_t place(uint64_t sector, bool again, uint32_t *at)
{
    struct fat_hold *h = &kept->hold;
    uint32_t r = again ? NO_RUN : run_ending_at(sector);
    if (!has_room(r == NO_RUN)) {
        if (op_running())
            op_steps();
        status_t st = disk_release();
        if (st != OK)
            return st;
        r = NO_RUN;
        if (!has_room(true))
            return ERR_NO_MEMORY;   /* no pages for the hold: the write fails */
    }
    if (r == NO_RUN) {
        r = h->runs++;
        h->run_first[r] = sector;
        h->run_len[r] = 0;
    }
    h->run_len[r]++;
    h->lba[h->held] = sector;
    h->run_of[h->held] = (uint8_t)r;
    *at = h->held++;
    return OK;
}

status_t hold_put(const uint8_t *buff, uint64_t sector, uint32_t count)
{
    struct fat_hold *h = &kept->hold;
    op_wrote();
    for (uint32_t c = 0; c < count; c++, sector++, buff += FAT_SECTOR) {
        uint32_t i = find(sector);
        bool again = false;
        /* Held by an earlier, committed operation: saved before it is
         * overwritten, or, with no room to save it, held anew after it. */
        if (i != FAT_HOLD_MAX && i < op_hold_first() && !undo_sector(i)) {
            i = FAT_HOLD_MAX;
            again = true;
        }
        if (i == FAT_HOLD_MAX) {
            status_t st = place(sector, again, &i);
            if (st != OK)
                return st;
        }
        uint8_t *d = h->data + (size_t)i * FAT_SECTOR;
        memcpy(d, buff, FAT_SECTOR);
        disk_patch_dirty(d, sector);
        cache_wrote(sector, 1, d);
    }
    return OK;
}

void disk_hold_stats(uint64_t *sectors, uint64_t *writes)
{
    *sectors = kept->hold.out_sectors;
    *writes = kept->hold.out_writes;
}
