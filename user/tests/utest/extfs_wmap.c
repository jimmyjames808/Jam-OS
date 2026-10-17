/* utest: writable shared mappings on extfs (user/services/extfs/pager.c;
 * docs/WRITEMAP-PLAN.md's WE1): one VMO per file, made writable, and one
 * cache for its mappings, read, write and truncate. extfs with a pager
 * over a RAM disk it formats. A store through a FILE_MAP_WRITE mapping is
 * seen by extfs's read, by a second mapping (FILE_MAP_READ: the same VMO,
 * its handle without RIGHT_WRITE) and by a second process, whose own store
 * is seen back; a write and a store on one page are both kept; a truncate
 * shrinks and grows with the last page's tail zero (a store past the end is
 * never the file's), and so does a write past the end; a map made after
 * the first mapper unmapped and closed, its pages still dirty, finds the
 * same VMO. FILE_MAP_WRITE is refused without FS_WRITE or FS_READ, through
 * a read-only view and on a file mapped to run (its open for writing is);
 * FILE_MAP_EXEC is refused while a writable map lives, and allowed once
 * it has gone. The stores are written back at the stop (WE2's
 * extfs_wback tests that), and e2fsck finds the disk whole. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <extfssvc.h>
#include <fs_idl.h>
#include <fsview.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

#define WM_FIRST  (3u * PAGE_SIZE + 1000u)   /* the file's first size */
#define WM_LEN    (4u * PAGE_SIZE)           /* our mappings: the first size's pages */
#define CHILD_AT  3000u                      /* the second process stores here ... */
#define CHILD_N   100u                       /* ... this many bytes */
#define OURS_AT   100u                       /* and checks ours here */
#define OURS_N    40u

/* What the file holds: its first `size` bytes, zeros after them. */
static struct {
    uint8_t  bytes[WM_LEN];
    uint64_t size;
} model;
static uint8_t got[WM_LEN];

static struct fatrun r;
static handle_t a, b;           /* the file's VMO: to write (A) and to read (B) */
static uint8_t *wm;             /* our writable mapping of A, WM_LEN bytes */
static const uint8_t *rm;       /* our mapping of B, WM_LEN bytes */

static uint64_t up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static uint64_t deadline(void)
{
    return now() + FAT_CALL_NS;
}

static void pattern(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed * 37 + i * 11 + (i >> 8) + 1);
}

/* n bytes of seed's pattern written at off through w, and into the model. */
static bool put(struct tfile *w, uint64_t off, uint32_t n, uint32_t seed)
{
    static uint8_t bytes[PAGE_SIZE];
    uint32_t done = 0;
    pattern(bytes, n, seed);
    CHECK_ST(t_write(w, off, bytes, n, &done), OK);
    CHECK_EQ(done, n);
    memcpy(model.bytes + off, bytes, n);   /* a gap before off is zeros in both */
    if (off + n > model.size)
        model.size = off + n;
    return true;
}

/* n bytes of seed's pattern stored at off through our writable mapping:
 * the file's bytes inside it, nobody's past its end. */
