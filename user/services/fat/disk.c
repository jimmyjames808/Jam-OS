/* fat: the disk under FatFs. FatFs calls disk_read / disk_write /
 * disk_ioctl (its diskio.h); here they become `block` calls on the one
 * partition channel fat was given. Data moves through the channel's
 * shared buffer (mapped once), a buffer's worth of sectors per call.
 *
 * Every block call has a deadline (BLOCK_WAIT, longer than usb-storage's
 * own command timeouts), so a disk that stops answering fails the request
 * with ERR_IO instead of hanging fat. A call that finds the channel closed
 * (the stick was pulled) sets vol.disk_gone; main.c then winds fat down.
 *
 * A read-only channel: disk_write answers "write protected" without
 * sending anything (the `fs` methods refuse writes before FatFs is even
 * called; this is the second fence).
 *
 * The dirty flag. FAT keeps a "clean shutdown" bit in FAT entry 1 (bit 15
 * on FAT16, bit 27 on FAT32; FAT12 has none), which FatFs neither reads
 * nor keeps. fat does: the bit is cleared on the medium before the first
 * sector written after a settle, and set again by disk_settle once
 * everything is flushed. So a volume whose power went while something was
 * unwritten is found dirty at the next mount (and logged; there is no
 * fsck). FatFs caches FAT sector 0 and writes it back with whatever bit it
 * read, so every write of that sector has the bit patched to the current
 * state on its way out.
 *
 * What a flush costs. block.sync is SCSI SYNCHRONIZE CACHE, the dear part
 * of a sync, so it is sent only when a sector was written since the last
 * one (`flushed`). A file.sync is then one flush: FatFs's own, after which
 * the clean bit is written and left for the next flush to carry. If the
 * power goes first the volume is found dirty with everything on it: a
 * false alarm, never a lie. fs.sync and fat's own end do flush the bit
 * (disk_settle(true)).
 *
 * Formatting. f_mkfs writes the boot sector first and the FATs after it,
 * so a format cut short (the stick pulled, the power gone) would leave a
 * boot sector that mounts over FATs full of whatever was there. Between
 * disk_hold_boot and disk_commit_boot the write of sector 0 is kept back
 * in memory and goes out last, after a flush: until then the partition is
 * still blank, and the next start formats it again. */
#include <ff.h>

#include <diskio.h>
#include <idl/block.h>
#include <wallclock.h>
#include "fat.h"

#define BLOCK_WAIT (30 * NS_PER_S)   /* one block call; usb-storage gives up after 10 s */

/* Where the clean-shutdown bit is in FAT sector 0 (byte offset, mask). */
static unsigned clean_off;
static uint8_t  clean_mask;

/* A format's boot sector, kept back until disk_commit_boot. */
static uint8_t boot[FAT_SECTOR];
static bool    boot_holding;   /* writes of sector 0 go into boot[] */
static bool    boot_held;      /* boot[] holds one */
static bool    flushed = true; /* no sector written since the last block.sync */

static uint64_t deadline(void)
{
    return now() + BLOCK_WAIT;
}

/* A failed block call: remember a vanished disk, log the rest. */
static status_t failed(const char *what, uint64_t lba, uint32_t count, status_t st)
{
    if (st == ERR_PEER_CLOSED) {
        vol.disk_gone = true;
        return st;
    }
    printf("fat %s: %s of %u sectors at %lu failed: %s\n", vol.name, what, count,
           (unsigned long)lba, status_str(st));
    return st;
}

status_t disk_open(handle_t block)
{
    uint32_t bs = 0, size = 0;
    uint8_t ro = 0;
    handle_t vmo = HANDLE_INVALID;
    status_t st = block_info_until(block, deadline(), &bs, &vol.blocks, &ro);
    if (st == OK && bs != FAT_SECTOR) {
        printf("fat %s: %u-byte sectors are not supported (only %u)\n", vol.name, bs, FAT_SECTOR);
        return ERR_NOT_SUPPORTED;
    }
    if (st == OK)
        st = block_map_buffer_until(block, deadline(), &vmo, &size);
    if (st != OK)
        return st;
    size -= size % PAGE_SIZE;
    uint64_t addr = 0;
    /* A read-only partition's buffer is only ever read here. */
    uint32_t perms = ro ? VMAR_READ : VMAR_READ | VMAR_WRITE;
    st = size ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, perms, &addr)
              : ERR_INVALID_ARGS;
    jam_handle_close(vmo);   /* the mapping keeps the buffer */
    if (st != OK)
        return st;
    vol.block = block;
    vol.bbuf = (uint8_t *)(uintptr_t)addr;
    vol.bbuf_size = size;
    vol.read_only = ro != 0;
    return OK;
}

