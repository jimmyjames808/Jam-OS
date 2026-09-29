/* tetris: falling blocks with the keyboard, on the screen borrowed from the console.
 *
 *   run tetris [hz=60]        play (hz: the frame rate while something moves)
 *   run tetris --selftest     check the rules with scripted keys, exit 0 if right
 *   run tetris --crash-test   (tests) die of a page fault holding the screen
 *   run tetris --hang-test    (tests) hold the screen until killed
 *
 * The rules are the modern ones: a 10 x 20 well (plus 2 hidden rows on
 * top), the seven pieces from a shuffled "bag" of seven, SRS rotation with
 * its wall kicks, a ghost showing where the piece will land, hold, three
 * pieces of preview, a 0.5 s lock delay (up to 15 moves on the ground),
 * gravity speeding up every 10 lines, and 100 / 300 / 500 / 800 x level for
 * 1 / 2 / 3 / 4 lines (+1 a row soft drop, +2 a row hard drop).
 *
 * Keys: left / right move (hold them: the keyboard repeats), up or x
 * rotates clockwise, z counter-clockwise, down drops softly, space drops
 * hard, c holds, p pauses, Enter starts again after game over, q or Esc
 * quits. The game logic (struct game) never touches the screen, so the
 * self-test drives it with scripted keys and a fake clock.
 *
 * The picture (full resolution, sized from the screen's height): bevelled
 * blocks (sprites made at start), a ghost outline, hold and three next
 * pieces, the score panel. The background is drawn once and the moving
 * parts over a copy of it each frame; frames come at `hz` while anything
 * animates (line clears flash white and the rows above slide down, a hard
 * drop leaves a fading trail and dust, locked pieces glow, particles fly),
 * else only when the game or a key changes something, and gfx_present
 * sends only the pixels that changed. */
#include "../fun/fun.h"

#define BW 10
#define BH 22        /* rows 0 and 1 are hidden above the well */
#define HIDDEN 2
#define LOCK_NS 500000000ull
#define LOCK_MOVES 15
#define MS 1000000ull

enum { I, O, T, S, Z, J, L, NPIECES };

/* SRS cells (x, y down) in the piece's box, per rotation. */
static const int8_t shape[NPIECES][4][4][2] = {
    [I] = { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }, { { 2, 0 }, { 2, 1 }, { 2, 2 }, { 2, 3 } },
            { { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 } }, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 } } },
    [O] = { { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } },
            { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } } },
    [T] = { { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 1, 2 } }, { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, 2 } } },
    [S] = { { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } }, { { 1, 0 }, { 1, 1 }, { 2, 1 }, { 2, 2 } },
            { { 1, 1 }, { 2, 1 }, { 0, 2 }, { 1, 2 } }, { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 2 } } },
    [Z] = { { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } }, { { 2, 0 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 1, 2 }, { 2, 2 } }, { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 0, 2 } } },
    [J] = { { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 2, 2 } }, { { 1, 0 }, { 1, 1 }, { 0, 2 }, { 1, 2 } } },
    [L] = { { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 2, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 2 } }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 1, 2 } } },
};

/* SRS wall kicks, [from rotation][0 clockwise, 1 counter-clockwise][try],
 * in the guideline's (x right, y UP) form. */
static const int8_t kick_jlstz[4][2][5][2] = {
    { { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },     /* 0 -> 1 */
      { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } } },      /* 0 -> 3 */
    { { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },         /* 1 -> 2 */
      { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } } },       /* 1 -> 0 */
    { { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },        /* 2 -> 3 */
      { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } } },   /* 2 -> 1 */
    { { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },      /* 3 -> 0 */
      { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } } },    /* 3 -> 2 */
};
static const int8_t kick_i[4][2][5][2] = {
    { { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },       /* 0 -> 1 */
      { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } } },     /* 0 -> 3 */
    { { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },       /* 1 -> 2 */
      { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } } },     /* 1 -> 0 */
    { { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },       /* 2 -> 3 */
      { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } } },     /* 2 -> 1 */
    { { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },       /* 3 -> 0 */
      { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } } },     /* 3 -> 2 */
};

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

static bool fits(const struct game *g, int type, int rot, int x, int y)
{
    for (int k = 0; k < 4; k++) {
        int cx = x + shape[type][rot][k][0], cy = y + shape[type][rot][k][1];
        if (cx < 0 || cx >= BW || cy < 0 || cy >= BH || g->board[cy][cx])
            return false;
    }
    return true;
}

static int bag_take(struct game *g)
{
    if (g->nbag == 0) {   /* a new bag: the seven pieces shuffled */
        for (int i = 0; i < 7; i++)
            g->bag[i] = (uint8_t)i;
        for (int i = 6; i > 0; i--) {
            int j = (int)(rng_next(&g->rng) % (uint64_t)(i + 1));
            uint8_t t = g->bag[i];
            g->bag[i] = g->bag[j];
            g->bag[j] = t;
        }
        g->nbag = 7;
    }
    return g->bag[7 - g->nbag--];
}

/* ns per row at the game's level (the guideline's (0.8 - (level-1) * 0.007)^(level-1) s). */
static uint64_t gravity_ns(const struct game *g)
{
    double base = 0.8 - (g->level - 1) * 0.007, t = 1;
    for (uint32_t i = 1; i < g->level; i++)
        t *= base;
    uint64_t ns = (uint64_t)(t * 1e9);
    return ns < 1000000 ? 1000000 : ns;
}

static void appear(struct game *g, int type, uint64_t now)
{
    g->type = type;
    g->rot = 0;
    g->x = 3;
    g->y = 0;
    g->lock_at = 0;
    g->lock_moves = 0;
    g->fall_at = now + gravity_ns(g);
    if (!fits(g, type, 0, g->x, g->y)) {
        g->over = true;
        return;
    }
    if (fits(g, type, 0, g->x, g->y + 1))   /* into view at once */
        g->y++;
}

