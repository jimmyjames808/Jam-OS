/* life: the engine (engine.c) that the game (main.c) and the self-test
 * (selftest.c) share: the world, one bit a cell, and the generation step. */
#pragma once
#include <fun.h>

#define BAND 8   /* rows per work item */

extern uint32_t W, WW;        /* world size (cells) and words per row */
extern uint64_t *cur, *old;   /* this generation and the one before */
extern uint64_t *age[3];      /* the age counter's bits (see main.c) */
extern bool ages;             /* keep the ages (else only cur and old) */
extern uint64_t gen;          /* generations since the world was made */
extern uint64_t soup_seed;    /* soup()'s next seed */

/* A world of size x size cells (a power of two >= 64); false: no memory. */
bool     world_alloc(uint32_t size);
/* Everything dead, and long dead (age 7). */
void     world_clear(void);
/* A random soup, 37.5% alive, from soup_seed (then a new seed). */
void     soup(void);
/* One generation on the pool (parallel) or on this thread alone.
 * Returns the population. */
uint64_t step(bool parallel);
uint64_t population(const uint64_t *g);

static inline bool get(const uint64_t *g, uint32_t x, uint32_t y)
{
    x &= W - 1;
    y &= W - 1;
    return g[(uint64_t)y * WW + x / 64] >> (x % 64) & 1;
}

static inline void set(uint64_t *g, uint32_t x, uint32_t y, bool on)
{
    x &= W - 1;
    y &= W - 1;
    uint64_t *p = &g[(uint64_t)y * WW + x / 64], m = 1ull << (x % 64);
    *p = on ? *p | m : *p & ~m;
}

static inline uint32_t age_of(uint32_t x, uint32_t y)
{
    return (uint32_t)get(age[0], x, y) | (uint32_t)get(age[1], x, y) << 1 |
           (uint32_t)get(age[2], x, y) << 2;
}

/* A pattern in rows of '.' and 'O', its top-left corner at (x, y): newborn
 * cells, in a cleared space (margin cells all round) so it gets a start. */
void stamp(const char *const *rows, uint32_t nrows, uint32_t x, uint32_t y, uint32_t margin);
extern const char *const glider[3], *const rpent[3], *const gun[9];

/* selftest.c: `run life --selftest`. */
int life_selftest(void);
