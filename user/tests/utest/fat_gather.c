/* utest: FS_GATHER (<os.h>, fat's disk.c "Writes held back") over a RAM
 * disk formatted as the ESP is: FAT32 with one-sector clusters, where
 * FatFs writes a file a sector at a time. A 1 MiB file written with
 * FS_GATHER costs the disk a few dozen block writes (64 KiB of data
 * each, every FAT copy's changed sectors in one), fewer than without (fat
 * holds each request's writes until it commits, so even then it is a few
 * per request, not one per cluster); both files read back right, through the
 * same fat and a fresh one; removing one (an unlink's writes are held
 * too) costs a few writes, not one per FAT sector freed in each FAT
 * copy, and frees its space; a reader that shares the writer's file sees
 * what is held; a request whose writes don't fit the hold goes out in
 * steps and leaves the file whole; and a held write that fails makes the
 * file's sync fail
 * and stops fat writing (the volume stays dirty). This is what makes
 * `update -w` take seconds on a real stick instead of many minutes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define GFILE      (1u << 20)   /* bytes per test file */
#define DISK_MIB   48u          /* FatFs formats this as FAT32, one sector a cluster */
#define GATHER_MAX 40u          /* block writes the gathered file may cost (about 30) */
#define UNLINK_MAX 10u          /* ... and removing a GFILE file (about 5; 35 not held) */
#define PLAIN_MAX  80u          /* ... and the file written without FS_GATHER (16 requests) */

static struct ramdisk disk;
static uint8_t pattern[GFILE], got[GFILE];

/* GFILE bytes of pattern[] into a new `path`, a buffer at a time, then
 * closed (and seen closed: an fs.sync after it). *writes: block writes
 * it cost. */
static bool put_big(const struct fatrun *r, const char *path, uint32_t flags, uint32_t *writes)
{
    uint32_t w0 = ramdisk_writes(&disk);
    struct tfile f;
    CHECK_ST(t_open(r, path, FS_WRITE | FS_CREATE | FS_TRUNCATE | flags, &f), OK);
    for (uint32_t off = 0; off < GFILE; off += FAT_BUF) {
        uint32_t done = 0;
        status_t st = t_write(&f, off, pattern + off, FAT_BUF, &done);
        if (st != OK || done != FAT_BUF) {
            t_close(&f);
            FAIL("%s: write at %u: %s", path, off, status_str(st));
        }
    }
    t_close(&f);
    CHECK_ST(t_sync(r), OK);
    *writes = ramdisk_writes(&disk) - w0;
    return true;
}

/* `path` holds pattern[] exactly. */
static bool is_big(const struct fatrun *r, const char *path)
{
    struct tfile f;
    CHECK_ST(t_open(r, path, FS_READ, &f), OK);
    CHECK_EQ(f.size, GFILE);
    for (uint32_t off = 0; off < GFILE; off += FAT_BUF) {
        uint32_t done = 0;
        status_t st = t_read(&f, off, got + off, FAT_BUF, &done);
        if (st != OK || done != FAT_BUF) {
            t_close(&f);
            FAIL("%s: read at %u: %s", path, off, status_str(st));
        }
    }
    t_close(&f);
    CHECK(!memcmp(got, pattern, GFILE));
    return true;
}

/* Removing the 1 MiB file frees 2048 clusters: 16 FAT sectors in each of
 * two FAT copies, one write each if written as FatFs goes; held, a few. */
static bool unlink_cost(const struct fatrun *r)
{
    uint64_t total, before, after;
    CHECK_ST(t_free(r, &total, &before), OK);
    uint32_t w0 = ramdisk_writes(&disk);
    CHECK_ST(t_unlink(r, "/plain.bin"), OK);
    uint32_t writes = ramdisk_writes(&disk) - w0;
    if (writes > UNLINK_MAX)
        FAIL("removing a 1 MiB file took %u block writes, want at most %u", writes, UNLINK_MAX);
    CHECK_ST(t_stat(r, "/plain.bin", NULL, NULL, NULL), ERR_NOT_FOUND);
    CHECK_ST(t_free(r, &total, &after), OK);
    CHECK(after >= before + GFILE);
    return is_big(r, "/gathered.bin");
}

