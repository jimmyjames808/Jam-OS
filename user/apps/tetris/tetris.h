/* tetris: what the rules (game.c), the picture (draw.c), the game loop
 * (main.c) and the self-test (selftest.c) share. */
#pragma once
#include "../fun/fun.h"

#define BW 10
#define BH 22        /* rows 0 and 1 are hidden above the well */
#define HIDDEN 2
#define LOCK_NS 500000000ull
#define LOCK_MOVES 15

enum { I, O, T, S, Z, J, L, NPIECES };

/* SRS cells (x, y down) in the piece's box, per rotation. */
extern const int8_t shape[NPIECES][4][4][2];

struct game {
    uint8_t  board[BH][BW];   /* 0 empty, else piece + 1 */
    int      type, rot, x, y; /* the falling piece: its box's top-left */
    int      hold;            /* -1: none */
    bool     held;            /* hold used for this piece */
    uint8_t  bag[7];
    int      nbag;
    int      next[3];
    uint64_t rng;
    uint32_t score, lines, level, pieces;
    bool     over, paused;
    uint64_t fall_at;         /* the next gravity step (ns) */
    uint64_t lock_at;         /* 0: not on the ground */
    int      lock_moves;
    /* For the animations only (the rules never look at these): */
    int      cleared[4], ncleared;   /* the rows the last lock cleared, where they were */
    uint8_t  cleared_cells[4][BW];   /* what they held */
    uint64_t cleared_at;
    int      locked_type, locked_rot, locked_x, locked_y;   /* the last piece locked */
    uint64_t locked_at;
    int      drop_type, drop_rot, drop_x, drop_y0, drop_y1; /* the last hard drop */
    uint64_t dropped_at;
};

/* ---- the rules (game.c) ---- */

void     game_new(struct game *g, uint64_t seed, uint64_t now);
/* One key press (a KEY_* code or a character). */
void     game_key(struct game *g, int key, uint64_t now);
/* Gravity and the lock delay up to `now`. */
void     game_tick(struct game *g, uint64_t now);
/* The next time game_tick has something to do. */
uint64_t game_deadline(const struct game *g);
/* The piece `type` at the top, as if it came next (game over if it can't). */
void     appear(struct game *g, int type, uint64_t now);
void     spawn_next(struct game *g, uint64_t now);
/* The next piece from the bag of seven. */
int      bag_take(struct game *g);
bool     fits(const struct game *g, int type, int rot, int x, int y);
/* Rows the piece falls on a hard drop. */
int      drop_distance(const struct game *g);
uint32_t board_cells(const struct game *g);

/* ---- the picture (draw.c) ---- */

/* The layout for this screen, the sprites and the background, drawn to
 * the screen; false if out of memory. */
bool draw_setup(void);
/* Particles for what the game just did (a line clear, a hard drop). */
void effects(const struct game *g);
void parts_step(float dt);
/* The frame at time t, into scr.s. */
void draw(const struct game *g, uint64_t t, uint32_t best);
/* Something is moving: frames at the frame rate until it stops. */
bool animating(const struct game *g, uint64_t t);

/* selftest.c: `run tetris --selftest`. */
int tetris_selftest(void);