static void spawn_next(struct game *g, uint64_t now)
{
    int t = g->next[0];
    g->next[0] = g->next[1];
    g->next[1] = g->next[2];
    g->next[2] = bag_take(g);
    g->held = false;
    appear(g, t, now);
}

static void game_new(struct game *g, uint64_t seed, uint64_t now)
{
    memset(g, 0, sizeof(*g));
    g->rng = seed | 1;
    g->hold = -1;
    g->level = 1;
    for (int i = 0; i < 3; i++)
        g->next[i] = bag_take(g);
    spawn_next(g, now);
}

static bool on_ground(const struct game *g)
{
    return !fits(g, g->type, g->rot, g->x, g->y + 1);
}

/* After a move or rotation: restart the lock delay (a limited number of times). */
static void moved(struct game *g, uint64_t now)
{
    if (!on_ground(g)) {
        g->lock_at = 0;
    } else if (g->lock_moves < LOCK_MOVES) {
        g->lock_at = now + LOCK_NS;
        g->lock_moves++;
    } else if (!g->lock_at) {
        g->lock_at = now + 1;   /* out of moves: locks at the next tick */
    }
}

static void lock_piece(struct game *g, uint64_t now)
{
    for (int k = 0; k < 4; k++)
        g->board[g->y + shape[g->type][g->rot][k][1]][g->x + shape[g->type][g->rot][k][0]] =
            (uint8_t)(g->type + 1);
    g->pieces++;
    g->locked_type = g->type;
    g->locked_rot = g->rot;
    g->locked_x = g->x;
    g->locked_y = g->y;
    g->locked_at = now;
    /* Clear full rows. */
    g->ncleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BW; x++)
            full &= g->board[y][x] != 0;
        if (!full)
            continue;
        g->cleared[g->ncleared] = y - g->ncleared;   /* where it was before any row moved */
        memcpy(g->cleared_cells[g->ncleared], g->board[y], BW);
        g->ncleared++;
        memmove(g->board[1], g->board[0], (size_t)y * BW);
        memset(g->board[0], 0, BW);
        y++;   /* look at this row again: the one above moved into it */
    }
    static const uint32_t points[5] = { 0, 100, 300, 500, 800 };
    g->score += points[g->ncleared] * g->level;
    g->lines += (uint32_t)g->ncleared;
    g->level = 1 + g->lines / 10;
    if (g->ncleared)
        g->cleared_at = now;
    spawn_next(g, now);
}

static bool try_move(struct game *g, int dx, int dy, uint64_t now)
{
    if (!fits(g, g->type, g->rot, g->x + dx, g->y + dy))
        return false;
    g->x += dx;
    g->y += dy;
    moved(g, now);
    return true;
}

static bool rotate(struct game *g, int dir, uint64_t now)   /* dir 0: clockwise, 1: counter */
{
    if (g->type == O)
        return true;
    int to = (g->rot + (dir ? 3 : 1)) & 3;
    const int8_t (*k)[2] = g->type == I ? kick_i[g->rot][dir] : kick_jlstz[g->rot][dir];
    for (int i = 0; i < 5; i++) {
        int nx = g->x + k[i][0], ny = g->y - k[i][1];   /* the table's y points up */
        if (fits(g, g->type, to, nx, ny)) {
            g->x = nx;
            g->y = ny;
            g->rot = to;
            moved(g, now);
            return true;
        }
    }
    return false;
}

static int drop_distance(const struct game *g)
{
    int d = 0;
    while (fits(g, g->type, g->rot, g->x, g->y + d + 1))
        d++;
    return d;
}

/* One key press. */
static void game_key(struct game *g, int key, uint64_t now)
{
    if (g->over) {
        if (key == KEY_ENTER)
            game_new(g, rng_next(&g->rng), now);
        return;
    }
    if (key == 'p' || key == 'P') {
        g->paused = !g->paused;
        if (!g->paused)
            g->fall_at = now + gravity_ns(g);
        return;
    }
    if (g->paused)
        return;
    switch (key) {
    case KEY_LEFT:
        try_move(g, -1, 0, now);
        break;
    case KEY_RIGHT:
        try_move(g, 1, 0, now);
        break;
    case KEY_DOWN:
        if (try_move(g, 0, 1, now)) {
            g->score += 1;
            g->fall_at = now + gravity_ns(g);
        }
        break;
    case KEY_UP: case 'x': case 'X':
        rotate(g, 0, now);
        break;
    case 'z': case 'Z':
        rotate(g, 1, now);
        break;
    case ' ': {
        int d = drop_distance(g);
        g->drop_type = g->type;
        g->drop_rot = g->rot;
        g->drop_x = g->x;
        g->drop_y0 = g->y;
        g->drop_y1 = g->y + d;
        g->dropped_at = now ? now : 1;
        g->y += d;
        g->score += 2 * (uint32_t)d;
        lock_piece(g, now);
        break;
    }
    case 'c': case 'C':
        if (!g->held) {
            int t = g->type;
            if (g->hold < 0)
                spawn_next(g, now);
            else
                appear(g, g->hold, now);
            g->hold = t;
            g->held = true;
        }
        break;
    }
}

/* Gravity and the lock delay up to `now`. */
static void game_tick(struct game *g, uint64_t now)
{
    if (g->over || g->paused)
        return;
    while (now >= g->fall_at) {
        if (on_ground(g)) {
            g->fall_at = now + gravity_ns(g);
            break;
        }
        g->y++;
        g->fall_at += gravity_ns(g);
    }
    if (!on_ground(g))
        g->lock_at = 0;
    else if (!g->lock_at)
        g->lock_at = now + LOCK_NS;   /* just landed */
    if (g->lock_at && now >= g->lock_at)
        lock_piece(g, now);
}