static void store(uint64_t off, uint32_t n, uint32_t seed)
{
    uint8_t bytes[PAGE_SIZE];
    pattern(bytes, n, seed);
    memcpy(wm + off, bytes, n);
    if (off < model.size)
        memcpy(model.bytes + off, bytes, off + n <= model.size ? n : model.size - off);
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

/* extfs's own read through f: the model, whole and from an odd place
 * across a page's end. */
static bool read_is_model(struct tfile *f, const char *step)
{
    uint32_t done = 0;
    CHECK_ST(t_read(f, 0, got, (uint32_t)model.size, &done), OK);
    CHECK_EQ(done, model.size);
    if (memcmp(got, model.bytes, model.size))
        FAIL("%s: extfs's read is not the file's bytes", step);
    uint64_t off = PAGE_SIZE - 7;
    uint32_t n = model.size <= off ? 0 : model.size - off < 300 ? (uint32_t)(model.size - off) : 300;
    if (!n)
        return true;
    CHECK_ST(t_read(f, off, got, n, &done), OK);
    CHECK_EQ(done, n);
    if (memcmp(got, model.bytes + off, n))
        FAIL("%s: extfs's read across a page's end is not the file's bytes", step);
    return true;
}

/* VMO v and mapping m (NULL: none) are the model: the file's size in whole
 * pages, its bytes and zeros to the end of its last page. */
static bool vmo_is_model(handle_t v, const uint8_t *m, const char *step, const char *which)
{
    uint64_t size = 0;
    CHECK_ST(jam_vmo_get_size(v, &size), OK);
    if (size != up(model.size))
        FAIL("%s: %s is %lu bytes, the file %lu", step, which, (unsigned long)size,
             (unsigned long)model.size);
    CHECK_ST(jam_vmo_read(v, 0, got, size), OK);
    if (memcmp(got, model.bytes, size))
        FAIL("%s: %s's pages are not the file's bytes", step, which);
    if (m && memcmp(m, model.bytes, size))
        FAIL("%s: our mapping of %s is not the file's bytes", step, which);
    CHECK_ST(jam_vmo_read(v, size, got, 1), ERR_OUT_OF_RANGE);
    return true;
}

/* extfs's read, A and B, and both mappings: all the model. */
static bool all_right(struct tfile *f, const char *step)
{
    return read_is_model(f, step) && vmo_is_model(a, wm, step, "A") &&
           vmo_is_model(b, rm, step, "B");
}

static uint64_t koid_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.koid : 0;
}

/* The second process (mode wmap-store, A's handle as SR_USER): our store
 * seen, its own made. */
int wmap_child(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t v = startup_handle(SR_USER);
    uint64_t addr = 0;
    uint8_t want[CHILD_N];
    if (!v || jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, WM_LEN, VMAR_READ | VMAR_WRITE,
                           &addr) != OK)
        return 2;
    uint8_t *m = (uint8_t *)(uintptr_t)addr;
    pattern(want, OURS_N, 2);
    if (memcmp(m + OURS_AT, want, OURS_N))
        return 1;
    pattern(want, CHILD_N, 3);
    memcpy(m + CHILD_AT, want, CHILD_N);
    return 0;
}

/* A store seen by read, by a second mapping and by a second process, and
 * that process's store seen here. */
static bool stores_seen(struct tfile *w)
{
    uint64_t size = 0, addr = 0;
    struct handle_info hi;
    memset(&model, 0, sizeof(model));
    for (uint64_t at = 0; at < WM_FIRST; at += PAGE_SIZE)
        if (!put(w, at, WM_FIRST - at < PAGE_SIZE ? (uint32_t)(WM_FIRST - at) : PAGE_SIZE, 1))
            return false;
    CHECK_ST(file_map_until(w->ch, deadline(), FILE_MAP_WRITE, &a, &size), OK);
    CHECK_EQ(size, WM_FIRST);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), a, 0, WM_LEN, VMAR_READ | VMAR_WRITE,
                          &addr), OK);
    wm = (uint8_t *)(uintptr_t)addr;
    store(OURS_AT, OURS_N, 2);
    store(2 * PAGE_SIZE - 20, 40, 2);   /* across a page's end */
    store(WM_FIRST - 30, 30, 2);        /* the last bytes */
    if (!read_is_model(w, "stored"))
        return false;

    /* The same VMO to read: no write right, and it can't be mapped to
     * write; its pages are the stores. */
    CHECK_ST(file_map_until(w->ch, deadline(), FILE_MAP_READ, &b, &size), OK);
    CHECK_EQ(koid_of(b), koid_of(a));
    CHECK_ST(jam_handle_info(b, &hi), OK);
    CHECK(!(hi.rights & RIGHT_WRITE));
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), b, 0, WM_LEN, VMAR_READ | VMAR_WRITE,
                          &addr), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), b, 0, WM_LEN, VMAR_READ, &addr), OK);
    rm = (const uint8_t *)(uintptr_t)addr;
    if (!all_right(w, "seen by a second mapping"))
        return false;

    /* A second process, given A. */
    handle_t job = HANDLE_INVALID, proc = HANDLE_INVALID, dup = HANDLE_INVALID;
    struct process_info info;
    CHECK_ST(jam_handle_duplicate(a, RIGHT_SAME, &dup), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("wmap-store", NULL, job, dup, &proc), OK);
    CHECK_ST(spawn_wait(proc, 20 * NS_PER_S, &info), OK);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    if (info.exit_code)
        FAIL("the second process: %s", info.exit_code == 1 ? "our store not seen"
                                                           : "A couldn't be mapped to write");
    uint8_t theirs[CHILD_N];
    pattern(theirs, CHILD_N, 3);
    memcpy(model.bytes + CHILD_AT, theirs, CHILD_N);
    return all_right(w, "the second process's store");
}

