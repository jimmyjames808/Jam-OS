/* utest: fat's block cache (user/services/fat/cache.c) over a RAM disk.
 * A file read twice in a fresh fat costs the disk a block read per 64 KiB
 * line, not one per cluster; what is written goes to the disk at once and
 * is what the next read sees (write-through: the cache never holds
 * anything the disk doesn't). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define CACHE_FILE (512u << 10)   /* bytes: 8 of the cache's 64 KiB lines */
#define LINE_BYTES (64u << 10)

static struct ramdisk disk;
static uint8_t pattern[CACHE_FILE], got[CACHE_FILE];

/* Read all of `path` (CACHE_FILE bytes) into got[]. */
static bool read_all(const struct fatrun *r, const char *path)
{
    struct tfile f;
    CHECK_ST(t_open(r, path, FS_READ, &f), OK);
    CHECK_EQ(f.size, CACHE_FILE);
    for (uint32_t off = 0; off < CACHE_FILE;) {
        uint32_t done = 0;
        status_t st = t_read(&f, off, got + off, FAT_BUF, &done);
        if (st != OK || !done) {
            t_close(&f);
            FAIL("read at %u: %s, %u bytes", off, status_str(st), done);
        }
        off += done;
    }
    t_close(&f);
    return true;
}

static bool write_file(const struct fatrun *r, const char *path, uint32_t off, const void *src,
                       uint32_t n, uint32_t flags)
{
    struct tfile f;
    CHECK_ST(t_open(r, path, FS_WRITE | flags, &f), OK);
    for (uint32_t k = 0; k < n;) {
        uint32_t done = 0, chunk = n - k < FAT_BUF ? n - k : FAT_BUF;
        status_t st = t_write(&f, off + k, (const uint8_t *)src + k, chunk, &done);
        if (st != OK || done != chunk) {
            t_close(&f);
            FAIL("write at %u: %s", off + k, status_str(st));
        }
        k += chunk;
    }
    t_close(&f);
    return true;
}

/* Where pattern[] sits on the disk: the first sector holding its start. */
static const uint8_t *on_disk(const uint8_t *what, uint32_t n)
{
    for (uint64_t s = 0; s < disk.blocks; s++)
        if (!memcmp(disk.mem + s * RAMDISK_SECTOR, what, n))
            return disk.mem + s * RAMDISK_SECTOR;
    return NULL;
}

bool t_fat_cache(void)
{
    struct fatrun r;
    for (uint32_t i = 0; i < CACHE_FILE; i++)
        pattern[i] = (uint8_t)(i * 7 + i / 512);
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false) ||
        !write_file(&r, "/big.bin", 0, pattern, CACHE_FILE, FS_CREATE | FS_TRUNCATE) ||
        !fat_stop(&r))
        return false;

    /* A fresh fat: an empty cache. The first read costs one block read per
     * line (and a few for the FAT and the directory); the second none. */
    if (!fat_start(&r, &disk, false))
        return false;
    uint32_t r0 = ramdisk_reads(&disk);
    if (!read_all(&r, "/big.bin"))
        return false;
    CHECK(!memcmp(got, pattern, CACHE_FILE));
    uint32_t first = ramdisk_reads(&disk) - r0;
    if (first > CACHE_FILE / LINE_BYTES + 6)
        FAIL("reading %u KiB took %u block reads, want at most %u (a line each)",
             CACHE_FILE >> 10, first, CACHE_FILE / LINE_BYTES + 6);
    uint32_t r1 = ramdisk_reads(&disk);
    if (!read_all(&r, "/big.bin"))
        return false;
    CHECK(!memcmp(got, pattern, CACHE_FILE));
    uint32_t again = ramdisk_reads(&disk) - r1;
    CHECK(again <= 2);   /* the cache holds 1 MiB: all of it is still there */

    /* Write-through: bytes overwritten in the middle of the cached file are
     * on the disk once the file is closed (and fat has seen the close),
     * and the next read sees them. */
    static const char news[] = "written through the cache";
    uint32_t mid = CACHE_FILE / 2 + 100;
    if (!write_file(&r, "/big.bin", mid, news, sizeof(news) - 1, 0))
        return false;
    memcpy(pattern + mid, news, sizeof(news) - 1);
    CHECK_ST(t_sync(&r), OK);   /* fat finishes the closed file first */
    const uint8_t *sector = on_disk(pattern + (mid & ~511u), 512);
    CHECK(sector != NULL);
    if (!read_all(&r, "/big.bin"))
        return false;
    CHECK(!memcmp(got, pattern, CACHE_FILE));
    return fat_stop(&r) && ramdisk_destroy(&disk);
}
