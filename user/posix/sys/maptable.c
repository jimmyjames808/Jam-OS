/* libc's table of its own mappings (maptable.h): a sorted array of
 * entries, in a VMO of its own mapped on first use (malloc can't hold it:
 * musl's malloc is built on these very mappings). Its size is the kernel's
 * cap on an address space's mappings, so the table is never the limit:
 * VMAR_MAX_MAPPINGS entries of 40 bytes, 2.5 MiB of address space whose
 * pages are committed as the table grows. Lookups are binary searches;
 * an insert or a removal moves the entries after it.
 *
 * fork (docs/FORK-PLAN.md section 1.6): a forked child's entries need
 * handles of its own to the VMOs its copy of the address space maps. The
 * table's VMOs are of three sorts by how vmar_clone has them: copied (it
 * makes one copy-on-write child of each, whose handle the child's entries
 * of that VMO get), shared (MAP_SHARED, and a file's own pages: a
 * duplicate) and left out (JIT memory: mem.c's step removes its entries).
 * mt_fork_prepare lists the copied ones and the shared ones, each VMO
 * once, sorted by the handle's value, in a room of their own (mapped at
 * the first fork, 1 MiB of address space committed as used): it runs in
 * the parent before the clone, so the child's copy of the room holds the
 * same lists, and each of its entries finds its VMO's place there. */
#include <stdlib.h>
#include <posixfork.h>
#include "maptable.h"

#define MT_MAX VMAR_MAX_MAPPINGS

_Static_assert(sizeof(struct mt_entry) == 40, "no padding in a table entry");

static struct mt_entry *table;   /* MT_MAX entries, or NULL before first use */
static unsigned count;           /* entries in use */

struct mt_entry *mt_entries(void)
{
    return table;
}

unsigned mt_count(void)
{
    return count;
}

/* Map the table's VMO. */
static bool make_table(void)
{
    uint64_t size = (uint64_t)MT_MAX * sizeof(struct mt_entry);
    handle_t vmo;
    if (jam_vmo_create(size, 0, &vmo) != OK)
        return false;
    uint64_t addr = 0;
    status_t st = jam_vmar_map(lx_self_vmar(), vmo, 0, size, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return false;
    table = (struct mt_entry *)(uintptr_t)addr;
    return true;
}

bool mt_room(unsigned n)
{
    if (!table && !make_table())
        return false;
    return n <= MT_MAX - count;
}

unsigned mt_first_after(uint64_t addr)
{
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (table[mid].base + table[mid].len > addr)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

void mt_insert(const struct mt_entry *e)
{
    unsigned i = mt_first_after(e->base);
    memmove(&table[i + 1], &table[i], (count - i) * sizeof(*table));
    table[i] = *e;
    count++;
}

bool mt_vmo_shared(unsigned i)
{
    for (unsigned j = 0; j < count; j++)
        if (j != i && table[j].vmo == table[i].vmo)
            return true;
    return false;
}

void mt_remove(unsigned i)
{
    if (!mt_vmo_shared(i))
        jam_handle_close(table[i].vmo);
    memmove(&table[i], &table[i + 1], (count - i - 1) * sizeof(*table));
    count--;
}

/* ---- fork ----------------------------------------------------------------------- */

/* The room's four lists, MT_MAX handles each. */
#define L_COPIES   0   /* the VMOs mapped as copies */
#define L_CHILDREN 1   /* vmar_clone's children of them (the parent's until passed; the
                        * child's own) */
#define L_SHARES   2   /* the VMOs mapped shared */
#define L_DUPS     3   /* the child's duplicates of them */
#define L_COUNT    4

static handle_t *room;               /* L_COUNT * MT_MAX handles; NULL before the first fork */
static uint32_t  ncopies, nshares;   /* in the lists now */

static handle_t *list(unsigned which)
{
    return room + (size_t)which * MT_MAX;
}

static bool make_room(void)
{
    uint64_t size = (uint64_t)L_COUNT * MT_MAX * sizeof(handle_t), addr = 0;
    handle_t vmo;
    if (jam_vmo_create(size, 0, &vmo) != OK)
        return false;
    status_t st = jam_vmar_map(lx_self_vmar(), vmo, 0, size, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return false;
    room = (handle_t *)(uintptr_t)addr;
    return true;
}

/* MAP_SHARED, and a file's own pages mapped read-only (mem.c maps both
 * VMAR_CLONE_SHARE). */
static bool shared_kind(uint32_t kind)
{
    return kind & (MT_SHARED | MT_PAGER_PRIVATE);
}

static int by_value(const void *a, const void *b)
{
    handle_t x = *(const handle_t *)a, y = *(const handle_t *)b;
    return x < y ? -1 : x > y;
}

/* The VMOs of the entries of one sort (shared or copied) into l, each
 * once, sorted: how many. */
static uint32_t list_of(handle_t *l, bool shared)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < count; i++)
        if (table[i].vmo && !(table[i].kind & MT_JIT) && shared_kind(table[i].kind) == shared)
            l[n++] = table[i].vmo;
    qsort(l, n, sizeof(*l), by_value);   /* musl's: in place, no allocation */
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!k || l[k - 1] != l[i])
            l[k++] = l[i];
    return k;
}

bool mt_fork_prepare(const handle_t **vmos, handle_t **children, uint32_t *n)
{
    if (!room && !make_room())
        return false;
    ncopies = list_of(list(L_COPIES), false);
    nshares = list_of(list(L_SHARES), true);
    *vmos = list(L_COPIES);
    *children = list(L_CHILDREN);
    *n = ncopies;
    return true;
}

/* h's place in the sorted list l of n, or n. */
static uint32_t place(const handle_t *l, uint32_t n, handle_t h)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (l[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && l[lo] == h ? lo : n;
}

void mt_fork_walk(struct lx_fork_io *io)
{
    handle_t *copies = list(L_COPIES), *children = list(L_CHILDREN);
    handle_t *shares = list(L_SHARES), *dups = list(L_DUPS);
    if (!lx_fork_child(io)) {
        for (uint32_t i = 0; i < ncopies; i++) {
            lx_fork_put(io, POSIXFORK_KEY(POSIXFORK_COPY, i, 0), children[i]);
            children[i] = HANDLE_INVALID;   /* the child's now */
        }
        for (uint32_t i = 0; i < nshares; i++)
            lx_fork_dup(io, POSIXFORK_KEY(POSIXFORK_SHARE, i, 0), shares[i]);
        return;
    }
    for (uint32_t i = 0; i < ncopies; i++)
        children[i] = lx_fork_take(io, POSIXFORK_KEY(POSIXFORK_COPY, i, 0));
    for (uint32_t i = 0; i < nshares; i++)
        dups[i] = lx_fork_take(io, POSIXFORK_KEY(POSIXFORK_SHARE, i, 0));
    for (unsigned i = 0; i < count; i++) {
        struct mt_entry *e = &table[i];
        bool shared = shared_kind(e->kind);
        uint32_t n = shared ? nshares : ncopies;
        uint32_t k = place(shared ? shares : copies, n, e->vmo);
        e->vmo = k < n ? (shared ? dups : children)[k] : HANDLE_INVALID;
    }
}