/* /gathered.bin copied to /copy.bin FS_GATHER, a buffer read then a buffer
 * written, as `update -w` keeps the stick's build: each read moves FatFs's
 * window off the FAT sector the last write changed, and that write joins
 * what is held instead of sending it all out every 64 KiB. */
static bool copy_cost(const struct fatrun *r)
{
    struct tfile from, to;
    uint32_t w0 = ramdisk_writes(&disk);
    CHECK_ST(t_open(r, "/gathered.bin", FS_READ, &from), OK);
    CHECK_ST(t_open(r, "/copy.bin", FS_WRITE | FS_CREATE | FS_GATHER, &to), OK);
    status_t st = OK;
    for (uint32_t off = 0; off < GFILE && st == OK; off += FAT_BUF) {
        uint32_t done = 0;
        st = t_read(&from, off, got + off, FAT_BUF, &done);
        if (st == OK)
            st = t_write(&to, off, got + off, FAT_BUF, &done);
    }
    t_close(&from);
    t_close(&to);
    CHECK_ST(st, OK);
    CHECK_ST(t_sync(r), OK);
    uint32_t writes = ramdisk_writes(&disk) - w0;
    if (writes > GATHER_MAX)
        FAIL("copying 1 MiB FS_GATHER took %u block writes, want at most %u", writes, GATHER_MAX);
    if (!is_big(r, "/copy.bin"))
        return false;
    CHECK_ST(t_unlink(r, "/copy.bin"), OK);
    return true;
}

/* A reader opened alongside the FS_GATHER writer reads what is held. */
static bool reader_sees_held(const struct fatrun *r)
{
    struct tfile w, rd;
    uint32_t done = 0;
    CHECK_ST(t_open(r, "/shared.bin", FS_WRITE | FS_CREATE | FS_GATHER, &w), OK);
    CHECK_ST(t_write(&w, 0, pattern, FAT_BUF, &done), OK);
    CHECK_ST(t_open(r, "/shared.bin", FS_READ, &rd), OK);
    CHECK_ST(t_read(&rd, 0, got, FAT_BUF, &done), OK);
    CHECK_EQ(done, FAT_BUF);
    CHECK(!memcmp(got, pattern, FAT_BUF));
    CHECK_ST(file_sync_until(w.ch, now() + FAT_CALL_NS), OK);
    t_close(&rd);
    t_close(&w);
    return true;
}

/* A request whose writes don't fit fat's hold (2304 sectors): a truncate
 * that grows a file by GROW (6144 one-sector clusters of zeros) goes out
 * in steps, and the file is whole: its size, zeros where it grew, the
 * bytes before kept; removing it frees its space again. */
