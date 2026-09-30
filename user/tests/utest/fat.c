/* utest: the fat service (user/services/fat) over a RAM disk: formatting
 * a blank partition, files and directories through the `fs` and `file`
 * protocols, a full disk, a read-only partition, partitions that must not
 * be formatted, the volume's dirty flag, and a disk that fails or goes
 * away. The names a FAT driver gets wrong easily are in fat_names.c. Each
 * test starts fat in a job of its own and ends with fat exited by itself
 * (0 unless the test says otherwise) and its job empty.
 *
 * The three FAT kinds are all used: a 1 MiB disk becomes FAT12, 16 MiB
 * FAT16, 40 MiB FAT32 (FatFs's f_mkfs picks by size; only the pages a test
 * touches are ever committed). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define FIXED_MTIME 1767225600ull   /* 2026-01-01 00:00:00: fat's clock without the RTC */

static struct ramdisk disk;

/* ---- the raw disk --------------------------------------------------------------------- */

static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

/* The volume's kind from its boot sector (12, 16, 32; 0: no FAT volume).
 * FAT12 and FAT16 differ only in how many clusters there are (the FAT
 * specification's rule: fewer than 4085 is FAT12). */
static unsigned fat_kind(const struct ramdisk *rd)
{
    const uint8_t *b = rd->mem;
    if (b[510] != 0x55 || b[511] != 0xaa || b[13] == 0)
        return 0;
    if (!memcmp(b + 82, "FAT32   ", 8))
        return 32;
    uint32_t total = le16(b + 19) ? le16(b + 19) : le16(b + 32) | le16(b + 34) << 16;
    uint32_t system = le16(b + 14) + b[16] * le16(b + 22) + le16(b + 17) / 16;
    return (total - system) / b[13] < 4085 ? 12 : 16;
}

/* FAT entry 1's clean-shutdown bit in FAT copy `copy` (FAT16: bit 15,
 * FAT32: bit 27). */
static bool clean_bit(const struct ramdisk *rd, unsigned copy)
{
    const uint8_t *b = rd->mem;
    bool fat32 = fat_kind(rd) == 32;
    uint32_t size = fat32 ? (b[36] | (uint32_t)b[37] << 8 | (uint32_t)b[38] << 16) : le16(b + 22);
    const uint8_t *fat = rd->mem + (uint64_t)(le16(b + 14) + copy * size) * RAMDISK_SECTOR;
    return fat32 ? (fat[7] & 0x08) != 0 : (fat[3] & 0x80) != 0;
}

/* fat started over rd must give up by itself (exit 1) without writing a
 * sector. */
static bool gives_up(struct ramdisk *rd, bool read_only)
{
    struct fatrun r;
    uint32_t writes = ramdisk_writes(rd);
    if (!fat_start(&r, rd, read_only) || !fat_wait(&r, 1) || !ramdisk_join(rd))
        return false;
    CHECK_EQ(ramdisk_writes(rd), writes);
    CHECK_ST(jam_handle_close(r.fs), OK);
    return true;
}

/* ---- tests ---------------------------------------------------------------------------- */

/* A blank partition is formatted (FAT32, one volume over the whole
 * partition, label JAMOS-DATA), once: the next fat finds the volume. A
 * format cut short leaves the partition blank, to be formatted again. */
