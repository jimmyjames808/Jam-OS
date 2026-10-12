/* utest: a mapped file kept right while its file changes (fat's pager.c,
 * fileops.c and fsops.c; docs/M12.5-PLAN.md, "Writes and coherence"). A
 * fat with a pager over a RAM disk serves one file mapped twice: A, mapped
 * while nobody had it open (as libos's file_map_path maps), and B, mapped
 * through the writer that opened it afterwards. After every change is
 * answered, both VMOs read as a model of the file (vmo_read waits for the
 * pages as a fault does), a system call's copy from our mapping of A does
 * too, and so does fat's own read. The changes: a write inside a page,
 * across two, a whole page, one growing the last page, one past the end
 * (the gap zeros, the VMOs grown), a truncate inside a page (its tail
 * zero; past the end ERR_OUT_OF_RANGE, and the copy refused where a load
 * would be a bus error) and one growing it back; the file renamed under
 * its mappings (still right, and a write through its new name reaches
 * them); an FS_TRUNCATE open. Then a file mapped to run (FILE_MAP_EXEC)
 * refuses every change while that VMO lives, and a mapped file renamed
 * over, or removed, is cut off (its pages ERR_IO, as for a pulled
 * stick). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fatsvc.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

#define CO_DISK   (16u * MIB_SECTORS)
#define CO_MAX    (8u * PAGE_SIZE)            /* the model's room: the file stays below it */
#define CO_FIRST  (3u * PAGE_SIZE + 1000u)    /* the file's first size */
#define CO_MAPPED (4u * PAGE_SIZE)            /* our mapping of A: the first size's pages */
#define OTHER     "another file"              /* what renames over it */

/* What the file holds: its first `size` bytes, zeros after them. */
static struct {
    uint8_t  bytes[CO_MAX];
    uint64_t size;
} model;
static uint8_t got[CO_MAX];

static struct fatrun r;
static const char *path_now;    /* the file's name now */
static handle_t a, b;           /* its VMOs: A (mapped first) and B (through the writer) */
static handle_t sink;           /* a VMO of ours, CO_MAPPED bytes: the copies' target */
static const uint8_t *map;      /* our mapping of A, CO_MAPPED bytes */

static uint64_t up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t deadline(void)
{
    return now() + FAT_CALL_NS;
}

/* n bytes (at most a page) of seed's pattern at off, into the file through
 * w and into the model. */
static bool put(struct tfile *w, uint64_t off, uint32_t n, uint32_t seed)
{
    static uint8_t bytes[PAGE_SIZE];
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++)
        bytes[i] = (uint8_t)(seed * 37 + i * 11 + (i >> 8));
    CHECK_ST(t_write(w, off, bytes, n, &done), OK);
    CHECK_EQ(done, n);
    memcpy(model.bytes + off, bytes, n);   /* a gap before off is zeros in both */
    if (off + n > model.size)
        model.size = off + n;
    return true;
}

/* The file cut (or grown with zeros) to size through w, and the model. */
static bool cut_to(struct tfile *w, uint64_t size)
{
    CHECK_ST(file_truncate_until(w->ch, deadline(), size), OK);
    if (size < model.size)
        memset(model.bytes + size, 0, model.size - size);
    model.size = size;
    return true;
}

/* vmo is the model: the file's size in whole pages, its bytes and zeros
 * to the end of its last page, nothing past that. */
static bool vmo_is_model(handle_t vmo, const char *step, const char *which)
{
    uint64_t size = 0;
    CHECK_ST(jam_vmo_get_size(vmo, &size), OK);
    if (size != up(model.size))
        FAIL("%s: %s is %lu bytes, the file %lu", step, which, (unsigned long)size,
             (unsigned long)model.size);
    if (size)
        CHECK_ST(jam_vmo_read(vmo, 0, got, size), OK);
    if (memcmp(got, model.bytes, size))
        FAIL("%s: %s's pages are not the file's bytes", step, which);
    CHECK_ST(jam_vmo_read(vmo, size, got, 1), ERR_OUT_OF_RANGE);
    return true;
}

