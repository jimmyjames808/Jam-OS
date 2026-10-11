/* pagertest write <path>: a mapped file that its own program changes
 * (fat's coherence: user/services/fat/pager.c's header). The file is made
 * (3 pages and 1000 bytes), mapped, then opened for writing (a file mapped
 * to read may be), and changed through that open in every way that
 * matters: a write inside a page, across two, a whole page, one growing
 * the last page, one past the end (a gap of zeros, the VMO grown past the
 * mapping), a truncate inside a page (its tail zero, the VMO shrunk under
 * the mapping) and one growing it back. After each change's answer the
 * mapping's bytes are loaded and compared with a model of the file, as far
 * as the file's VMO goes now (a load past it would be a bus error), and
 * the whole VMO is read with vmo_read too (past the mapping's end).
 * Prints "pagertest: write <path>: PASS", or the first difference and
 * MISMATCH. */
#include <fs_idl.h>
#include <os.h>
#include "pagertest.h"

#define W_MAX   (8u * PAGE_SIZE)            /* the model's room: the file stays below it */
#define W_FIRST (3u * PAGE_SIZE + 1000u)    /* the file's first size */

/* What the file holds: its first `size` bytes, zeros after them. */
static struct {
    uint8_t  bytes[W_MAX];
    uint64_t size;
} model;
static uint8_t got[W_MAX];

static uint64_t up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

/* n bytes (at most a page) of seed's pattern at off: into the file through
 * w, and into the model. */
static status_t put(struct jfile *w, uint64_t off, uint32_t n, uint32_t seed)
{
    static uint8_t bytes[PAGE_SIZE];
    size_t done = 0;
    for (uint32_t i = 0; i < n; i++)
        bytes[i] = (uint8_t)(seed * 41 + i * 13 + (i >> 9));
    status_t st = file_write(w, off, bytes, n, &done);
    if (st == OK && done != n)
        st = ERR_IO;
    memcpy(model.bytes + off, bytes, n);
    if (off + n > model.size)
        model.size = off + n;
    return st;
}

/* The file cut, or grown with zeros, to size. */
static status_t cut_to(struct jfile *w, uint64_t size)
{
    if (size < model.size)
        memset(model.bytes + size, 0, model.size - size);
    model.size = size;
    return file_truncate(w, size);
}

/* The mapping (loads, as far as the VMO goes now) and the VMO (vmo_read,
 * all of it) against the model. NULL, or what differs. */
static const char *differs(const struct mapped_file *mf)
{
    uint64_t size = 0, vmo_end = up(model.size);
    if (jam_vmo_get_size(mf->vmo, &size) != OK || size != vmo_end)
        return "the VMO's size is not the file's";
    uint64_t n = vmo_end < mf->map_len ? vmo_end : mf->map_len;
    for (uint64_t i = 0; i < n; i++)
        if (mf->data[i] != model.bytes[i])
            return "a byte loaded from the mapping is not the file's";
    if (size && (jam_vmo_read(mf->vmo, 0, got, size) != OK || memcmp(got, model.bytes, size)))
        return "the VMO's bytes are not the file's";
    return NULL;
}

/* One change through w, then the check; *step names it for the report. */
static status_t step(struct jfile *w, const struct mapped_file *mf, unsigned k,
                     const char **name, const char **why)
{
    static const char *const names[] = {
        "a write inside a page", "a write across two pages", "a whole page",
        "a write growing the last page", "a write past the end", "a truncate inside a page",
        "a truncate growing it",
    };
    *name = names[k];
    status_t st = k == 0   ? put(w, 100, 10, 1)
                  : k == 1 ? put(w, PAGE_SIZE - 100, 200, 2)
                  : k == 2 ? put(w, 2 * PAGE_SIZE, PAGE_SIZE, 3)
                  : k == 3 ? put(w, model.size, 500, 4)
                  : k == 4 ? put(w, 20000, 100, 5)
                  : k == 5 ? cut_to(w, 10000)
                           : cut_to(w, 15000);
    *why = st == OK ? differs(mf) : NULL;
    return st;
}

#define STEPS 7u

/* The file made, W_FIRST bytes. */
static status_t make(const char *path)
{
    struct jfile w = { 0 };
    status_t st = file_open(path, FS_READ | FS_WRITE | FS_CREATE | FS_TRUNCATE, &w);
    memset(&model, 0, sizeof(model));
    for (uint64_t at = 0; st == OK && at < W_FIRST; at += PAGE_SIZE)
        st = put(&w, at, W_FIRST - at < PAGE_SIZE ? (uint32_t)(W_FIRST - at) : PAGE_SIZE, 9);
    if (w.ch)
        file_close(&w);
    return st;
}

int pagertest_write(const char *path)
{
    struct mapped_file mf;
    struct jfile w = { 0 };
    const char *name = "made and mapped", *why = NULL;
    status_t st = make(path);
    if (st == OK)
        st = mapped_file_open(path, &mf);
    if (st != OK) {
        printf("pagertest: write %s: %s: %s\n", path, name, status_str(st));
        return 2;
    }
    why = differs(&mf);
    name = "opened for writing";
    if (!why)
        st = file_open(path, FS_READ | FS_WRITE, &w);
    for (unsigned k = 0; st == OK && !why && k < STEPS; k++)
        st = step(&w, &mf, k, &name, &why);
    if (w.ch)
        file_close(&w);
    mapped_file_close(&mf);
    if (st == OK && !why) {
        printf("pagertest: write %s: PASS (%u changes, each seen through the mapping)\n", path,
               STEPS);
        return 0;
    }
    printf("pagertest: write %s: %s: %s: MISMATCH\n", path, name, why ? why : status_str(st));
    return 1;
}