bool t_fat_format(void)
{
    struct fatrun r;
    uint64_t total = 0, free_bytes = 0;
    uint8_t ro = 9, label[16];
    unsigned n = 9;
    bool dir = false;
    if (!ramdisk_create(&disk, 40 * MIB_SECTORS))
        return false;
    ramdisk_fail_after(&disk, 6);   /* the disk fails in the middle of the FATs */
    if (!fat_start(&r, &disk, false) || !fat_wait(&r, 1) || !ramdisk_join(&disk))
        return false;
    CHECK_ST(jam_handle_close(r.fs), OK);
    CHECK_EQ(ramdisk_writes(&disk), 6);
    CHECK_EQ(fat_kind(&disk), 0);   /* no boot sector yet: still blank */
    ramdisk_fail_after(&disk, 0);

    if (!fat_start(&r, &disk, false))
        return false;
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, &total, &free_bytes, &ro, label), OK);
    CHECK(!strcmp((const char *)label, "JAMOS-DATA"));
    CHECK_EQ(ro, 0);
    CHECK(total > 36ull << 20 && total < 40ull << 20 && free_bytes <= total);
    CHECK(free_bytes > total - (1u << 20));
    CHECK_EQ(fat_kind(&disk), 32);
    CHECK(disk.mem[0] == 0xeb);   /* a boot sector at sector 0: no partition table inside */
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));
    CHECK_ST(t_stat(&r, "/", NULL, &dir, NULL), OK);
    CHECK(dir);
    if (!dir_count(&r, "/", NULL, &n, NULL))
        return false;
    CHECK_EQ(n, 0);
    if (!put_file(&r, "/kept.txt", "still here") || !fat_stop(&r))
        return false;
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));

    if (!fat_start(&r, &disk, false) || !file_is(&r, "/kept.txt", "still here"))
        return false;
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, NULL, NULL, NULL, label), OK);
    CHECK(!strcmp((const char *)label, "JAMOS-DATA"));
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* Reading: offsets, the end of the file, what a handle may do. */
static bool check_reads(const struct fatrun *r)
{
    struct tfile f;
    char got[64];
    uint32_t done = 9;
    uint64_t size = 0, mtime = 0;
    bool dir = true;
    CHECK_ST(t_stat(r, "/hello.txt", &size, &dir, &mtime), OK);
    CHECK(size == 10 && !dir && mtime == FIXED_MTIME);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ, &f), OK);
    CHECK_EQ(f.size, 10);
    CHECK_ST(t_read(&f, 0, got, 5, &done), OK);
    CHECK(done == 5 && !memcmp(got, "hello", 5));
    CHECK_ST(t_read(&f, 7, got, 50, &done), OK);
    CHECK(done == 3 && !memcmp(got, "fat", 3));
    CHECK_ST(t_read(&f, 10, got, 5, &done), OK);
    CHECK_EQ(done, 0);
    CHECK_ST(t_read(&f, 1u << 30, got, 5, &done), OK);   /* far past the end: no growth */
    CHECK_EQ(done, 0);
    CHECK_ST(t_write(&f, 0, "x", 1, &done), ERR_ACCESS_DENIED);   /* opened FS_READ */
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 0), ERR_ACCESS_DENIED);
    CHECK_ST(file_read_until(f.ch, now() + FAT_CALL_NS, 0, FAT_BUF + 1, &done),
             ERR_INVALID_ARGS);
    CHECK_ST(file_stat_until(f.ch, now() + FAT_CALL_NS, &size, &mtime), OK);
    CHECK(size == 10 && mtime == FIXED_MTIME);
    t_close(&f);
    CHECK_ST(t_open(r, "/hello.txt", FS_WRITE, &f), OK);
    CHECK_ST(t_read(&f, 0, got, 5, &done), ERR_ACCESS_DENIED);    /* opened FS_WRITE */
    t_close(&f);
    return true;
}

/* fs.open's flags. */
static bool check_open_flags(const struct fatrun *r)
{
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(r, "/missing.txt", FS_READ, &f), ERR_NOT_FOUND);
    CHECK_ST(t_open(r, "/missing.txt", FS_WRITE, &f), ERR_NOT_FOUND);
    CHECK_ST(t_open(r, "/nodir/x.txt", FS_WRITE | FS_CREATE, &f), ERR_NOT_FOUND);
    CHECK_ST(t_open(r, "/hello.txt", 0, &f), ERR_INVALID_ARGS);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ | FS_CREATE, &f), ERR_INVALID_ARGS);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ | FS_TRUNCATE, &f), ERR_INVALID_ARGS);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ | 0x100, &f), ERR_INVALID_ARGS);
    CHECK_ST(t_open(r, "/", FS_READ, &f), ERR_WRONG_TYPE);
    /* FS_APPEND: at the end, whatever the offset says. */
    CHECK_ST(t_open(r, "/hello.txt", FS_WRITE | FS_APPEND, &f), OK);
    CHECK_ST(t_write(&f, 0, " more", 5, &done), OK);
    CHECK_ST(t_write(&f, 2, "!", 1, &done), OK);
    t_close(&f);
    if (!file_is(r, "/hello.txt", "hello, fat more!"))
        return false;
    /* FS_CREATE on a file that exists opens it as it is; FS_TRUNCATE empties it. */
    CHECK_ST(t_open(r, "/hello.txt", FS_WRITE | FS_CREATE, &f), OK);
    CHECK_EQ(f.size, 16);
    CHECK_ST(t_write(&f, 0, "J", 1, &done), OK);
    t_close(&f);
    if (!file_is(r, "/hello.txt", "Jello, fat more!"))
        return false;
    CHECK_ST(t_open(r, "/hello.txt", FS_READ | FS_WRITE | FS_TRUNCATE, &f), OK);
    CHECK_EQ(f.size, 0);
    CHECK_ST(t_write(&f, 0, "new", 3, &done), OK);
    t_close(&f);
    return file_is(r, "/hello.txt", "new");
}

