/* fat: the block cache, write-through.
 *
 * FatFs reads one cluster's sectors per disk_read, and on the ESP a
 * cluster is one sector: a 3 MiB file was 6000 block calls, each a round
 * trip to usb-storage and a SCSI command on the stick. So a read goes by
 * lines: CACHE_LINE sectors (64 KiB, one block call: the block buffer's
 * size), aligned, read whole and kept, CACHE_LINES of them, the least
 * recently used given up first. The next sectors of a file, and the FAT
 * and directory sectors around the one asked for, are then a memcpy.
 *
 * Write-through (sticks get pulled): every write still goes to the disk
 * at once, as it did without the cache; afterwards the sectors written are
 * copied into the lines that hold them (a line is never made by a write).
 * So what the cache holds is always what the disk holds, and nothing is
 * lost when the stick goes. Nobody else writes the partition while fat
 * runs (the block channel is fat's alone; a stick changed elsewhere comes
 * back as a new fat), so the cache never goes stale.
 *
 * A read of CACHE_BYPASS sectors or more (big clusters read whole) goes
 * straight to the disk: it would only push the FAT and directory lines
 * out. */
#include <idl/block.h>
#include "fat.h"

#define CACHE_LINE   128u   /* sectors per line: 64 KiB */
#define CACHE_LINES  16u    /* 1 MiB per volume */
#define CACHE_BYPASS CACHE_LINE

struct line {
    bool     valid;         /* holds sectors [first, first + count) */
    uint64_t first;         /* its first sector (a multiple of CACHE_LINE) */
    uint32_t count;         /* sectors held (fewer only at the partition's end) */
    uint64_t used;          /* when it was last used (cache_clock) */
    uint8_t *data;          /* count * FAT_SECTOR bytes (malloc, CACHE_LINE sectors) */
};

static struct line lines[CACHE_LINES];
static uint64_t cache_clock;   /* bumped on every use: the LRU order */
static struct fat_cache_stats stats;

/* The line holding `sector`, or NULL. */
static struct line *find(uint64_t sector)
{
    uint64_t first = sector - sector % CACHE_LINE;
    for (unsigned i = 0; i < CACHE_LINES; i++)
        if (lines[i].valid && lines[i].first == first)
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

/* Read the line holding `sector` from the disk. */
static status_t fill(uint64_t sector, struct line **out)
{
    struct line *l = victim();
    if (!l)
        return ERR_NO_MEMORY;
    uint64_t first = sector - sector % CACHE_LINE;
    uint64_t left = vol.blocks - first;
    uint32_t count = left < CACHE_LINE ? (uint32_t)left : CACHE_LINE;
    status_t st = disk_block_read(first, count);
    if (st != OK)
        return st;
    memcpy(l->data, vol.bbuf, (size_t)count * FAT_SECTOR);
    l->first = first;
    l->count = count;
    l->valid = true;
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
    while (count) {
        uint64_t first = sector - sector % CACHE_LINE;
        uint32_t n = (uint32_t)(first + CACHE_LINE - sector);
        if (n > count)
            n = count;
        struct line *l = find(sector);
        uint32_t at = (uint32_t)(sector - first);
        if (l && at < l->count) {
            uint32_t m = l->count - at < n ? l->count - at : n;
            memcpy(l->data + (size_t)at * FAT_SECTOR, data, (size_t)m * FAT_SECTOR);
            stats.updated += m;
        }
        data += (size_t)n * FAT_SECTOR;
        sector += n;
        count -= n;
    }
}

void cache_stats(struct fat_cache_stats *out)
{
    *out = stats;
}
