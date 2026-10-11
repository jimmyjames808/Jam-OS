/* libc's table of its own mappings (maptable.h): a sorted array of
 * entries, in a VMO of its own mapped on first use (malloc can't hold it:
 * musl's malloc is built on these very mappings). Its size is the kernel's
 * cap on an address space's mappings, so the table is never the limit:
 * VMAR_MAX_MAPPINGS entries of 32 bytes, 512 KiB of address space whose
 * pages are committed as the table grows. Lookups are binary searches;
 * an insert or a removal moves the entries after it. */
#include "maptable.h"

#define MT_MAX VMAR_MAX_MAPPINGS

_Static_assert(sizeof(struct mt_entry) == 32, "no padding in a table entry");

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
