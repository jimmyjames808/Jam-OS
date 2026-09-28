#pragma once

#include <stdint.h>

struct ksym {
    uint64_t addr;
    uint32_t name_offset;
};

extern const uint64_t    ksyms_count;
extern const struct ksym ksyms_table[];
extern const char        ksyms_names[];

/* Name of the function containing addr, or NULL; *offset gets addr - start. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);