/* A system call's copy from our mapping of A (vmo_write's source is it):
 * the model up to the VMO's end, refused past it. */
static bool copy_is_model(const char *step)
{
    uint64_t n = up(model.size) < CO_MAPPED ? up(model.size) : CO_MAPPED;
    if (n) {
        CHECK_ST(jam_vmo_write(sink, 0, map, n), OK);
        CHECK_ST(jam_vmo_read(sink, 0, got, n), OK);
        if (memcmp(got, model.bytes, n))
            FAIL("%s: a copy from the mapping is not the file's bytes", step);
    }
    if (n < CO_MAPPED)
        CHECK_ST(jam_vmo_write(sink, 0, map + n, 1), ERR_INVALID_ARGS);
    return true;
}

/* After a change was answered: fat's own read, A, B and the copy are all
 * the model. */
static bool all_right(const char *step)
{
    struct tfile f;
    uint32_t done = 0;
    CHECK_ST(t_open(&r, path_now, FS_READ, &f), OK);
    status_t st = model.size ? t_read(&f, 0, got, (uint32_t)model.size, &done) : OK;
    uint64_t size = f.size;
    t_close(&f);
    CHECK_ST(st, OK);
    CHECK_EQ(size, model.size);
    CHECK_EQ(done, model.size);
    if (memcmp(got, model.bytes, model.size))
        FAIL("%s: fat reads other bytes than were written", step);
    return vmo_is_model(a, step, "A") && vmo_is_model(b, step, "B") && copy_is_model(step);
}

/* Every kind of write and truncate through w. */
static bool changes(struct tfile *w)
{
    return put(w, 100, 10, 1) && all_right("a write inside a page") &&
           put(w, PAGE_SIZE - 100, 200, 2) && all_right("a write across two pages") &&
           put(w, 2 * PAGE_SIZE, PAGE_SIZE, 3) && all_right("a whole page") &&
           put(w, model.size, 500, 4) && all_right("a write growing the last page") &&
           put(w, 20000, 100, 5) && all_right("a write past the end") &&
           cut_to(w, 10000) && all_right("a truncate inside a page") &&
           cut_to(w, 15000) && all_right("a truncate growing it");
}

/* The file made, A mapped with nobody holding it open, then opened for
 * writing (not refused: only mapped to read), B mapped through the writer
 * (to read; to run it is refused), and every change. */
static bool mapped_then_written(void)
{
    struct tfile f, w;
    uint64_t size = 0, addr = 0;
    handle_t x = HANDLE_INVALID;
    memset(&model, 0, sizeof(model));
    path_now = "/c.bin";
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE | FS_CREATE, &w), OK);
    for (uint64_t at = 0; at < CO_FIRST; at += PAGE_SIZE)
        if (!put(&w, at, CO_FIRST - at < PAGE_SIZE ? (uint32_t)(CO_FIRST - at) : PAGE_SIZE, 9))
            return false;
    t_close(&w);
    CHECK_ST(t_open(&r, path_now, FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_READ, &a, &size), OK);
    t_close(&f);
    CHECK_EQ(size, CO_FIRST);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), a, 0, CO_MAPPED, VMAR_READ, &addr), OK);
    map = (const uint8_t *)(uintptr_t)addr;
    CHECK_ST(jam_vmo_create(CO_MAPPED, 0, &sink), OK);
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE, &w), OK);
    CHECK_ST(file_map_until(w.ch, deadline(), FILE_MAP_READ, &b, &size), OK);
    CHECK_ST(file_map_until(w.ch, deadline(), FILE_MAP_EXEC, &x, &size), ERR_BUSY);
    if (!all_right("mapped") || !changes(&w))
        return false;
    t_close(&w);
    return all_right("its writer gone");
}

/* Renamed under its mappings: still right, and the file its new name
 * reaches is the one they map (a write through it reaches them; the old
 * name is gone). Then an FS_TRUNCATE open empties it under them. */