/* A write or truncate past the end fills the gap with zeros, not with
 * what the clusters held before. */
static bool check_growth(const struct fatrun *r)
{
    static uint8_t junk[8192], got[8192];
    struct tfile f;
    uint32_t done = 0;
    uint64_t size = 0;
    memset(junk, 0xa5, sizeof(junk));
    CHECK_ST(t_open(r, "/junk.bin", FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(t_write(&f, 0, junk, sizeof(junk), &done), OK);
    t_close(&f);
    CHECK_ST(t_unlink(r, "/junk.bin"), OK);   /* its clusters are free, still full of 0xa5 */
    CHECK_ST(t_open(r, "/gap.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    CHECK_ST(t_write(&f, 5000, "end", 3, &done), OK);
    CHECK_ST(file_stat_until(f.ch, now() + FAT_CALL_NS, &size, NULL), OK);
    CHECK_EQ(size, 5003);
    CHECK_ST(t_read(&f, 0, got, 5003, &done), OK);
    CHECK_EQ(done, 5003);
    for (unsigned i = 0; i < 5000; i++)
        if (got[i])
            FAIL("byte %u of the gap is %#x, not 0", i, got[i]);
    CHECK(!memcmp(got + 5000, "end", 3));
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 100), OK);
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 300), OK);
    CHECK_ST(t_read(&f, 0, got, 1000, &done), OK);
    CHECK_EQ(done, 300);
    for (unsigned i = 0; i < 300; i++)
        CHECK(got[i] == 0);
    /* More than FAT_GROW_MAX (16 MiB) in one step is refused. */
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 1u << 30), ERR_OUT_OF_RANGE);
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 1ull << 32), ERR_OUT_OF_RANGE);
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), OK);
    t_close(&f);
    CHECK_ST(t_stat(r, "/gap.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, 300);
    return true;
}

/* A file of many clusters, written and read a buffer at a time. */
static bool check_big_file(const struct fatrun *r)
{
    enum { BIG = 200000 };
    static uint8_t chunk[FAT_BUF];
    struct tfile f;
    uint32_t done = 0;
    uint64_t size = 0;
    CHECK_ST(t_open(r, "/big.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    for (uint32_t off = 0; off < BIG; off += done) {
        uint32_t n = BIG - off < FAT_BUF ? BIG - off : FAT_BUF;
        for (uint32_t i = 0; i < n; i++)
            chunk[i] = (uint8_t)((off + i) * 31 >> 3);
        CHECK_ST(t_write(&f, off, chunk, n, &done), OK);
        CHECK_EQ(done, n);
    }
    for (uint32_t off = 0; off < BIG; off += done) {
        CHECK_ST(t_read(&f, off, chunk, FAT_BUF, &done), OK);
        CHECK_EQ(done, BIG - off < FAT_BUF ? BIG - off : FAT_BUF);
        for (uint32_t i = 0; i < done; i++)
            if (chunk[i] != (uint8_t)((off + i) * 31 >> 3))
                FAIL("byte %u of /big.bin is wrong", off + i);
    }
    t_close(&f);
    CHECK_ST(t_stat(r, "/big.bin", &size, NULL, NULL), OK);
    CHECK_EQ(size, BIG);
    return true;
}

/* An open file is locked: no second writer, no unlink, no rename; and the
 * table of open files has an end. */
static bool check_open_files(const struct fatrun *r)
{
    static struct tfile many[40];
    struct tfile f, g;
    unsigned n = 0;
    status_t st = OK;
    CHECK_ST(t_open(r, "/hello.txt", FS_WRITE, &f), OK);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ, &g), ERR_BAD_STATE);
    CHECK_ST(t_unlink(r, "/hello.txt"), ERR_BAD_STATE);
    CHECK_ST(t_rename(r, "/hello.txt", "/other.txt"), ERR_BAD_STATE);
    t_close(&f);
    /* Closed: the very next call finds it so. Two readers are fine. */
    CHECK_ST(t_open(r, "/hello.txt", FS_READ, &f), OK);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ, &g), OK);
    t_close(&f);
    t_close(&g);
    while (n < 40 && (st = t_open(r, "/hello.txt", FS_READ, &many[n])) == OK)
        n++;
    CHECK_ST(st, ERR_NO_RESOURCES);
    CHECK_EQ(n, 32);
    while (n)
        t_close(&many[--n]);
    CHECK_ST(t_open(r, "/hello.txt", FS_READ, &f), OK);
    t_close(&f);
    return true;
}

