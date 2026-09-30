/* mines: what the board's rules (board.c), the picture (draw.c), the game
 * loop (main.c) and the self-test (selftest.c) share. */
#pragma once
#include <fun.h>

#define MAX_BW 40
#define MAX_BH 22

/* ---- the board (board.c) ---- */

enum { CELL_MINE = 1, CELL_OPEN = 2, CELL_FLAG = 4 };
enum state { PLAYING, WON, LOST };

struct board {
    int        w, h, mines;          /* cells across and down; mines in them */
    uint8_t    cell[MAX_BH][MAX_BW]; /* CELL_* bits */
    uint8_t    near[MAX_BH][MAX_BW]; /* mines in the (up to) 8 cells around */
    bool       laid;                 /* the mines are placed (the first reveal does it) */
    int        opened, flags;        /* cells open; flags set */
    enum state state;
    int        boom_x, boom_y;       /* LOST: the mine that was opened */
    uint64_t   rng;                  /* the random state the mines come from */
    uint64_t   started, ended;       /* uptime ns of the first reveal and of the end; 0: not yet */
};

/* A difficulty level. */
struct level {
    const char *name;   /* on its button */
    int         w, h;   /* the board */
    int         mines;
};
#define NLEVELS 4
extern const struct level levels[NLEVELS];

void board_new(struct board *b, const struct level *lv, uint64_t seed);
/* Open (x, y) and, from a cell with no mines around, everything that
 * connects to it. The first reveal of a game lays the mines, never on
 * (x, y) and, while there is room, not next to it either. Nothing happens
 * to a flagged or open cell, or after the game ended. Returns the cells
 * opened (a mine: 0, and the game is LOST). */
int  board_reveal(struct board *b, int x, int y, uint64_t now);
/* What a right click did. */
enum right {
    RIGHT_NOTHING,   /* an open cell without a number, off the board, or the game is over */
    RIGHT_FLAG,      /* a closed cell: its flag was set or cleared */
    RIGHT_CHORD,     /* a number with as many flags around it: the rest around it opened */
    RIGHT_PEEK,      /* a number with fewer or more flags: nothing changed (board_peeks) */
};
/* A right click on (x, y). On a closed cell it toggles the flag. On an
 * open number whose flags around equal it, it opens every other closed
 * cell around it as board_reveal would (the chord: with a wrong flag one
 * of them is a mine, and the game is LOST). On an open number with any
 * other count of flags nothing changes: the answer is RIGHT_PEEK, and
 * while the button is held the picture dims the cells board_peeks names. */
enum right board_right(struct board *b, int x, int y, uint64_t now);
/* (nx, ny) is one of the cells a peek at (x, y) shows: (x, y) is an open
 * number whose flags around don't equal it, and (nx, ny) is a closed cell
 * next to it without a flag (one the number still covers). */
bool board_peeks(const struct board *b, int x, int y, int nx, int ny);
/* Mines minus flags (negative with too many flags). */
int  board_mines_left(const struct board *b);
/* Whole seconds played, 0 before the first reveal, at most 999. */
uint32_t board_seconds(const struct board *b, uint64_t now);
/* Every cell's number from the mines as they lie, and the board counts as
 * laid: the last step of laying the mines (the self-test lays boards of
 * its own and calls it). */
void board_numbers(struct board *b);
/* Flags around (x, y). */
int  board_flags_near(const struct board *b, int x, int y);


/* ---- the picture (draw.c) ---- */

enum hit { HIT_NONE, HIT_CELL, HIT_NEW, HIT_LEVEL };

/* What the frame shows besides the board. */
struct view {
    int  level;         /* index into levels */
    int  hover_x, hover_y;   /* the cell under the pointer; -1: none */
    bool pressing;      /* the left button is down on it */
    int  peek_x, peek_y;     /* the number a held right button peeks at; -1: none */
    bool no_mouse;      /* no mouse has stirred: say the game needs one */
    uint32_t wins, games;    /* this session */
    uint32_t best[NLEVELS];  /* the quickest win per level, seconds; 0: none */
};

/* The layout for a board of level lv on this screen, and the background;
 * false if out of memory. Called again when the level changes. */
bool draw_setup(const struct level *lv);
/* What is at screen pixel (x, y): a cell (*cx, *cy), the new-game button,
 * or a level button (*cx its index). */
enum hit draw_hit(int x, int y, int *cx, int *cy);
/* The frame at time t, into scr.s. */
void draw(const struct board *b, const struct view *v, uint64_t t);

/* selftest.c: `run mines --selftest`. */
int mines_selftest(void);