static bool renamed(void)
{
    struct tfile w;
    CHECK_ST(t_rename(&r, "/c.bin", "/d.bin"), OK);
    path_now = "/d.bin";
    if (!all_right("renamed"))
        return false;
    CHECK_ST(t_open(&r, "/c.bin", FS_READ, &w), ERR_NOT_FOUND);
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE, &w), OK);
    if (!put(&w, 50, 20, 6) || !all_right("written through its new name"))
        return false;
    t_close(&w);
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE | FS_TRUNCATE, &w), OK);
    memset(model.bytes, 0, model.size);
    model.size = 0;
    if (!all_right("emptied by an open") || !put(&w, 0, 3000, 7) ||
        !all_right("written again"))
        return false;
    t_close(&w);
    return true;
}

/* Mapped to run (a program's, docs/M12.5-PLAN.md Q7): every change
 * ERR_BUSY while that VMO lives, and allowed once it has gone (fat hears
 * of it before its next fs request). */
static bool running(void)
{
    struct tfile f, w;
    handle_t x = HANDLE_INVALID;
    uint64_t size = 0;
    CHECK_ST(t_open(&r, path_now, FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_EXEC, &x, &size), OK);
    t_close(&f);
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE, &w), ERR_BUSY);
    CHECK_ST(t_unlink(&r, path_now), ERR_BUSY);
    CHECK_ST(t_rename(&r, path_now, "/e.bin"), ERR_BUSY);
    if (!put_file(&r, "/o.bin", OTHER))
        return false;
    CHECK_ST(t_rename_flags(&r, "/o.bin", path_now, FS_RENAME_REPLACE), ERR_BUSY);
    CHECK_ST(jam_handle_close(x), OK);
    CHECK_ST(t_open(&r, path_now, FS_READ | FS_WRITE, &w), OK);
    t_close(&w);
    return all_right("its program's mapping gone");
}

/* Renamed over, then removed, under a mapping: cut off. */
static bool removed(void)
{
    struct tfile f;
    handle_t c = HANDLE_INVALID;
    uint64_t size = 0;
    CHECK_ST(t_rename_flags(&r, "/o.bin", path_now, FS_RENAME_REPLACE), OK);
    CHECK_ST(jam_vmo_read(a, 0, got, 1), ERR_IO);
    CHECK_ST(jam_vmo_read(b, 0, got, 1), ERR_IO);
    CHECK_ST(jam_vmo_write(sink, 0, map, 1), ERR_IO);
    CHECK_ST(t_open(&r, path_now, FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_READ, &c, &size), OK);
    t_close(&f);
    CHECK_EQ(size, sizeof(OTHER) - 1);
    CHECK_ST(jam_vmo_read(c, 0, got, size), OK);
    CHECK(!memcmp(got, OTHER, size));
    CHECK_ST(t_unlink(&r, path_now), OK);
    CHECK_ST(jam_vmo_read(c, 0, got, 1), ERR_IO);
    CHECK_ST(jam_handle_close(c), OK);
    return true;
}

bool t_fat_map_coherent(void)
{
    static struct ramdisk rd;
    handle_t root = startup_handle(SR_RESOURCE), pager = HANDLE_INVALID;
    if (!root || jam_pager_create(root, 4096, FAT_CALL_NS, 0, &pager) != OK) {
        printf("utest: fat_map_coherent: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    a = b = sink = HANDLE_INVALID;
    map = NULL;
    bool ok = ramdisk_create(&rd, CO_DISK) && fat_start_pager(&r, &rd, pager) &&
              mapped_then_written() && renamed() && running() && removed();
    if (map)   /* our own mapping: the unmap can't fail */
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map, CO_MAPPED);
    const handle_t hs[] = { a, b, sink };
    for (unsigned i = 0; i < 3; i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    ok = ok && fat_stop(&r);
    CHECK_ST(jam_handle_close(pager), OK);
    return ramdisk_destroy(&rd) && ok;
}