bool t_fat_files(void)
{
    struct fatrun r;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    if (!put_file(&r, "/hello.txt", "hello, fat"))
        return false;
    CHECK_EQ(fat_kind(&disk), 16);
    if (!check_reads(&r) || !check_open_flags(&r) || !check_growth(&r) || !check_big_file(&r) ||
        !check_open_files(&r))
        return false;
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* rename: within and across directories, and what it refuses. */
static bool check_rename(const struct fatrun *r)
{
    uint64_t size = 0;
    CHECK_ST(t_rename(r, "/docs/a.txt", "/docs/sub/c.txt"), OK);
    CHECK_ST(t_stat(r, "/docs/a.txt", &size, NULL, NULL), ERR_NOT_FOUND);
    if (!file_is(r, "/docs/sub/c.txt", "file a"))
        return false;
    CHECK_ST(t_rename(r, "/docs/sub/c.txt", "/docs/sub/b.txt"), ERR_ALREADY_EXISTS);
    CHECK_ST(t_rename(r, "/docs/nothing", "/docs/x"), ERR_NOT_FOUND);
    CHECK_ST(t_rename(r, "/docs/sub/c.txt", "/nodir/c.txt"), ERR_NOT_FOUND);
    /* A directory can't move into itself, however the path is spelled. */
    CHECK_ST(t_rename(r, "/docs", "/docs/sub/docs"), ERR_INVALID_ARGS);
    CHECK_ST(t_rename(r, "/DOCS", "/docs/SUB/x"), ERR_INVALID_ARGS);
    CHECK_ST(t_rename(r, "/docs", "/docs/./x/../y"), ERR_INVALID_ARGS);
    CHECK_ST(t_rename(r, "/", "/x"), ERR_INVALID_ARGS);
    CHECK_ST(t_rename(r, "/docs/sub", "/moved"), OK);   /* a directory, with what it holds */
    return file_is(r, "/moved/b.txt", "file b") && file_is(r, "/moved/c.txt", "file a");
}

bool t_fat_dirs(void)
{
    struct fatrun r;
    struct tfile f;
    char name[FS_PATH_MAX];
    uint64_t size = 9, before = 0, after = 0;
    bool dir = false, found = false;
    unsigned n = 0;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_free(&r, NULL, &before), OK);
    CHECK_ST(t_mkdir(&r, "/docs"), OK);
    CHECK_ST(t_mkdir(&r, "/docs"), ERR_ALREADY_EXISTS);
    CHECK_ST(t_mkdir(&r, "/"), ERR_ALREADY_EXISTS);
    CHECK_ST(t_mkdir(&r, "/nodir/sub"), ERR_NOT_FOUND);
    CHECK_ST(t_mkdir(&r, "/docs/sub"), OK);
    CHECK_ST(t_stat(&r, "/docs", &size, &dir, NULL), OK);
    CHECK(dir && size == 0);
    if (!put_file(&r, "/docs/a.txt", "file a") || !put_file(&r, "/docs/sub/b.txt", "file b"))
        return false;
    CHECK_ST(t_mkdir(&r, "/docs/a.txt"), ERR_ALREADY_EXISTS);
    CHECK_ST(t_open(&r, "/docs", FS_READ, &f), ERR_WRONG_TYPE);

    /* readdir: every entry once, "." and ".." left out. */
    if (!dir_count(&r, "/docs", "a.txt", &n, &found))
        return false;
    CHECK(n == 2 && found);
    CHECK_ST(t_readdir(&r, "/docs", 0, name, &dir), OK);
    CHECK(!strcmp(name, "sub") && dir);
    CHECK_ST(t_readdir(&r, "/docs", 1, name, &dir), OK);
    CHECK(!strcmp(name, "a.txt") && !dir);
    CHECK_ST(t_readdir(&r, "/docs", 2, name, &dir), ERR_NOT_FOUND);
    CHECK_ST(t_readdir(&r, "/docs", 0xffffffffu, name, &dir), ERR_NOT_FOUND);
    CHECK_ST(t_readdir(&r, "/docs/a.txt", 0, name, &dir), ERR_WRONG_TYPE);
    CHECK_ST(t_readdir(&r, "/nodir", 0, name, &dir), ERR_NOT_FOUND);

    /* "." and ".." are resolved, and ".." never leaves the volume. */
    if (!file_is(&r, "/docs/./sub/../a.txt", "file a") ||
        !file_is(&r, "//docs///a.txt", "file a") ||
        !file_is(&r, "/../../docs/sub/../../docs/a.txt", "file a"))
        return false;
    CHECK_ST(t_stat(&r, "/docs/sub/..", &size, &dir, NULL), OK);
    CHECK(dir);
    CHECK_ST(t_stat(&r, "/..", &size, &dir, NULL), OK);   /* the root itself */
    CHECK(dir);
    CHECK_ST(t_stat(&r, "docs", &size, &dir, NULL), ERR_INVALID_ARGS);   /* not absolute */
    CHECK_ST(t_stat(&r, "", &size, &dir, NULL), OK);                     /* the root */

    if (!check_rename(&r))
        return false;
    CHECK_ST(t_unlink(&r, "/moved"), ERR_BAD_STATE);   /* not empty */
    CHECK_ST(t_unlink(&r, "/"), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(&r, "/moved/nothing"), ERR_NOT_FOUND);
    CHECK_ST(t_unlink(&r, "/moved/b.txt"), OK);
    CHECK_ST(t_unlink(&r, "/moved/c.txt"), OK);
    CHECK_ST(t_unlink(&r, "/moved"), OK);
    CHECK_ST(t_unlink(&r, "/docs"), OK);
    if (!dir_count(&r, "/", NULL, &n, NULL))
        return false;
    CHECK_EQ(n, 0);
    CHECK_ST(t_free(&r, NULL, &after), OK);
    CHECK_EQ(after, before);   /* every cluster came back */
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* A full disk: what fits is written, then ERR_NO_SPACE; what was written
 * stays readable; removing a file makes room again. */
bool t_fat_full_disk(void)
{
    static uint8_t chunk[FAT_BUF];
    struct fatrun r;
    struct tfile f, g;
    uint64_t total = 0, free_bytes = 0, written = 0;
    uint32_t done = 0;
    status_t st = OK;
    if (!ramdisk_create(&disk, 1 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_free(&r, &total, &free_bytes), OK);
    CHECK_EQ(fat_kind(&disk), 12);
    CHECK(free_bytes > 900u << 10 && free_bytes <= total);
    memset(chunk, 0x5a, sizeof(chunk));
    CHECK_ST(t_open(&r, "/fill.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    while (written < (2u << 20) && (st = t_write(&f, written, chunk, FAT_BUF, &done)) == OK)
        written += done;
    CHECK_ST(st, ERR_NO_SPACE);
    CHECK_EQ(written, free_bytes);   /* the last write that fit was a short one */
    CHECK(done > 0 && done < FAT_BUF);
    CHECK_ST(t_free(&r, NULL, &free_bytes), OK);
    CHECK_EQ(free_bytes, 0);
    CHECK_ST(t_mkdir(&r, "/nodir"), ERR_NO_SPACE);
    /* A new name still fits in the root directory; its first byte doesn't. */
    CHECK_ST(t_open(&r, "/more.txt", FS_WRITE | FS_CREATE, &g), OK);
    CHECK_ST(t_write(&g, 0, "x", 1, &done), ERR_NO_SPACE);
    CHECK_ST(file_truncate_until(g.ch, now() + FAT_CALL_NS, 4096), ERR_NO_SPACE);
    t_close(&g);
    CHECK_ST(t_read(&f, written - 1000, chunk, 1000, &done), OK);
    CHECK_EQ(done, 1000);
    for (unsigned i = 0; i < 1000; i++)
        CHECK(chunk[i] == 0x5a);
    t_close(&f);
    CHECK_ST(t_unlink(&r, "/fill.bin"), OK);
    CHECK_ST(t_free(&r, NULL, &free_bytes), OK);
    CHECK_EQ(free_bytes, written);
    if (!put_file(&r, "/more.txt", "room again") || !file_is(&r, "/more.txt", "room again"))
        return false;
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* A read-only partition (the /esp instance): reading works, every change
 * is refused by fat itself (nothing reaches the disk), and a blank one is
 * not formatted. */
bool t_fat_read_only(void)
{
    struct fatrun r;
    struct tfile f;
    uint8_t ro = 0;
    uint32_t done = 0;
    unsigned n = 0;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_mkdir(&r, "/EFI"), OK);
    if (!put_file(&r, "/EFI/limine.conf", "timeout: 3") || !fat_stop(&r))
        return false;
    uint32_t writes = ramdisk_writes(&disk), syncs = ramdisk_syncs(&disk);

    if (!fat_start(&r, &disk, true))
        return false;
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, NULL, NULL, &ro, NULL), OK);
    CHECK_EQ(ro, 1);
    if (!file_is(&r, "/EFI/limine.conf", "timeout: 3") || !dir_count(&r, "/EFI", NULL, &n, NULL))
        return false;
    CHECK_EQ(n, 1);
    CHECK_ST(t_open(&r, "/EFI/limine.conf", FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&r, "/EFI/limine.conf", FS_READ | FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&r, "/new.txt", FS_WRITE | FS_CREATE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_mkdir(&r, "/newdir"), ERR_ACCESS_DENIED);
    CHECK_ST(t_unlink(&r, "/EFI/limine.conf"), ERR_ACCESS_DENIED);
    CHECK_ST(t_rename(&r, "/EFI", "/efi2"), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&r, "/EFI/limine.conf", FS_READ, &f), OK);
    CHECK_ST(t_write(&f, 0, "x", 1, &done), ERR_ACCESS_DENIED);
    CHECK_ST(file_truncate_until(f.ch, now() + FAT_CALL_NS, 0), ERR_ACCESS_DENIED);
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), OK);
    t_close(&f);
    CHECK_ST(t_sync(&r), OK);
    if (!file_is(&r, "/EFI/limine.conf", "timeout: 3") || !fat_stop(&r))
        return false;
    CHECK_EQ(ramdisk_writes(&disk), writes);   /* not one sector, not one flush */
    CHECK_EQ(ramdisk_syncs(&disk), syncs);
    if (!ramdisk_destroy(&disk))
        return false;

    /* No FAT volume and read-only: fat gives up, the disk untouched. */
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !gives_up(&disk, true))
        return false;
    CHECK_EQ(fat_kind(&disk), 0);
    return ramdisk_destroy(&disk);
}