/* The next time game_tick has something to do. */
static uint64_t game_deadline(const struct game *g)
{
    if (g->over || g->paused)
        return DEADLINE_NEVER;
    uint64_t d = g->fall_at;
    if (g->lock_at && g->lock_at < d)
        d = g->lock_at;
    return d;
}

static uint32_t board_cells(const struct game *g)
{
    uint32_t n = 0;
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            n += g->board[y][x] != 0;
    return n;
}

/* ---- the self-test ------------------------------------------------------------------ */

static int failures;

static void check(bool ok, const char *what)
{
    say("tetris: selftest: %-60s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static void force(struct game *g, int type)
{
    appear(g, type, 0);
}

static int selftest(void)
{
    struct game g;
    char what[96];

    /* The bag: every 7 pieces are the 7 different pieces. */
    game_new(&g, 42, 0);
    g.nbag = 0;   /* start at a bag boundary (the game took 4 already) */
    bool bag_ok = true;
    for (int b = 0; b < 20; b++) {
        int seen = 0;
        for (int i = 0; i < 7; i++)
            seen |= 1 << bag_take(&g);
        bag_ok &= seen == 0x7f;
    }
    check(bag_ok, "7-bag: 20 bags, each a permutation of the 7 pieces");

    /* An I dropped hard on an empty well lands on the floor. */
    game_new(&g, 1, 0);
    force(&g, I);
    game_key(&g, ' ', 0);
    bool ok = board_cells(&g) == 4 && g.pieces == 1;
    for (int x = 3; x < 7; x++)
        ok &= g.board[BH - 1][x] == I + 1;
    check(ok, "hard drop: I lands flat on the floor, columns 3-6");
    snprintf(what, sizeof(what), "hard drop scores 2 a row (score %u)", g.score);
    check(g.score == 2 * (BH - 1 - 2), what);

    /* Walls: ten lefts stop at the wall, ten rights at the other. */
    game_new(&g, 1, 0);
    force(&g, T);
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_LEFT, 0);
    ok = g.x == 0;
    for (int i = 0; i < 20; i++)
        game_key(&g, KEY_RIGHT, 0);
    check(ok && g.x == BW - 3, "moves stop at both walls");

    /* Rotation: four clockwise turns come back; an I stood up against the
     * right wall kicks left to lie down. */
    game_new(&g, 1, 0);
    force(&g, T);
    int x0 = g.x, y0 = g.y;
    for (int i = 0; i < 4; i++)
        game_key(&g, KEY_UP, 0);
    check(g.rot == 0 && g.x == x0 && g.y == y0, "rotation: 4 x clockwise = where it started");
    force(&g, I);
    game_key(&g, KEY_UP, 0);   /* vertical, in box column 2 */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_RIGHT, 0);
    ok = g.rot == 1 && g.x + 2 == BW - 1;
    game_key(&g, KEY_UP, 0);   /* 1 -> 2 needs a kick left */
    ok &= g.rot == 2 && fits(&g, I, 2, g.x, g.y) && g.x + 3 == BW - 1;
    check(ok, "SRS wall kick: I at the right wall rotates by kicking left");
    force(&g, T);
    game_key(&g, KEY_UP, 0);   /* 0 -> 1: the box's left column is empty */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_LEFT, 0);
    ok = g.rot == 1 && g.x == -1;
    game_key(&g, KEY_UP, 0);   /* 1 -> 2 at x -1 is off the well: kicks right */
    check(ok && g.rot == 2 && g.x == 0, "SRS wall kick: T at the left wall kicks right");

    /* A tetris: four rows full but for column 9, an I dropped in it. */
    game_new(&g, 1, 0);
    for (int y = BH - 4; y < BH; y++)
        for (int x = 0; x < BW - 1; x++)
            g.board[y][x] = (uint8_t)(1 + (x + y) % 7);
    force(&g, I);
    game_key(&g, KEY_UP, 0);   /* vertical in box column 2 */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_RIGHT, 0);
    game_key(&g, ' ', 0);
    snprintf(what, sizeof(what), "tetris: 4 lines, 800 + drop points, empty well (score %u)", g.score);
    check(g.lines == 4 && g.ncleared == 4 && board_cells(&g) == 0 && g.score >= 800 &&
              g.score < 900,
          what);

    /* Lines above a clear fall down; a single scores 100 x level. */
    game_new(&g, 1, 0);
    for (int x = 0; x < BW - 1; x++)
        g.board[BH - 1][x] = 1;
    g.board[BH - 2][0] = 3;   /* a block above the row that clears */
    g.level = 3;
    force(&g, I);
    game_key(&g, KEY_UP, 0);
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_RIGHT, 0);
    uint32_t before = g.score;
    game_key(&g, ' ', 0);
    /* The I stands in column 9 on the floor: the bottom row clears, 3 of its cells stay. */
    ok = g.lines == 1 && g.board[BH - 1][0] == 3 && g.board[BH - 1][BW - 1] == I + 1 &&
         board_cells(&g) == 4 && g.score - before >= 300;
    check(ok, "single at level 3: 300, the rows above move down");

    /* Gravity: level 1 moves one row a second; the lock delay locks it. */
    game_new(&g, 7, 0);
    force(&g, O);
    int y1 = g.y;
    game_tick(&g, 999 * MS);
    ok = g.y == y1;
    game_tick(&g, 1000 * MS);
    ok &= g.y == y1 + 1;
    uint64_t t = 1000 * MS;
    uint32_t p0 = g.pieces;
    while (g.pieces == p0 && t < 60000 * MS)
        game_tick(&g, t += 10 * MS);
    check(ok && g.pieces == p0 + 1, "gravity: 1 row/s at level 1, the piece locks at the floor");
    snprintf(what, sizeof(what), "  ... 19 rows down, it locked at %lu ms (19.5 s)",
             (unsigned long)(t / MS));
    check(t >= 19490 * MS && t <= 19520 * MS, what);

    /* Hold: swaps in the held piece, once per piece. */
    game_new(&g, 3, 0);
    int first = g.type, second = g.next[0];
    game_key(&g, 'c', 0);
    ok = g.hold == first && g.type == second;
    game_key(&g, 'c', 0);   /* not again for this piece */
    ok &= g.hold == first && g.type == second;
    check(ok, "hold: once per piece");

    /* Game over when a new piece can't appear. */
    game_new(&g, 5, 0);
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            g.board[y][x] = (y + x) % 2 ? 1 : 0;
    spawn_next(&g, 0);
    check(g.over, "game over when the spawn position is blocked");
    game_key(&g, KEY_ENTER, 0);
    check(!g.over && board_cells(&g) == 0, "Enter starts a new game");

    /* A long scripted game: random keys and time; the bookkeeping holds. */
    game_new(&g, 2024, 0);
    uint64_t r = 99;
    static const int keys[] = { KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP, 'z', ' ', 'c',
                                KEY_LEFT, KEY_LEFT, KEY_RIGHT, KEY_RIGHT, ' ' };
    uint32_t games = 0, total_pieces = 0, total_lines = 0;
    t = 0;
    ok = true;
    for (int i = 0; i < 20000; i++) {
        t += (rng_next(&r) % 300) * MS;
        game_tick(&g, t);
        game_key(&g, keys[rng_next(&r) % (sizeof(keys) / sizeof(keys[0]))], t);
        if (!g.over)
            ok &= fits(&g, g.type, g.rot, g.x, g.y);
        ok &= board_cells(&g) == 4 * g.pieces - 10 * g.lines;
        if (g.over) {
            games++;
            total_pieces += g.pieces;
            total_lines += g.lines;
            game_key(&g, KEY_ENTER, t);
        }
    }
    snprintf(what, sizeof(what), "20000 random keys: %u games, %u pieces, %u lines, consistent",
             games, total_pieces + g.pieces, total_lines + g.lines);
    check(ok && games > 0, what);

    say("tetris: selftest %s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

/* ---- drawing -------------------------------------------------------------------------- */

#define KEY_PX    0x010203       /* transparent pixels in the block sprites */
#define CLEAR_NS  260000000ull   /* the line-clear animation */
#define FLASH_NS  150000000ull   /* ... its flash; then the rows above slide down */
#define LOCK_FLASH_NS 140000000ull
#define TRAIL_NS  220000000ull

static int B;                    /* block size in pixels */
static int wx, wy;               /* the well's top-left on screen */
static int lx, rx, pw;           /* the side panels: left x, right x, width */
static int ls, ns, hs;           /* text scales: labels, numbers, headings */
static int hold_y, hold_h, stat_y, stat_h, next_y, next_h, help_y;
static struct surf bg;           /* everything that doesn't move, drawn once */
enum { SZ_WELL, SZ_BIG, SZ_SMALL, NSIZES };
static struct surf sprite[NSIZES][NPIECES];
static int sprite_px[NSIZES];

static const uint32_t piece_rgb[NPIECES] = {
    [I] = 0x2ec8f0, [O] = 0xf5cc1a, [T] = 0xb052d8, [S] = 0x4cd04a,
    [Z] = 0xf0443c, [J] = 0x4070ec, [L] = 0xf58a28,
};

/* A bevelled block b x b (its last row and column left transparent: the
 * gap between blocks). */
static struct surf make_block(int b, uint32_t c)
{
    struct surf s = surf_new(b, b);
    if (!s.px)
        return s;
    int n = b - 1, e = b >= 30 ? b / 8 : b >= 14 ? 3 : 2;
    for (int y = 0; y < b; y++)
        for (int x = 0; x < b; x++) {
            uint32_t *p = &s.px[y * b + x];
            if (x == n || y == n) {
                *p = KEY_PX;
                continue;
            }
            /* The face: a gentle top-to-bottom gradient. */
            uint32_t col = mixc(mixc(c, 0xffffff, 36), scalec(c, 200), (uint32_t)(y * 256 / n));
            int dt = y, dl = x, db = n - 1 - y, dr = n - 1 - x;
            int m = dt < dl ? dt : dl;
            m = db < m ? db : m;
            m = dr < m ? dr : m;
            if (m < e) {   /* the bevel: lit from the top left */
                if (m == dt && dt <= dr)
                    col = mixc(c, 0xffffff, 120);
                else if (m == dl && dl <= db)
                    col = mixc(c, 0xffffff, 64);
                else if (m == db)
                    col = scalec(c, 120);
                else
                    col = scalec(c, 160);
            } else if (y < e + (n - 2 * e) / 3 && x < n - e - 1) {
                /* a soft shine across the top of the face */
                uint32_t k = (uint32_t)(y - e) * 256 / (uint32_t)((n - 2 * e) / 3 + 1);
                col = mixc(col, 0xffffff, 40 - k * 40 / 256);
            }
            *p = col;
        }
    return s;
}

static inline double ease_out(double t)   /* 0..1 */
{
    if (t <= 0)
        return 0;
    if (t >= 1)
        return 1;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ---- particles (line clears, hard drops) ---- */

#define MAX_PARTS 900
static struct part {
    float x, y, vx, vy, life, max;
    uint32_t c;
    int size;
} parts[MAX_PARTS];
static int nparts;
static uint64_t rng_fx = 0x9a17c1e5;

static float frand(void) { return (float)(rng_next(&rng_fx) >> 40) / (float)(1 << 24); }

static void fx_spawn(float x, float y, float vx, float vy, float life, uint32_t c, int size)
{
    if (nparts == MAX_PARTS)
        return;
    parts[nparts++] = (struct part){ x, y, vx, vy, life, life, c, size };
}

static void parts_step(float dt)
{
    if (dt > 0.1f)
        dt = 0.1f;
    int j = 0;
    for (int i = 0; i < nparts; i++) {
        struct part *p = &parts[i];
        p->life -= dt;
        if (p->life <= 0)
            continue;
        p->vy += (float)B * 30.0f * dt;   /* gravity */
        p->vx *= 1.0f - 1.5f * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        parts[j++] = *p;
    }
    nparts = j;
}

static void parts_draw(const struct surf *s)
{
    for (int i = 0; i < nparts; i++) {
        const struct part *p = &parts[i];
        uint32_t a = (uint32_t)(256 * p->life / p->max);
        int sz = p->size * (int)(64 + a * 3 / 4) / 256 + 1;
        blend(s, (int)p->x - sz / 2, (int)p->y - sz / 2, sz, sz, p->c, a);
    }
}

/* The effects the game's last moves call for (once per new event). */
static uint64_t seen_clear, seen_drop;

static void effects(const struct game *g)
{
    if (g->ncleared && g->cleared_at != seen_clear) {
        seen_clear = g->cleared_at;
        for (int i = 0; i < g->ncleared; i++) {
            int row = g->cleared[i] - HIDDEN;
            for (int x = 0; x < BW; x++) {
                uint8_t cell = g->cleared_cells[i][x];
                uint32_t c = cell ? piece_rgb[cell - 1] : 0xffffff;
                for (int k = 0; k < 3 + g->ncleared; k++) {
                    float a = frand() * 6.2832f, sp = (float)B * (4 + 10 * frand());
                    fx_spawn((x + frand()) * B, (row + frand()) * B, sp * (float)cosd(a),
                          sp * (float)sind(a) - (float)B * 8, 0.5f + 0.5f * frand(),
                          mixc(c, 0xffffff, (uint32_t)(frand() * 160)), B / 5 + 2);
                }
            }
        }
    }
    if (g->dropped_at && g->dropped_at != seen_drop && g->drop_y1 > g->drop_y0) {
        seen_drop = g->dropped_at;
        for (int k = 0; k < 4; k++) {
            int cx = g->drop_x + shape[g->drop_type][g->drop_rot][k][0];
            int cy = g->drop_y1 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            for (int j = 0; j < 3; j++)
                fx_spawn((cx + frand()) * B, (cy + 1) * B - 2, (frand() - 0.5f) * B * 8,
                      -frand() * B * 5, 0.25f + 0.2f * frand(),
                      mixc(piece_rgb[g->drop_type], 0xffffff, 140), B / 8 + 2);
        }
    }
}

/* ---- layout and the background ---- */

static const char *const help[][2] = {
    { "left right", "move" }, { "up  x", "rotate" }, { "z", "rotate back" },
    { "down", "soft drop" }, { "space", "hard drop" }, { "c", "hold" }, { "p", "pause" },
    { "q  Esc", "quit" },
};

static int help_kw(void)   /* the keys' column width */
{
    int kw = 0;
    for (unsigned i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        int w = text_width(ls, help[i][0]);
        kw = w > kw ? w : kw;
    }
    return kw;
}

static void layout(void)
{
    int W = scr.w, H = scr.h;
    B = (H * 86 / 100) / 20;
    if (B * 23 > W)
        B = W / 23;
    wx = (W - BW * B) / 2;
    wy = (H - 20 * B) / 2;
    pw = B * 5;
    lx = wx - B * 3 / 4 - pw;
    rx = wx + BW * B + B * 3 / 4;
    ls = B >= 44 ? 2 : 1;
    ns = B >= 44 ? 4 : 2;
    hs = B >= 44 ? 5 : 3;
    int vw = 0;   /* the side panels: as wide as the help needs */
    for (unsigned i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        int w = text_width(ls, help[i][1]);
        vw = w > vw ? w : vw;
    }
    if (2 * (B / 3) + help_kw() + 3 * ls * 4 + vw + B / 4 > pw) {
        pw = 2 * (B / 3) + help_kw() + 3 * ls * 4 + vw + B / 4;
        lx = wx - B * 3 / 4 - pw;
    }
    hold_y = wy + TEXT_H(hs) + B / 2;
    hold_h = 3 * B;
    stat_y = hold_y + hold_h + B / 2;
    /* four numbers and the level bar */
    stat_h = B / 3 + 4 * (TEXT_H(ls) + ls * 3 + TEXT_H(ns) + B / 3) + B / 4 + B / 3;
    next_y = wy;
    next_h = TEXT_H(ls) + B / 4 + 2 * B + B / 2 + 2 * (2 * B) + B / 4;
    help_y = next_y + next_h + B / 2;
    sprite_px[SZ_WELL] = B;
    sprite_px[SZ_BIG] = B * 4 / 5;
    sprite_px[SZ_SMALL] = B * 3 / 5;
    for (int z = 0; z < NSIZES; z++)
        for (int t = 0; t < NPIECES; t++)
            sprite[z][t] = make_block(sprite_px[z], piece_rgb[t]);
}

static void box(const struct surf *s, int x, int y, int w, int h, const char *label)
{
    int r = B / 4;
    panel(s, x - 2, y - 2, w + 4, h + 4, r + 2, 0x3b4c86, 170);
    panel(s, x, y, w, h, r, 0x0c1022, 240);
    if (label)
        text(s, x + B / 3, y + B / 5, ls, 0x8fa3d8, label);
}

static void draw_background(void)
{
    struct surf *s = &bg;
    vgrad(s, 0, 0, s->w, s->h, 0x182044, 0x05060c);
    /* faint stars */
    uint64_t r = 12345;
    for (int i = 0; i < s->w * s->h / 3000; i++) {
        int x = (int)(rng_next(&r) % (uint64_t)s->w), y = (int)(rng_next(&r) % (uint64_t)s->h);
        uint32_t a = 30 + (uint32_t)(rng_next(&r) % 90);
        int d = a > 100 && scr.ui > 1 ? 2 : 1;
        blend(s, x, y, d, d, 0xc8d4ff, a);
    }
    /* the well: a glowing rim, a dark floor with a faint grid */
    for (int g = 5; g >= 1; g--)
        panel(s, wx - 3 * g - 2, wy - 3 * g - 2, BW * B + 6 * g + 4, 20 * B + 6 * g + 4,
              B / 4 + 3 * g, 0x4a70ff, 22);
    fill(s, wx - 2, wy - 2, BW * B + 4, 20 * B + 4, 0x5a6cb0);
    vgrad(s, wx, wy, BW * B, 20 * B, 0x0b0f20, 0x070914);
    for (int x = 1; x < BW; x++)
        fill(s, wx + x * B - 1, wy, 1, 20 * B, 0x151b35);
    for (int y = 1; y < 20; y++)
        fill(s, wx, wy + y * B - 1, BW * B, 1, 0x151b35);
    /* the title: one letter per piece colour */
    static const char title[] = "TETRIS";
    static const int tc[] = { Z, L, O, S, I, T };
    int tx = lx;
    for (int i = 0; title[i]; i++) {
        char ch[2] = { title[i], 0 };
        tx = text_shadow(s, tx, wy, hs, piece_rgb[tc[i]], ch) + hs;
    }
    text(s, lx + 2, wy - TEXT_H(ls) - 4 * ls, ls, 0x6b7bb0, "JAM OS");
    box(s, lx, hold_y, pw, hold_h, "HOLD");
    box(s, lx, stat_y, pw, stat_h, NULL);
    box(s, rx, next_y, pw, next_h, "NEXT");
    int n = (int)(sizeof(help) / sizeof(help[0])), step = TEXT_H(ls) + ls * 5, kw = help_kw();
    int hh = 2 * (B / 3) + n * step - ls * 5;
    if (help_y + hh <= wy + 20 * B) {
        box(s, rx, help_y, pw, hh, NULL);
        int y = help_y + B / 3;
        for (int i = 0; i < n; i++, y += step) {
            text(s, rx + B / 3, y, ls, 0xe0e6ff, help[i][0]);
            text(s, rx + B / 3 + kw + 3 * ls * 4, y, ls, 0x8290c0, help[i][1]);
        }
    }
}

/* ---- the frame ---- */

static void restore(int x, int y, int w, int h)
{
    blit(&scr.s, x, y, &bg, x, y, w, h);
}

/* A piece picture centred in a box (x, y, w, h). */
static void piece_at(int x, int y, int w, int h, int type, int size, bool dim)
{
    int b = sprite_px[size];
    int minx = 4, maxx = 0, miny = 4, maxy = 0;
    for (int k = 0; k < 4; k++) {
        int cx = shape[type][0][k][0], cy = shape[type][0][k][1];
        minx = cx < minx ? cx : minx;
        maxx = cx > maxx ? cx : maxx;
        miny = cy < miny ? cy : miny;
        maxy = cy > maxy ? cy : maxy;
    }
    int px = x + (w - (maxx - minx + 1) * b) / 2, py = y + (h - (maxy - miny + 1) * b) / 2;
    for (int k = 0; k < 4; k++) {
        int cx = shape[type][0][k][0] - minx, cy = shape[type][0][k][1] - miny;
        blit_key(&scr.s, px + cx * b, py + cy * b, &sprite[size][type], KEY_PX);
        if (dim)
            blend(&scr.s, px + cx * b, py + cy * b, b - 1, b - 1, 0x101018, 170);
    }
}

static void stat(int *y, const char *label, const char *value, uint32_t c)
{
    text(&scr.s, lx + B / 3, *y, ls, 0x8fa3d8, label);
    *y += TEXT_H(ls) + ls * 3;
    text_shadow(&scr.s, lx + B / 3, *y, ns, c, value);
    *y += TEXT_H(ns) + B / 3;
}

static void draw(const struct game *g, uint64_t now, uint32_t best)
{
    struct surf *s = &scr.s;
    struct surf well = { s->px + (uint64_t)wy * s->stride + wx, BW * B, 20 * B, s->stride };
    int in_y = TEXT_H(ls) + B / 4;
    restore(wx, wy, BW * B, 20 * B);
    restore(lx, hold_y + in_y, pw, hold_h - in_y - B / 8);
    restore(lx, stat_y, pw, stat_h);
    restore(rx, next_y + in_y, pw, next_h - in_y - B / 8);

    /* The stack. During a line clear the rows above the cleared ones are
     * drawn where they were, then slide down into place. */
    uint64_t since = g->ncleared ? now - g->cleared_at : ~0ull;
    bool clearing = since < CLEAR_NS;
    int orig[BH];   /* each row's row before the clear */
    for (int r = 0; r < BH; r++)
        orig[r] = r;
    if (clearing) {
        int r = BH - 1;
        for (int o = BH - 1; o >= 0 && r >= 0; o--) {
            bool gone = false;
            for (int i = 0; i < g->ncleared; i++)
                gone |= g->cleared[i] == o;
            if (!gone)
                orig[r--] = o;
        }
        for (; r >= 0; r--)
            orig[r] = r - g->ncleared;
    }
    double slide = clearing ? ease_out(((double)since - FLASH_NS / 2) /
                                       (double)(CLEAR_NS - FLASH_NS / 2)) : 1;
    for (int r = 0; r < BH; r++) {
        int y = (int)((orig[r] + (r - orig[r]) * slide - HIDDEN) * B + 0.5);
        if (y <= -B)
            continue;
        for (int x = 0; x < BW; x++)
            if (g->board[r][x])
                blit_key(&well, x * B, y, &sprite[SZ_WELL][g->board[r][x] - 1], KEY_PX);
    }
    if (clearing && since < FLASH_NS * 3 / 2) {   /* the cleared rows: white-hot, then shrinking */
        double t = (double)since / (double)(FLASH_NS * 3 / 2);
        for (int i = 0; i < g->ncleared; i++) {
            int y = (g->cleared[i] - HIDDEN) * B;
            if (y < 0)
                continue;
            int w = (int)(BW * B * (1 - ease_out((t - 0.35) / 0.65)));
            int x0 = (BW * B - w) / 2;
            for (int x = 0; x < BW; x++)
                if (g->cleared_cells[i][x])
                    blit_key(&well, x * B, y, &sprite[SZ_WELL][g->cleared_cells[i][x] - 1], KEY_PX);
            blend(&well, 0, y, BW * B, B - 1, 0xffffff, (uint32_t)(256 * (t < 0.35 ? t / 0.35 : 1)));
            restore(wx, wy + y, x0, B - 1);
            restore(wx + x0 + w, wy + y, BW * B - x0 - w, B - 1);
        }
    }
    /* The last piece locked glows for a moment. */
    if (g->pieces && now - g->locked_at < LOCK_FLASH_NS && !clearing) {
        uint32_t a = (uint32_t)(140 * (LOCK_FLASH_NS - (now - g->locked_at)) / LOCK_FLASH_NS);
        for (int k = 0; k < 4; k++) {
            int cx = g->locked_x + shape[g->locked_type][g->locked_rot][k][0];
            int cy = g->locked_y + shape[g->locked_type][g->locked_rot][k][1] - HIDDEN;
            blend(&well, cx * B, cy * B, B - 1, B - 1, 0xffffff, a);
        }
    }
    /* The hard drop's trail: streaks from where the piece was to where it landed. */
    if (g->dropped_at && now - g->dropped_at < TRAIL_NS) {
        double t = (double)(now - g->dropped_at) / TRAIL_NS;
        uint32_t c = piece_rgb[g->drop_type];
        for (int k = 0; k < 4; k++) {
            int cx = g->drop_x + shape[g->drop_type][g->drop_rot][k][0];
            int top = g->drop_y0 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            int bot = g->drop_y1 + shape[g->drop_type][g->drop_rot][k][1] - HIDDEN;
            int y0 = top * B + (int)((bot - top) * B * ease_out(t * 1.3)), y1 = bot * B;
            for (int y = y0; y < y1; y++) {
                uint32_t a = (uint32_t)(130 * (1 - t) * (y - y0 + 1) / (y1 - y0 + 1));
                blend(&well, cx * B + B / 5, y, B - 1 - 2 * (B / 5), 1, c, a);
            }
        }
    }
    if (!g->over) {
        int d = drop_distance(g);
        uint32_t c = piece_rgb[g->type];
        int t = B >= 30 ? 3 : 2;
        for (int k = 0; k < 4 && d; k++) {   /* the ghost: where it will land */
            int cx = g->x + shape[g->type][g->rot][k][0];
            int cy = g->y + shape[g->type][g->rot][k][1] + d - HIDDEN;
            if (cy < 0)
                continue;
            blend(&well, cx * B, cy * B, B - 1, B - 1, c, 50);
            struct surf cell = { well.px + (int64_t)cy * B * well.stride + cx * B, B - 1, B - 1,
                                 well.stride };
            frame(&cell, 0, 0, B - 1, B - 1, t, mixc(c, 0xffffff, 40));
        }
        for (int k = 0; k < 4; k++) {
            int cx = g->x + shape[g->type][g->rot][k][0];
            int cy = g->y + shape[g->type][g->rot][k][1] - HIDDEN;
            blit_key(&well, cx * B, cy * B, &sprite[SZ_WELL][g->type], KEY_PX);
        }
    }
    parts_draw(&well);

    /* Hold and next. */
    int iy = hold_y + in_y;
    if (g->hold >= 0)
        piece_at(lx, iy, pw, hold_h - in_y - B / 8, g->hold, SZ_BIG, g->held);
    iy = next_y + in_y;
    piece_at(rx, iy, pw, 2 * B + B / 2, g->next[0], SZ_BIG, false);
    iy += 2 * B + B / 2;
    for (int i = 1; i < 3; i++, iy += 2 * B)
        piece_at(rx, iy, pw, 2 * B, g->next[i], SZ_SMALL, false);

    /* The numbers. */
    char a[48], n[24];
    int y = stat_y + B / 3;
    stat(&y, "SCORE", commas(a, sizeof(a), g->score), 0xffe07a);
    snprintf(a, sizeof(a), "%u", g->level);
    stat(&y, "LEVEL", a, 0x7fe0ff);
    snprintf(a, sizeof(a), "%u", g->lines);
    stat(&y, "LINES", a, 0xffffff);
    stat(&y, "BEST", commas(a, sizeof(a), best), 0xb8c4f0);
    /* the way to the next level */
    int bw = pw - 2 * (B / 3), bh = B / 4 > 4 ? B / 4 : 4;
    if (y + bh < stat_y + stat_h - B / 4) {
        panel(s, lx + B / 3, y, bw, bh, bh / 2, 0x242c50, 256);
        int fw = bw * (int)(g->lines % 10) / 10;
        if (fw >= bh)
            panel(s, lx + B / 3, y, fw, bh, bh / 2, 0x4ac8ff, 256);
    }

    if (g->paused || g->over) {
        blend(&well, 0, 0, BW * B, 20 * B, 0x04050c, 190);
        const char *m1 = g->over ? "GAME OVER" : "PAUSED";
        int ts = hs, cy = 20 * B / 2 - TEXT_H(ts);
        while (ts > 1 && text_width(ts, m1) > BW * B - B)
            ts--;
        text_shadow(&well, (BW * B - text_width(ts, m1)) / 2, cy, ts,
                    g->over ? 0xff6a5a : 0xffffff, m1);
        cy += TEXT_H(ts) + B / 2;
        if (g->over) {
            snprintf(a, sizeof(a), "score %s", commas(n, sizeof(n), g->score));
            text_shadow(&well, (BW * B - text_width(ls + 1, a)) / 2, cy, ls + 1, 0xffe07a, a);
            cy += TEXT_H(ls + 1) + B / 3;
        }
        const char *m2 = g->over ? "Enter: play again" : "p: carry on";
        text_shadow(&well, (BW * B - text_width(ls, m2)) / 2, cy, ls, 0xc8d0f0, m2);
    }
}

static bool animating(const struct game *g, uint64_t now)
{
    return nparts || (g->ncleared && now - g->cleared_at < CLEAR_NS) ||
           (g->pieces && now - g->locked_at < LOCK_FLASH_NS) ||
           (g->dropped_at && now - g->dropped_at < TRAIL_NS);
}

static int play(int argc, char **argv)
{
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    uint32_t hz = (uint32_t)arg_num(argc, argv, "hz", 60);
    if (hz < 20 || hz > 240)
        hz = 60;
    status_t st = gfx_open();
    if (st != OK) {
        say("tetris: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    layout();
    bg = surf_new(scr.w, scr.h);
    if (!bg.px) {
        gfx_close();
        say("tetris: out of memory\n");
        return 1;
    }
    draw_background();
    blit(&scr.s, 0, 0, &bg, 0, 0, scr.w, scr.h);

    struct game g;
    game_new(&g, now() ^ 0x7e7715, now());
    uint32_t best = 0, games = 0, frames = 0;
    uint64_t period = 1000000000ull / hz, last = now(), t_start = last;
    bool quit = false;
    while (!quit) {
        uint64_t t = now();
        game_tick(&g, t);
        effects(&g);
        if (g.score > best)
            best = g.score;
        parts_step((float)(t - last) / 1e9f);
        last = t;
        draw(&g, t, best);
        gfx_present();
        frames++;
        /* The next frame: at the frame rate while something moves, else
         * when the game has something to do (or a key comes). */
        uint64_t deadline = animating(&g, t) ? t + period : game_deadline(&g);
        int k = gfx_key(deadline);
        while (k != KEY_NONE) {
            if (k == KEY_QUIT || k == 'q' || k == 'Q') {
                quit = true;
                break;
            }
            bool was_over = g.over;
            game_key(&g, k, now());
            games += was_over && !g.over;
            k = gfx_key(0);   /* everything typed meanwhile, then draw */
        }
    }
    uint64_t ms = (now() - t_start) / 1000000ull;
    gfx_close();
    say("tetris: score %u, %u lines, level %u (best %u over %u game(s)); %u frames in %lu ms, "
        "%lu MB to the screen\n",
        g.score, g.lines, g.level, best, games + 1, frames, (unsigned long)ms,
        (unsigned long)(scr.bytes >> 20));
    return 0;
}

/* For tools/shell-tests/fun.txt: borrow the screen, draw on it and die
 * without giving it back, as a crashing program would. The console must
 * notice (the lease closes with the process) and redraw its text screen. */
static int crash_test(void)
{
    pool_start(0);
    if (gfx_open() != OK)
        return 1;
    vgrad(&scr.s, 0, 0, scr.w, scr.h, 0x700010, 0x100008);
    text_shadow(&scr.s, 40, 40, scr.ui * 2, 0xffffff,
                "tetris --crash-test: dying with the screen borrowed");
    gfx_present();
    jam_nanosleep(now() + 500000000ull);
    say("tetris: crash test: faulting now\n");
    volatile int *p;
    __asm__("" : "=r"(p) : "0"((uintptr_t)8));   /* hide the null from the compiler */
    return *p;   /* a page fault: the kernel kills us */
}

/* For tools/shell-tests/fun.txt: borrow the screen and hang, reading no
 * keys: only Ctrl+C (the shell kills the job) gets rid of it. */
static int hang_test(void)
{
    pool_start(0);
    if (gfx_open() != OK)
        return 1;
    vgrad(&scr.s, 0, 0, scr.w, scr.h, 0x001060, 0x000818);
    text_shadow(&scr.s, 40, 40, scr.ui * 2, 0xffffff,
                "tetris --hang-test: holding the screen until killed (Ctrl+C)");
    gfx_present();
    say("tetris: hang test: holding the screen\n");
    for (;;)
        jam_nanosleep(now() + 1000000000ull);
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    if (has_arg(argc, argv, "--crash-test"))
        return crash_test();
    if (has_arg(argc, argv, "--hang-test"))
        return hang_test();
    return play(argc, argv);
}
