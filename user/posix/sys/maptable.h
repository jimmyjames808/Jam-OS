/* libc's table of its own mappings (maptable.c), for mem.c: which VMO
 * backs each range mmap made, so munmap of part of one, madvise and a
 * MAP_FIXED over one find it. The table knows only what mmap and brk
 * mapped; the program's image, its stack and libjam's own mappings are
 * not in it, and mem.c never unmaps or decommits anything that isn't.
 *
 * Entries are sorted by address and never overlap. Several entries may
 * share a VMO (one mapping split by a munmap in its middle); the VMO's
 * handle is closed when the last of them goes.
 *
 * Every function here is called with mem.c's lock held. */
#pragma once

#include "layer.h"

struct mt_entry {
    uint64_t base;      /* page-aligned */
    uint64_t len;       /* bytes, page-aligned, > 0 */
    uint64_t vmo_off;   /* the VMO's offset at base */
    handle_t vmo;       /* the VMO mapped (shared with entries split from this one) */
    uint32_t flags;     /* MT_*; kept by the entries split from this one */
};

/* A private read-only mapping straight on a file's own pages (mem.c's
 * file part): an mprotect that adds write puts a copy-on-write child in
 * its place first, since the file's VMO is never writable. */
#define MT_PAGER_PRIVATE (1u << 0)

/* The table: entries[0 .. count). */
struct mt_entry *mt_entries(void);
unsigned         mt_count(void);
/* Room for n more entries (the table is made on first use). false: it is
 * full (VMAR_MAX_MAPPINGS, as many as the address space may hold) or
 * couldn't be made. */
bool             mt_room(unsigned n);
/* The index of the first entry that ends after addr (mt_count if none). */
unsigned         mt_first_after(uint64_t addr);
/* Insert e in its place (after mt_room said yes). */
void             mt_insert(const struct mt_entry *e);
/* Remove entry i; its VMO's handle is closed if no other entry has it. */
void             mt_remove(unsigned i);
