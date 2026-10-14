/* posixtest's mem section's reservation helpers (memres.c), shared by
 * memres.c and mempat.c (V8's and PartitionAlloc's patterns). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PG     4096ul
#define KIB    1024ul
#define MIB    (1ul << 20)
#define GIB    (1ul << 30)
#define SLACK  4ul   /* pages: a page of mapping records, a page table made or freed */
#define TABLES 12ul  /* pages: the tables a first touch in a new GiB may make (VMO, address space) */

/* Handle units charged to our job now: a handle each, and one for every
 * VMO object. */
uint64_t mr_job_handles(void);
/* CLOCK_MONOTONIC in ns. */
uint64_t mr_now_ns(void);
/* len bytes of address space, no access, nothing committed: V8's and
 * PartitionAlloc's reservation (MAP_FAILED if refused). */
char    *mr_reserve(uint64_t len);
/* MAP_FIXED private anonymous memory at p, with prot: true if placed. */
bool     mr_fixed_anon(char *p, uint64_t len, int prot);
/* A byte v written in every page of [p, p + len): each page committed. */
void     mr_touch(char *p, uint64_t len, char v);
/* Does every page of [p, p + len) read zero at its start? (Reading
 * commits the pages.) */
bool     mr_zero(const char *p, uint64_t len);
/* Our address space's mappings now (process_info.mappings; 0 if it
 * can't be read). */
uint32_t mr_mappings(void);

/* V8's and PartitionAlloc's patterns (mempat.c). */
void     pt_mem_patterns(void);
