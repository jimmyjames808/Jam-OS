/* snake: what the rules (game.c), the picture (draw.c), the game loop
 * (main.c) and the self-test (selftest.c) share. */
#pragma once
#include <fun.h>

#define FW 30                 /* the field, in cells */
#define FH 20
#define CELLS (FW * FH)
#define START_LEN 4

enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

struct game {
    int      w, h;            /* the field in cells (FW x FH; the self-test plays smaller ones) */
    /* The snake: a ring of cells (y * w + x). body[head] is the head, the
     * entries after it (wrapping at CELLS) follow towards the tail. */
    uint16_t body[CELLS];
    int      head, len;
    uint16_t left;            /* the cell the tail was in before the last step (its own cell
                                 if the snake grew): the picture slides the tail from it */
    uint8_t  used[CELLS];     /* 1: a cell of the snake */
    int      grow;            /* cells it still has to grow */
    int      dir;             /* DIR_*: where the head goes at the next step */
    int      turn[2], nturns; /* turns typed but not yet taken, in order */
    uint16_t food;            /* the cell with the apple */
    uint32_t score, eaten;    /* the counters shown */
    bool     over, won;       /* the game ended; ... because the field is full */
    bool     paused;
    uint64_t step_ns;         /* the time one step takes at this length */
    uint64_t step_at;         /* when the next step is due (ns) */
    uint64_t stepped_at;      /* when the last one was due: the slide starts there */
    uint64_t paused_at;       /* when p was pressed */
    uint64_t rng;             /* the random state (where the apples go) */
    /* For the animations only (the rules never look at these): */
    uint16_t ate_cell;        /* where the last apple was eaten */
    uint64_t ate_at, over_at; /* when (ns); 0: never */
};

/* ---- the rules (game.c) ---- */

/* A new game on a w x h field (at least 6 x 3): the snake START_LEN long
 * in the middle, heading right. */
void     game_new(struct game *g, int w, int h, uint64_t seed, uint64_t now);
/* One key press (a KEY_* code or a character): arrows or WASD turn (up to
 * two turns are remembered, a turn straight back is ignored), p pauses,
 * Enter or space starts again after the end. */
void     game_key(struct game *g, int key, uint64_t now);
/* The steps due up to `now`. */
void     game_tick(struct game *g, uint64_t now);
/* One step, now: turn, move, eat or die. */
void     game_step(struct game *g, uint64_t now);
/* The next time game_tick has something to do. */
uint64_t game_deadline(const struct game *g);
/* How long a step takes after `eaten` apples: shorter with each, down to a floor. */
uint64_t step_interval(uint32_t eaten);
/* The speed shown: 1 at the start, one more every five apples. */
static inline uint32_t game_level(const struct game *g) { return 1 + g->eaten / 5; }
/* Segment i's cell: 0 the head .. len - 1 the tail. */
static inline int game_cell(const struct game *g, int i)
{
    return g->body[(g->head + i) % CELLS];
}

/* ---- the picture (draw.c) ---- */

/* The layout for this screen and the background, drawn to the screen;
 * false if out of memory. */
bool draw_setup(void);
/* The frame at time t, into scr.s. */
void draw(const struct game *g, uint64_t t, uint32_t best);

/* selftest.c: `run snake --selftest`. */
int snake_selftest(void);