/* A write and stores on one page: both kept, the later where they
 * overlap. */
static bool write_and_store(struct tfile *w)
{
    store(PAGE_SIZE + 200, 60, 4);
    if (!put(w, PAGE_SIZE + 300, 50, 5) || !all_right(w, "a write beside a store"))
        return false;
    if (!put(w, PAGE_SIZE + 240, 40, 6) || !all_right(w, "a write over a store"))
        return false;
    store(PAGE_SIZE + 320, 10, 7);
    return all_right(w, "a store over a write");
}

/* Truncates and a write past the end: the tails zero, a store past the
 * end never the file's, dirty pages past a shrink gone. */
static bool resized(struct tfile *w)
{
    uint64_t end = model.size;
    store(end + 10, 40, 8);   /* past the end: nobody's */
    if (!read_is_model(w, "a store past the end") || !cut_to(w, end + 500) ||
        !all_right(w, "grown by a truncate"))
        return false;
    store(PAGE_SIZE + 1000, 100, 9);
    store(2 * PAGE_SIZE + 100, 100, 9);   /* a dirty page the shrink drops */
    if (!cut_to(w, 6000) || !all_right(w, "shrunk by a truncate"))
        return false;
    CHECK_ST(jam_vmo_read(a, 2 * PAGE_SIZE, got, 1), ERR_OUT_OF_RANGE);
    if (!cut_to(w, 15000) || !all_right(w, "grown again: zeros, not the dropped stores"))
        return false;
    store(15010, 20, 10);   /* past the end again */
    return put(w, 15900, 100, 11) && all_right(w, "a write past the end");
}

/* Mapped, stored to, unmapped and closed (nobody has the VMO but the
 * kernel's hold of its dirty pages): the next map finds the same VMO and
 * its stores, and so does a read. */
