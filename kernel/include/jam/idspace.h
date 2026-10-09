/* Small ids handed out in turn: process ids and thread ids (POSIX's pid
 * and tid, which programs keep in 32-bit or 30-bit fields, so a koid
 * won't do).
 *
 * An id space holds the values 1 .. max - 1. Each value is unique among
 * the ids taken and not yet given back. Values are handed out in rising
 * order from a cursor that wraps at max, and a value given back is not
 * handed out again before the cursor has gone once round the whole space
 * since: one given back ahead of the cursor (in the lap that will still
 * reach it) is held until the cursor wraps. So a program that kept an old
 * pid sees it name nothing for as long as possible rather than a newcomer.
 *
 * Lock: each space's own spinlock ("pid space", "tid space"), taken with
 * interrupts off and nothing nested under it. Nothing sleeps or
 * allocates: the bitmaps are the space's own memory. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/spinlock.h>
#include <jam/status.h>

struct idspace {
    spinlock_t lock;        /* guards everything below */
    uint64_t  *used;        /* bit v: v is taken, or given back and held for the lap */
    uint64_t  *held;        /* bit v: given back ahead of the cursor, free at the wrap */
    uint32_t   max;         /* values are 1 .. max - 1; a multiple of 64 */
    uint32_t   next;        /* the cursor: where the next search starts */
    uint32_t   live;        /* values taken and not given back */
    uint32_t   nheld;       /* bits set in held */
    uint64_t   laps;        /* times the cursor wrapped (tests) */
};

/* The space's bitmaps: `words` 64-bit words each, max = 64 * words
 * values (the value 0 is never handed out). For tests: the
 * kernel's own two spaces are below. */
void idspace_init(struct idspace *s, const char *name, uint64_t *used, uint64_t *held,
                  uint32_t words);
/* Take the next free value after the cursor (wrapping): OK and *out, or
 * ERR_NO_RESOURCES when every value is taken or held. */
status_t idspace_alloc(struct idspace *s, uint32_t *out);
/* Give v back (it must be taken; a value never taken, or 0, panics: a
 * kernel bug). */
void idspace_free(struct idspace *s, uint32_t v);
/* Values taken now (tests, the shell's counts). */
uint32_t idspace_live(struct idspace *s);

/* The kernel's two spaces: process ids (pids) and user thread ids
 * (tids), each 1 .. IDS_MAX - 1 (<jam/abi.h>). 2^22 is Linux's own
 * ceiling for pids; a tid also fits the 30 bits musl's mutexes keep it
 * in. */
extern struct idspace pid_space;
extern struct idspace tid_space;