static bool grow_in_steps(const struct fatrun *r)
{
    enum { GROW = 3u << 20 };
    struct tfile f;
    uint32_t done = 0;
    uint64_t size = 0, total, before, after;
    CHECK_ST(t_free(r, &total, &before), OK);
    CHECK_ST(t_open(r, "/grown.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(t_write(&f, 0, pattern, FAT_BUF, &done), OK);
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, FAT_BUF + GROW), OK);
    CHECK_ST(file_stat_until(f.ch, now() + FAT_CALL_NS, &size, NULL), OK);
    CHECK_EQ(size, FAT_BUF + GROW);
    for (uint32_t off = 0; off < FAT_BUF + GROW; off += GROW / 4) {
        CHECK_ST(t_read(&f, off, got, FAT_BUF, &done), OK);
        CHECK_EQ(done, FAT_BUF);
        for (uint32_t i = 0; i < FAT_BUF; i++)
            if (got[i] != (off ? 0 : pattern[i]))
                FAIL("byte %u of /grown.bin is %#x", off + i, got[i]);
    }
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), OK);
    t_close(&f);
    CHECK_ST(t_stat(r, "/grown.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, FAT_BUF + GROW);
    CHECK_ST(t_unlink(r, "/grown.bin"), OK);
    CHECK_ST(t_free(r, &total, &after), OK);
    CHECK_EQ(after, before);
    return true;
}

/* A held write that fails: the file's sync says so, and fat writes
 * nothing more (the RAM disk works again, and still nothing is written). */
static bool failed_hold(const struct fatrun *r)
{
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(r, "/lost.bin", FS_WRITE | FS_CREATE | FS_GATHER, &f), OK);
    CHECK_ST(t_write(&f, 0, pattern, FAT_BUF, &done), OK);   /* held: nothing written yet */
    ramdisk_fail_writes(&disk, true);
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), ERR_IO);
    ramdisk_fail_writes(&disk, false);
    uint32_t w0 = ramdisk_writes(&disk);
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), ERR_IO);
    t_close(&f);
    CHECK_ST(t_unlink(r, "/gathered.bin"), ERR_IO);
    CHECK_EQ(ramdisk_writes(&disk) - w0, 0u);
    return true;
}

bool t_fat_gather(void)
{
    struct fatrun r;
    for (uint32_t i = 0; i < GFILE; i++)
        pattern[i] = (uint8_t)(i * 13 + i / 4096);
    if (!ramdisk_create(&disk, DISK_MIB * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_sync(&r), OK);   /* answered once fat has formatted and mounted: not counted */
    uint32_t gathered = 0, plain = 0;
    if (!put_big(&r, "/gathered.bin", FS_GATHER, &gathered) ||
        !put_big(&r, "/plain.bin", 0, &plain))
        return false;
    /* The volume fat formatted (it had answered by now): FAT32, one
     * 512-byte sector a cluster, as the ESP. */
    if (memcmp(disk.mem + 82, "FAT32", 5) || disk.mem[13] != 1)
        FAIL("not FAT32 with one-sector clusters: \"%.5s\", %u sectors a cluster",
             (const char *)disk.mem + 82, disk.mem[13]);
    if (gathered > GATHER_MAX)
        FAIL("1 MiB written FS_GATHER took %u block writes, want at most %u", gathered,
             GATHER_MAX);
    /* Without FS_GATHER each request's writes still go out together after
     * it (fat holds every request's writes until it commits): its data in
     * one 64 KiB write and the FAT sectors it finished, a few writes per
     * request rather than one per cluster. FS_GATHER, which carries them
     * across requests, still costs fewer. */
    if (plain > PLAIN_MAX || plain <= gathered)
        FAIL("1 MiB written without FS_GATHER took %u block writes (with it %u), want at most "
             "%u and more than with it", plain, gathered, PLAIN_MAX);
    if (!is_big(&r, "/gathered.bin") || !is_big(&r, "/plain.bin") || !unlink_cost(&r) ||
        !copy_cost(&r) || !reader_sees_held(&r) || !grow_in_steps(&r) || !fat_stop(&r))
        return false;

    /* A fresh fat (an empty cache): the bytes are on the disk. After the
     * lost hold, fat can't leave the volume clean: it ends with 1. */
    if (!fat_start(&r, &disk, false) || !is_big(&r, "/gathered.bin") || !failed_hold(&r))
        return false;
    CHECK_ST(jam_handle_close(r.fs), OK);
    if (!fat_wait(&r, 1) || !ramdisk_join(&disk))
        return false;
    /* The lost hold left the volume dirty: FAT[1]'s clean bit (bit 27) off. */
    uint32_t fat0 = disk.mem[14] | (uint32_t)disk.mem[15] << 8;   /* reserved sectors */
    CHECK_EQ(disk.mem[(size_t)fat0 * RAMDISK_SECTOR + 7] & 0x08, 0u);
    return ramdisk_destroy(&disk);
}