static bool found_again(void)
{
    struct tfile f;
    handle_t v = HANDLE_INVALID;
    uint64_t size = 0, addr = 0, koid = 0;
    uint8_t mine[100];
    memset(&model, 0, sizeof(model));
    CHECK_ST(t_open(&r, "/u.bin", FS_READ | FS_WRITE | FS_CREATE, &f), OK);
    if (!put(&f, 0, PAGE_SIZE, 12) || !put(&f, PAGE_SIZE, 100, 13))
        return false;
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_WRITE, &v, &size), OK);
    koid = koid_of(v);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, 2 * PAGE_SIZE,
                          VMAR_READ | VMAR_WRITE, &addr), OK);
    pattern(mine, sizeof(mine), 14);
    memcpy((uint8_t *)(uintptr_t)addr + 500, mine, sizeof(mine));
    memcpy(model.bytes + 500, mine, sizeof(mine));
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, 2 * PAGE_SIZE), OK);
    CHECK_ST(jam_handle_close(v), OK);
    t_close(&f);
    /* extfs's next request: a GONE would be served first (fs.stat: fs.sync
     * would write the stores back, and the clean VMO go) */
    CHECK_ST(t_stat(&r, "/u.bin", NULL, NULL, NULL), OK);
    CHECK_ST(t_open(&r, "/u.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_READ, &v, &size), OK);
    CHECK_EQ(size, model.size);
    CHECK_EQ(koid_of(v), koid);
    CHECK_ST(jam_vmo_read(v, 0, got, model.size), OK);
    bool ok = !memcmp(got, model.bytes, model.size) && read_is_model(&f, "found again");
    jam_handle_close(v);
    t_close(&f);
    if (!ok)
        FAIL("a map after the first mapper went doesn't have its stores");
    return true;
}

/* FILE_MAP_WRITE needs an open that may write and read, through a view
 * that may change the file, and never of a file mapped to run;
 * FILE_MAP_EXEC never while a writable map lives. */
static bool refused(void)
{
    struct tfile f, g;
    struct fatrun v;
    handle_t x = HANDLE_INVALID, ch = HANDLE_INVALID;
    uint64_t size = 0;
    if (!put_file(&r, "/x.bin", "a program, say"))
        return false;
    CHECK_ST(t_open(&r, "/x.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_WRITE, &x, &size), ERR_ACCESS_DENIED);
    t_close(&f);
    CHECK_ST(t_open(&r, "/x.bin", FS_WRITE, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_WRITE, &x, &size), ERR_ACCESS_DENIED);
    t_close(&f);
    CHECK_ST(fs_view_until(r.fs, deadline(), FS_VIEW_READ_ONLY, &ch), OK);
    v = r;
    v.fs = ch;
    CHECK_ST(t_open(&v, "/x.bin", FS_READ | FS_WRITE, &f), ERR_ACCESS_DENIED);
    CHECK_ST(t_open(&v, "/x.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_WRITE, &x, &size), ERR_ACCESS_DENIED);
    t_close(&f);
    jam_handle_close(ch);

    /* Mapped to run: no open for writing, so no map to write. */
    CHECK_ST(t_open(&r, "/x.bin", FS_READ, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_EXEC, &x, &size), OK);
    t_close(&f);
    CHECK_ST(t_open(&r, "/x.bin", FS_READ | FS_WRITE, &f), ERR_BUSY);
    CHECK_ST(jam_handle_close(x), OK);

    /* Mapped to write (no store): not to run while that lives, even once
     * nobody has it open for writing; to run once it has gone. */
    CHECK_ST(t_open(&r, "/x.bin", FS_READ | FS_WRITE, &f), OK);
    CHECK_ST(file_map_until(f.ch, deadline(), FILE_MAP_WRITE, &x, &size), OK);
    t_close(&f);
    CHECK_ST(t_open(&r, "/x.bin", FS_READ, &g), OK);
    handle_t y = HANDLE_INVALID;
    CHECK_ST(file_map_until(g.ch, deadline(), FILE_MAP_EXEC, &y, &size), ERR_BUSY);
    t_close(&g);
    CHECK_ST(jam_handle_close(x), OK);
    CHECK_ST(t_sync(&r), OK);   /* its GONE first */
    CHECK_ST(t_open(&r, "/x.bin", FS_READ, &g), OK);
    CHECK_ST(file_map_until(g.ch, deadline(), FILE_MAP_EXEC, &y, &size), OK);
    t_close(&g);
    CHECK_ST(jam_handle_close(y), OK);
    return true;
}

bool t_extfs_wmap(void)
{
    static struct ramdisk rd;
    handle_t root = startup_handle(SR_RESOURCE), pager = HANDLE_INVALID;
    struct tfile w;
    if (!root || jam_pager_create(root, 4096, FAT_CALL_NS, 0, &pager) != OK) {
        printf("utest: extfs_wmap: no pager power (RIGHT_ROOT_PAGER): skipped\n");
        return true;
    }
    a = b = HANDLE_INVALID;
    wm = NULL;
    rm = NULL;
    bool ok = ramdisk_create(&rd, EXT_DISK_SECTORS) && extfs_start_pager(&r, &rd, pager) &&
              t_open(&r, "/w.bin", FS_READ | FS_WRITE | FS_CREATE, &w) == OK;
    bool opened = ok;
    ok = ok && stores_seen(&w) && write_and_store(&w) && resized(&w);
    if (opened)
        t_close(&w);
    ok = ok && found_again() && refused();
    handle_t self = startup_handle(SR_SELF_VMAR);
    if (wm)   /* our own mappings: the unmaps can't fail */
        (void)jam_vmar_unmap(self, (uint64_t)(uintptr_t)wm, WM_LEN);
    if (rm)
        (void)jam_vmar_unmap(self, (uint64_t)(uintptr_t)rm, WM_LEN);
    if (a)
        jam_handle_close(a);
    if (b)
        jam_handle_close(b);
    ok = ok && fat_stop(&r) && extfs_save(&rd, "wmap");
    CHECK_ST(jam_handle_close(pager), OK);   /* the stores never written back go with it */
    return ramdisk_destroy(&rd) && ok;
}
