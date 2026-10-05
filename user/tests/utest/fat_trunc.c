/* utest: a truncate to 0 whose writes don't fit fat's hold, which then go
 * out in steps before the request commits (user/services/fat/hold.c,
 * "Room"), never leaves the disk with the file's directory entry naming a
 * free cluster. A death between two steps makes the successor start fresh
 * from the disk as it is (adopt.c), and a free cluster that an entry still
 * names is one the next allocation gives to another file: two files
 * sharing clusters. So the first thing the disk may learn about the head
 * of the chain is that it now ends there; it may be freed only after.
 *
 * The test watches the disk write by write (ramdisk_on_write), as any death
 * between two of them would leave it: a 24 MiB file on FAT32 with
 * one-sector clusters (385 FAT sectors a copy) is opened with FS_TRUNCATE
 * while an FS_GATHER file holds most of the hold, so the truncate's freed
 * chain can't fit and goes out in steps. The file's first FAT entry must
 * change first to an end-of-chain mark, never straight to free. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define DISK_MIB      40u     /* FAT32 with one-sector clusters (FatFs's choice under 64 MiB) */
#define BIG_CHUNKS    384u    /* /big.bin: 24 MiB, 49152 clusters */
#define GATHER_CHUNKS 14u     /* /g.bin, FS_GATHER: about 1820 of the hold's 2304 sectors */
#define FAT_EOC       0x0ffffff8u   /* FAT32 entries from here on end a chain */

static struct ramdisk disk;
static uint8_t chunk[FAT_BUF];

/* The FAT entry being watched, and its first change (the server thread's). */
static struct {
    uint64_t sector;     /* the first FAT copy's sector that holds it */
    uint32_t off;        /* its offset there */
    uint32_t was;        /* its value before the truncate */
    uint32_t first;      /* the first other value written over it */
    bool     changed;    /* first is set (released) */
} watch;

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | le16(p + 2) << 16;
}

static uint32_t entry_now(void)
{
    return le32(disk.mem + watch.sector * RAMDISK_SECTOR + watch.off) & 0x0fffffffu;
}

static void wrote(struct ramdisk *rd, uint64_t lba, uint32_t count)
{
    (void)rd;
    if (__atomic_load_n(&watch.changed, __ATOMIC_ACQUIRE) || lba > watch.sector ||
        watch.sector - lba >= count)
        return;
    uint32_t v = entry_now();
    if (v == watch.was)
        return;
    watch.first = v;
    __atomic_store_n(&watch.changed, true, __ATOMIC_RELEASE);   /* the test acquires it */
}

/* /big.bin's first cluster, from the volume's root directory (its first
 * cluster: the label and two files), and where its FAT entry is. */
static bool find_big(void)
{
    const uint8_t *b = disk.mem;
    if (memcmp(b + 82, "FAT32", 5) || b[13] != 1)
        FAIL("not FAT32 with one-sector clusters: \"%.5s\", %u sectors a cluster",
             (const char *)b + 82, b[13]);
    uint32_t rsvd = le16(b + 14), fats = b[16], fatsz = le32(b + 36), root = le32(b + 44);
    const uint8_t *dir = b + ((uint64_t)rsvd + fats * fatsz + (root - 2)) * RAMDISK_SECTOR;
    for (unsigned i = 0; i < RAMDISK_SECTOR / 32; i++) {
        const uint8_t *e = dir + i * 32;
        if (memcmp(e, "BIG     BIN", 11))
            continue;
        uint32_t first = le16(e + 20) << 16 | le16(e + 26);
        CHECK(first >= 2);
        watch.sector = rsvd + (uint64_t)first * 4 / RAMDISK_SECTOR;
        watch.off = first * 4 % RAMDISK_SECTOR;
        watch.was = entry_now();
        CHECK(watch.was >= 2 && watch.was < FAT_EOC);   /* the chain goes on */
        return true;
    }
    FAIL("no BIG.BIN in the root directory's first sector");
}

/* n chunks into a new `path` opened with flags (and left open in *f). */
static bool fill(const struct fatrun *r, const char *path, uint32_t flags, uint32_t n,
                 struct tfile *f)
{
    CHECK_ST(t_open(r, path, FS_READ | FS_WRITE | FS_CREATE | flags, f), OK);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t done = 0;
        memset(chunk, (int)(i + 1), sizeof(chunk));
        status_t st = t_write(f, (uint64_t)i * FAT_BUF, chunk, FAT_BUF, &done);
        if (st != OK || done != FAT_BUF) {
            t_close(f);
            FAIL("%s: chunk %u: %s", path, i, status_str(st));
        }
    }
    return true;
}

static bool truncate_in_steps(const struct fatrun *r)
{
    struct tfile big, g;
    if (!fill(r, "/big.bin", 0, BIG_CHUNKS, &big))
        return false;
    t_close(&big);
    CHECK_ST(t_sync(r), OK);   /* its entry and chain on the disk */
    if (!find_big() || !fill(r, "/g.bin", FS_GATHER, GATHER_CHUNKS, &g))
        return false;
    ramdisk_on_write(&disk, wrote);
    status_t st = t_open(r, "/big.bin", FS_READ | FS_WRITE | FS_TRUNCATE, &big);
    ramdisk_on_write(&disk, NULL);
    CHECK_ST(st, OK);
    CHECK_EQ(big.size, 0u);
    t_close(&big);
    t_close(&g);
    CHECK(__atomic_load_n(&watch.changed, __ATOMIC_ACQUIRE));
    if (watch.first < FAT_EOC)
        FAIL("the truncated file's first FAT entry went from %#x to %#x on the disk while its "
             "directory entry still named it", watch.was, watch.first);
    CHECK_ST(t_sync(r), OK);
    uint64_t size = 1;
    CHECK_ST(t_stat(r, "/big.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, 0u);
    return true;
}

bool t_fat_truncate_steps(void)
{
    struct fatrun r;
    memset(&watch, 0, sizeof(watch));
    if (!ramdisk_create(&disk, DISK_MIB * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    bool ok = truncate_in_steps(&r);
    return fat_stop(&r) && ramdisk_destroy(&disk) && ok;
}
