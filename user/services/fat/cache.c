/* fat: the block cache, write-through.
 *
 * FatFs reads one cluster's sectors per disk_read, and on the ESP a
 * cluster is one sector: a 3 MiB file was 6000 block calls, each a round
 * trip to usb-storage and a SCSI command on the stick. So a miss reads a
 * run of sectors into a line and keeps it, CACHE_LINES lines of up to
 * CACHE_LINE sectors (64 KiB, one block call: the block buffer's size),
 * the least recently used given up first. The next sectors of a file, and
 * the FAT and directory sectors around the one asked for, are then a
 * memcpy.
 *
 * How much a miss reads (read-ahead): FILL_MIN sectors (4 KiB) from the
 * sector asked for, rounded down to FILL_MIN; a miss right where the last
 * fill ended (a file read from start to end) reads twice what that one
 * did, up to a whole line. So scattered reads (a mount, a directory, the
 * FAT) cost a stick little more than before, and long ones go by 64 KiB.
 * Lines may overlap (a fill isn't cut short at a line already there);
 * they hold the same bytes, since they come from the same disk.
 *
 * Write-through (sticks get pulled): every write still goes to the disk
 * at once, as it did without the cache; afterwards the sectors written are
 * copied into every line that holds them (a line is never made by a
 * write). So what the cache holds is always what the disk holds, and
 * nothing is lost when the stick goes. Nobody else writes the partition
 * while fat runs (the block channel is fat's alone; a stick changed
 * elsewhere comes back as a new fat), so the cache never goes stale.
 *
 * A read of CACHE_BYPASS sectors or more (big clusters read whole) goes
 * straight to the disk: it would only push the FAT and directory lines
 * out.
 *
 * Writes held back (disk.c, a file opened FS_GATHER) are copied into the
 * lines when they are held, so reads see them; a held write that then
 * fails to reach the disk takes its lines with it (cache_forget), so what
 * the cache holds is still never other than what the disk holds or is
 * about to. */
#include <idl/block.h>
#include "fat.h"

#define CACHE_LINE   128u   /* sectors a line holds at most: 64 KiB */
#define CACHE_LINES  16u    /* 1 MiB per volume */
#define CACHE_BYPASS CACHE_LINE
#define FILL_MIN     8u     /* sectors a miss reads at least: 4 KiB */

struct line {
    bool     valid;         /* holds sectors [first, first + count) */
    uint64_t first;         /* its first sector (a multiple of FILL_MIN) */
    uint32_t count;         /* sectors held */
    uint64_t used;          /* when it was last used (cache_clock) */
    uint8_t *data;          /* count * FAT_SECTOR bytes (malloc, CACHE_LINE sectors) */
};

static struct line lines[CACHE_LINES];
static uint64_t cache_clock;   /* bumped on every use: the LRU order */
static uint64_t last_end;      /* the sector after the last fill */
static uint32_t last_fill;     /* how many sectors that fill read */
static struct fat_cache_stats stats;

/* A line holding `sector`, or NULL. */
static struct line *find(uint64_t sector)
{
    for (unsigned i = 0; i < CACHE_LINES; i++)
        if (lines[i].valid && sector >= lines[i].first && sector - lines[i].first < lines[i].count)
            return &lines[i];
    return NULL;
}

/* A line to fill: an empty one, or the least recently used. NULL if
 * there is no memory for one. */
static struct line *victim(void)
{
    struct line *v = &lines[0];
    for (unsigned i = 0; i < CACHE_LINES; i++) {
        if (!lines[i].valid) {
            v = &lines[i];
            break;
        }
        if (lines[i].used < v->used)
            v = &lines[i];
    }
    if (!v->data && !(v->data = malloc((size_t)CACHE_LINE * FAT_SECTOR)))
        return NULL;
    v->valid = false;
    return v;
}

/* Read a run of sectors from `sector` on into a line (the read-ahead in
 * the header). */
static status_t fill(uint64_t sector, struct line **out)
{
    struct line *l = victim();
    if (!l)
        return ERR_NO_MEMORY;
    uint64_t first = sector - sector % FILL_MIN;
    uint32_t want = FILL_MIN;
    if (first == last_end && last_fill)
        want = last_fill * 2 < CACHE_LINE ? last_fill * 2 : CACHE_LINE;
    uint64_t left = vol.blocks - first;
    uint32_t count = left < want ? (uint32_t)left : want;
    status_t st = disk_block_read(first, count);
    if (st != OK)
        return st;
    memcpy(l->data, vol.bbuf, (size_t)count * FAT_SECTOR);
    l->first = first;
    l->count = count;
    l->valid = true;
    last_end = first + count;
    last_fill = count;
    stats.fills++;
    *out = l;
    return OK;
}

status_t cache_read(uint64_t sector, uint32_t count, uint8_t *buff)
{
    if (count >= CACHE_BYPASS || vol.bbuf_size < CACHE_LINE * FAT_SECTOR) {
        stats.bypassed++;
        return disk_read_direct(sector, count, buff);
    }
    while (count) {
        struct line *l = find(sector);
        if (l) {
            stats.hits++;
        } else {
            status_t st = fill(sector, &l);
            if (st != OK)
                return st;
        }
        l->used = ++cache_clock;
        uint32_t at = (uint32_t)(sector - l->first);
        uint32_t n = l->count - at < count ? l->count - at : count;
        memcpy(buff, l->data + (size_t)at * FAT_SECTOR, (size_t)n * FAT_SECTOR);
        buff += (size_t)n * FAT_SECTOR;
        sector += n;
        count -= n;
    }
    return OK;
}

void cache_wrote(uint64_t sector, uint32_t count, const uint8_t *data)
{
    uint64_t end = sector + count;
    for (unsigned i = 0; i < CACHE_LINES; i++) {
        struct line *l = &lines[i];
        if (!l->valid || l->first >= end || sector >= l->first + l->count)
            continue;
        uint64_t from = sector > l->first ? sector : l->first;
        uint64_t to = end < l->first + l->count ? end : l->first + l->count;
        memcpy(l->data + (from - l->first) * FAT_SECTOR, data + (from - sector) * FAT_SECTOR,
               (size_t)(to - from) * FAT_SECTOR);
        stats.updated += to - from;
    }
}

void cache_forget(uint64_t sector, uint32_t count)
{
    uint64_t end = sector + count;
    for (unsigned i = 0; i < CACHE_LINES; i++)
        if (lines[i].valid && lines[i].first < end && sector < lines[i].first + lines[i].count)
            lines[i].valid = false;
}

void cache_stats(struct fat_cache_stats *out)
{
    *out = stats;
}