/* block.sync, if anything was written since the last one. */
static status_t flush(void)
{
    if (flushed)
        return OK;
    status_t st = block_sync_until(vol.block, deadline());
    if (st != OK)
        return failed("sync", 0, 0, st);
    flushed = true;
    return OK;
}

status_t disk_is_blank(bool *out)
{
    status_t st = block_read_until(vol.block, deadline(), 0, 1, 0);
    if (st != OK)
        return failed("read", 0, 1, st);
    *out = !(vol.bbuf[510] == 0x55 && vol.bbuf[511] == 0xaa);
    return OK;
}

void disk_hold_boot(void)
{
    boot_holding = true;
    boot_held = false;
}

void disk_drop_boot(void)
{
    boot_holding = boot_held = false;
}

/* Where a boot sector keeps its label (BS_VolLab) and, on FAT32, which
 * sector holds its backup copy (BPB_BkBootSec; 0: none). */
static unsigned boot_label_at(const uint8_t *b, uint32_t *backup)
{
    bool fat32 = !memcmp(b + 82, "FAT32", 5);
    *backup = fat32 ? b[50] | (uint32_t)b[51] << 8 : 0;
    return fat32 ? 71 : 43;
}

status_t disk_commit_boot(const char *label)
{
    boot_holding = false;
    if (!boot_held)
        return ERR_BAD_STATE;
    /* FatFs writes "NO NAME" here and f_setlabel only makes the root
     * directory's entry; the specification wants the two to agree, and
     * some systems show this one. */
    uint32_t backup = 0;
    unsigned at = boot_label_at(boot, &backup);
    memset(boot + at, ' ', 11);
    memcpy(boot + at, label, strnlen(label, 11));
    status_t st = flush();
    if (st != OK)
        return st;
    /* The backup first: the partition stays blank until sector 0 is there. */
    for (int i = 0; i < 2; i++) {
        uint32_t sector = i == 0 ? backup : 0;
        if (i == 0 && (!backup || backup >= vol.blocks))
            continue;
        memcpy(vol.bbuf, boot, FAT_SECTOR);
        flushed = false;
        st = block_write_until(vol.block, deadline(), sector, 1, 0);
        if (st != OK)
            return failed("write", sector, 1, st);
    }
    return flush();
}

/* ---- the dirty flag ---------------------------------------------------------------- */

static void patch(uint8_t *sector, bool clean)
{
    if (clean)
        sector[clean_off] |= clean_mask;
    else
        sector[clean_off] &= (uint8_t)~clean_mask;
}

/* Rewrite the bit in FAT sector 0 of every FAT copy, straight on the disk. */
static status_t mark(bool clean)
{
    for (unsigned i = 0; i < vol.nfats; i++) {
        status_t st = block_read_until(vol.block, deadline(), vol.fat0[i], 1, 0);
        if (st != OK)
            return failed("read", vol.fat0[i], 1, st);
        patch(vol.bbuf, clean);
        flushed = false;
        st = block_write_until(vol.block, deadline(), vol.fat0[i], 1, 0);
        if (st != OK)
            return failed("write", vol.fat0[i], 1, st);
    }
    vol.clean_on_disk = clean;
    return OK;
}

void disk_watch(void)
{
    vol.track_dirty = false;
    if (vol.fs.fs_type == FS_FAT32) {
        clean_off = 7;        /* entry 1 is bytes 4..7; bit 27 */
        clean_mask = 0x08;
    } else if (vol.fs.fs_type == FS_FAT16) {
        clean_off = 3;        /* entry 1 is bytes 2..3; bit 15 */
        clean_mask = 0x80;
    } else {
        return;
    }
    vol.nfats = vol.fs.n_fats == 2 ? 2 : 1;
    vol.fat0[0] = vol.fs.fatbase;
    vol.fat0[1] = vol.fs.fatbase + vol.fs.fsize;
    status_t st = block_read_until(vol.block, deadline(), vol.fat0[0], 1, 0);
    if (st != OK) {
        (void)failed("read", vol.fat0[0], 1, st);   /* logged; the flag stays unknown */
        return;
    }
    vol.clean_on_disk = (vol.bbuf[clean_off] & clean_mask) != 0;
    if (!vol.clean_on_disk)
        printf("fat %s: the volume is dirty (not shut down cleanly): mounted anyway, there is "
               "no fsck\n", vol.name);
    vol.track_dirty = !vol.read_only;
}