/* Only a blank partition is formatted. A boot sector that is not a FAT
 * volume's (another filesystem), a FAT volume's that is damaged, and one
 * whose FAT size no real volume has (FatFs R0.16 would mount it:
 * CVE-2026-6682) all make fat give up with the disk as it was. */
bool t_fat_not_formatted(void)
{
    struct fatrun r;
    uint8_t saved[RAMDISK_SECTOR];
    if (!ramdisk_create(&disk, 40 * MIB_SECTORS))
        return false;
    memcpy(disk.mem + 3, "NTFS    ", 8);
    disk.mem[510] = 0x55;
    disk.mem[511] = 0xaa;
    if (!gives_up(&disk, false))
        return false;
    CHECK(!memcmp(disk.mem + 3, "NTFS    ", 8));

    memset(disk.mem, 0, RAMDISK_SECTOR);   /* blank again: formatted */
    if (!fat_start(&r, &disk, false) || !put_file(&r, "/kept.txt", "precious") || !fat_stop(&r))
        return false;
    memcpy(saved, disk.mem, RAMDISK_SECTOR);
    disk.mem[11] = disk.mem[12] = 0;       /* bytes per sector: 0 */
    if (!gives_up(&disk, false))
        return false;
    memcpy(disk.mem, saved, RAMDISK_SECTOR);
    disk.mem[13] = 64;                      /* sectors per cluster */
    memset(disk.mem + 32, 0xff, 4);         /* total sectors: 2^32 - 1 */
    memcpy(disk.mem + 36, "\x00\x00\x20\x00", 4);   /* sectors per FAT: 0x200000 */
    if (!gives_up(&disk, false))
        return false;

    memcpy(disk.mem, saved, RAMDISK_SECTOR);   /* repaired: the data was never touched */
    if (!fat_start(&r, &disk, false) || !file_is(&r, "/kept.txt", "precious") || !fat_stop(&r))
        return false;
    return ramdisk_destroy(&disk);
}

