/* kexec's memory maps: a range overlaid with a type, neighbours merged,
 * and the types' numbers on the wire (<jam/kexec_handoff.h>).
 *
 * The reservation overlays the region onto the loader's map (as
 * FOREIGN); an image's map is this kernel's, with each type turned into
 * what the next kernel may do with it and then the image's own pieces
 * overlaid (image.c). Pure functions on arrays: tested in
 * kernel/test/test_kexec.c. */
#include <jam/string.h>
#include "kexec_internal.h"

/* How many entries [base, end) splits map[i] into (1..3). */
static unsigned pieces(const struct boot_mem_region *r, uint64_t base, uint64_t end)
{
    uint64_t rend = r->base + r->length;
    if (end <= r->base || base >= rend)
        return 1;
    return 1 + (base > r->base) + (end < rend);
}

status_t kx_memmap_overlay(struct boot_mem_region *map, size_t *n, size_t cap, uint64_t base,
                           uint64_t len, enum boot_mem_type type)
{
    uint64_t end = base + len;
    if (end < base)
        return ERR_INVALID_ARGS;
    size_t need = 0;
    for (size_t i = 0; i < *n; i++)
        need += pieces(&map[i], base, end);
    if (need > cap)
        return ERR_NO_RESOURCES;
    /* From the last entry down, so each split moves only entries already
     * placed at their final index. */
    size_t out = need;
    for (size_t i = *n; i-- > 0;) {
        struct boot_mem_region r = map[i];
        uint64_t rend = r.base + r.length;
        if (end <= r.base || base >= rend) {
            map[--out] = r;
            continue;
        }
        uint64_t lo = base > r.base ? base : r.base, hi = end < rend ? end : rend;
        if (hi < rend)
            map[--out] = (struct boot_mem_region){ hi, rend - hi, r.type };
        map[--out] = (struct boot_mem_region){ lo, hi - lo, type };
        if (lo > r.base)
            map[--out] = (struct boot_mem_region){ r.base, lo - r.base, r.type };
    }
    *n = need;
    return OK;
}

void kx_memmap_merge(struct boot_mem_region *map, size_t *n)
{
    size_t out = 0;
    for (size_t i = 0; i < *n; i++) {
        if (!map[i].length)
            continue;
        struct boot_mem_region *last = out ? &map[out - 1] : NULL;
        if (last && last->type == map[i].type && last->base + last->length == map[i].base)
            last->length += map[i].length;
        else
            map[out++] = map[i];
    }
    *n = out;
}

/* One table both ways: the wire numbers are fixed forever, the enum is
 * this kernel's own. */
static const struct {
    enum boot_mem_type type;
    uint32_t           wire;
} types[] = {
    { BOOT_MEM_USABLE, KEXEC_MEM_USABLE },
    { BOOT_MEM_RESERVED, KEXEC_MEM_RESERVED },
    { BOOT_MEM_ACPI_RECLAIMABLE, KEXEC_MEM_ACPI_RECLAIMABLE },
    { BOOT_MEM_ACPI_NVS, KEXEC_MEM_ACPI_NVS },
    { BOOT_MEM_BAD, KEXEC_MEM_BAD },
    { BOOT_MEM_LOADER_RECLAIMABLE, KEXEC_MEM_LOADER_RECLAIMABLE },
    { BOOT_MEM_KERNEL_AND_MODULES, KEXEC_MEM_KERNEL_AND_MODULES },
    { BOOT_MEM_FRAMEBUFFER, KEXEC_MEM_FRAMEBUFFER },
    { BOOT_MEM_FOREIGN, KEXEC_MEM_FOREIGN },
    { BOOT_MEM_CRASH_LOG, KEXEC_MEM_CRASH_LOG },
};

uint32_t kexec_mem_to_wire(enum boot_mem_type t)
{
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (types[i].type == t)
            return types[i].wire;
    return KEXEC_MEM_RESERVED;   /* not reached: every type is in the table */
}

bool kexec_mem_from_wire(uint32_t w, enum boot_mem_type *out)
{
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (types[i].wire == w) {
            *out = types[i].type;
            return true;
        }
    return false;
}
