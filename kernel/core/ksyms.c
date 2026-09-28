#include <stddef.h>
#include <jam/ksyms.h>

extern char __text_start[], __text_end[];

const char *ksym_lookup(uint64_t addr, uint64_t *offset)
{
    if (addr < (uint64_t)__text_start || addr >= (uint64_t)__text_end || !ksyms_count)
        return NULL;
    uint64_t lo = 0, hi = ksyms_count;   /* last entry with addr <= target */
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if (ksyms_table[mid].addr <= addr)
            lo = mid;
        else
            hi = mid;
    }
    if (ksyms_table[lo].addr > addr)
        return NULL;
    *offset = addr - ksyms_table[lo].addr;
    return &ksyms_names[ksyms_table[lo].name_offset];
}