/* The dirty flag: clear on the disk while something written is not
 * flushed, set again by a sync; a volume left dirty (fat killed mid-way)
 * is mounted all the same, and is clean again after the next clean stop. */
static bool dirty_on(unsigned megabytes, unsigned kind)
{
    static uint8_t chunk[FAT_BUF];
    struct fatrun r;
    struct tfile f;
    struct process_info info;
    uint32_t done = 0;
    uint64_t size = 0;
    if (!ramdisk_create(&disk, megabytes * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_open(&r, "/log.txt", FS_WRITE | FS_CREATE, &f), OK);
    CHECK_EQ(fat_kind(&disk), kind);
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));
    memset(chunk, 'a', sizeof(chunk));
    CHECK_ST(t_write(&f, 0, chunk, FAT_BUF, &done), OK);
    CHECK(!clean_bit(&disk, 0) && !clean_bit(&disk, 1));   /* sectors went out: dirty */
    uint32_t syncs = ramdisk_syncs(&disk);
    CHECK_ST(file_sync_until(f.ch, now() + FAT_CALL_NS), OK);
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));
    CHECK(ramdisk_syncs(&disk) > syncs);                   /* block.sync: on the medium */
    CHECK_ST(t_write(&f, FAT_BUF, chunk, FAT_BUF, &done), OK);
    CHECK(!clean_bit(&disk, 0) && !clean_bit(&disk, 1));
    syncs = ramdisk_syncs(&disk);
    CHECK_ST(t_sync(&r), OK);                              /* fs.sync flushes open files too */
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));
    CHECK(ramdisk_syncs(&disk) > syncs);
    CHECK_ST(t_write(&f, 2 * FAT_BUF, chunk, FAT_BUF, &done), OK);

    /* The power goes: no flush, no clean stop. */
    CHECK_ST(jam_process_kill(r.proc), OK);
    CHECK_ST(spawn_wait(r.proc, FAT_CALL_NS, &info), OK);
    CHECK_ST(jam_handle_close(r.proc), OK);
    CHECK_ST(jam_handle_close(r.job), OK);
    CHECK_ST(jam_handle_close(r.fs), OK);
    t_close(&f);
    if (!ramdisk_join(&disk))
        return false;
    CHECK(!clean_bit(&disk, 0) && !clean_bit(&disk, 1));

    /* Mounted anyway (fat logs it): the file is there as last synced. */
    if (!fat_start(&r, &disk, false))
        return false;
    CHECK_ST(t_stat(&r, "/log.txt", &size, NULL, NULL), OK);
    CHECK_EQ(size, 2 * FAT_BUF);
    CHECK(!clean_bit(&disk, 0));   /* reading changes nothing */
    if (!put_file(&r, "/after.txt", "fine") || !fat_stop(&r))
        return false;
    CHECK(clean_bit(&disk, 0) && clean_bit(&disk, 1));
    return ramdisk_destroy(&disk);
}

