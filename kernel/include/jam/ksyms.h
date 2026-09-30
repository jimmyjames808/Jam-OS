/* Kernel symbol table for backtraces. tools/gensyms.py builds it from the
 * linked kernel and a second link embeds it: a table sorted by address, and
 * the NUL-separated names it points into. */
#pragma once

#include <stdint.h>

struct ksym {
    uint64_t addr;          /* function start */
    uint32_t name_offset;   /* its name, in ksyms_names */
};

extern const uint64_t    ksyms_count;
extern const struct ksym ksyms_table[];
extern const char        ksyms_names[];

/* Name of the function containing addr, or NULL; *offset gets addr - start. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);