status_t disk_settle(bool durable)
{
    if (vol.read_only)
        return OK;
    status_t st = OK;
    if (vol.track_dirty && !vol.clean_on_disk) {
        st = flush();   /* what the bit vouches for is on the medium first */
        if (st == OK)
            st = mark(true);
    }
    status_t st2 = durable ? flush() : OK;
    return st != OK ? st : st2;
}

/* ---- FatFs's callbacks (diskio.h) --------------------------------------------------- */

static bool in_range(LBA_t sector, UINT count)
{
    return count > 0 && sector < vol.blocks && count <= vol.blocks - sector;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !vol.bbuf)
        return STA_NOINIT;
    return vol.read_only ? STA_PROTECT : 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !in_range(sector, count))
        return RES_PARERR;
    uint32_t per = vol.bbuf_size / FAT_SECTOR;
    while (count) {
        uint32_t n = count < per ? count : per;
        status_t st = block_read_until(vol.block, deadline(), sector, n, 0);
        if (st != OK) {
            (void)failed("read", sector, n, st);   /* logged; FatFs gets RES_ERROR */
            return RES_ERROR;
        }
        memcpy(buff, vol.bbuf, (size_t)n * FAT_SECTOR);
        buff += (size_t)n * FAT_SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !in_range(sector, count))
        return RES_PARERR;
    if (vol.read_only)
        return RES_WRPRT;
    if (vol.track_dirty && vol.clean_on_disk && mark(false) != OK)
        return RES_ERROR;
    if (boot_holding && sector == 0) {
        memcpy(boot, buff, FAT_SECTOR);
        boot_held = true;
        buff += FAT_SECTOR;
        sector++;
        count--;
    }
    uint32_t per = vol.bbuf_size / FAT_SECTOR;
    while (count) {
        uint32_t n = count < per ? count : per;
        memcpy(vol.bbuf, buff, (size_t)n * FAT_SECTOR);
        for (unsigned i = 0; vol.track_dirty && i < vol.nfats; i++)
            if (vol.fat0[i] >= sector && vol.fat0[i] - sector < n)
                patch(vol.bbuf + (size_t)(vol.fat0[i] - sector) * FAT_SECTOR, false);
        flushed = false;
        status_t st = block_write_until(vol.block, deadline(), sector, n, 0);
        if (st != OK) {
            (void)failed("write", sector, n, st);   /* logged; FatFs gets RES_ERROR */
            return RES_ERROR;
        }
        buff += (size_t)n * FAT_SECTOR;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
        return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        if (vol.read_only)
            return RES_OK;
        return flush() == OK ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        /* FatFs's LBAs are 32 bits: a bigger partition is used up to there. */
        *(LBA_t *)buff = vol.blocks > 0xffffffffull ? 0xffffffffu : (LBA_t)vol.blocks;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = FAT_SECTOR;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;   /* the erase block size is unknown: no alignment */
        return RES_OK;
    }
    return RES_PARERR;
}

/* FatFs's clock, FAT-packed: (year - 1980) << 25 | month << 21 | day << 16 |
 * hour << 11 | minute << 5 | second / 2. FAT keeps local time (what other
 * computers show as it is): the system's clock in the system's zone
 * (libos clock_local, <wallclock.h>; init sets both from the real-time
 * clock and /data/etc/settings). Before init has set it, the RTC's own
 * reading; with no clock at all, 2026-01-01 00:00:00. */
DWORD get_fattime(void)
{
    struct civil t;
    (void)clock_local(&t);   /* not set: still the best date there is */
    if (t.year < 1980 || t.year > 2107)
        return (DWORD)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
    return (DWORD)(t.year - 1980) << 25 | (DWORD)t.month << 21 | (DWORD)t.day << 16 |
           (DWORD)t.hour << 11 | (DWORD)t.minute << 5 | (DWORD)t.second / 2;
}