bool t_fat_dirty_volume(void)
{
    return dirty_on(16, 16) && dirty_on(40, 32);
}

/* A disk error is ERR_IO; a disk that goes away ends fat (exit 0) and
 * with it every channel it served. */
bool t_fat_disk_gone(void)
{
    static uint8_t chunk[FAT_BUF];
    struct fatrun r;
    struct tfile f;
    uint32_t done = 0;
    uint64_t size = 0;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    if (!put_file(&r, "/a.txt", "abc"))
        return false;
    CHECK_ST(t_open(&r, "/b.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    ramdisk_fail_writes(&disk, true);
    CHECK_ST(t_write(&f, 0, chunk, FAT_BUF, &done), ERR_IO);
    CHECK_ST(t_mkdir(&r, "/d"), ERR_IO);
    ramdisk_fail_writes(&disk, false);
    if (!file_is(&r, "/a.txt", "abc"))   /* still serving */
        return false;

    if (!ramdisk_unplug(&disk))
        return false;
    CHECK_ST(jam_object_wait_one(r.fs, SIG_PEER_CLOSED, now() + FAT_CALL_NS, NULL), OK);
    CHECK_ST(t_stat(&r, "/a.txt", &size, NULL, NULL), ERR_PEER_CLOSED);
    CHECK_ST(t_read(&f, 0, chunk, 16, &done), ERR_PEER_CLOSED);
    t_close(&f);   /* its buffer is fat's memory until we unmap it */
    CHECK_ST(jam_handle_close(r.fs), OK);
    return fat_wait(&r, 0) && ramdisk_destroy(&disk);
}
