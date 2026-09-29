/* tetris: falling blocks with the keyboard, in the console's alternate screen.
 *
 *   run tetris              play
 *   run tetris --selftest   check the rules with scripted keys, exit 0 if right
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
 * self-test drives it with scripted keys and a fake clock. */
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
    int      cleared[4], ncleared;   /* the rows the last lock cleared (for the flash) */
    uint64_t cleared_at;
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
    /* Clear full rows. */
    g->ncleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BW; x++)
            full &= g->board[y][x] != 0;
        if (!full)
            continue;
        g->cleared[g->ncleared] = y - g->ncleared;   /* where it was before any row moved */
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

static struct term tv;
static int bs;      /* block height in text rows; a block is 2 * bs columns wide */
static int ox, oy;  /* the well's top-left cell on screen */

static const struct { uint8_t base, light; } colours[NPIECES] = {
    [I] = { C_CYAN, C_BCYAN },     [O] = { C_YELLOW, C_BYELLOW }, [T] = { C_MAGENTA, C_BMAGENTA },
    [S] = { C_GREEN, C_BGREEN },   [Z] = { C_RED, C_BRED },       [J] = { C_BLUE, C_BBLUE },
    [L] = { C_RED, C_BYELLOW },    /* orange: a shade of red over yellow */
};

/* A block at cell (x, y) (bs rows, 2 * bs columns): a light top edge, a dark
 * bottom edge. ghost: just an outline of shade. */
static void block(int x, int y, int type, bool ghost, bool flash)
{
    int w = 2 * bs;
    if (flash) {
        term_fill(&tv, x, y, w, bs, ' ', C_WHITE, C_WHITE);
        return;
    }
    uint8_t base = colours[type].base, light = colours[type].light;
    if (ghost) {
        term_fill(&tv, x, y, w, bs, G_LIGHT, light, C_BLACK);
        return;
    }
    if (type == L) {   /* dithered orange */
        term_fill(&tv, x, y, w, bs, G_MEDIUM, C_BRED, C_BYELLOW);
        if (bs > 1)
            term_fill(&tv, x, y, w, 1, G_UPPER, C_BYELLOW, C_BYELLOW);
        return;
    }
    term_fill(&tv, x, y, w, bs, ' ', base, base);
    if (bs == 1) {
        term_fill(&tv, x, y, w, 1, G_UPPER, light, base);
        return;
    }
    term_fill(&tv, x, y, w, 1, G_UPPER, light, base);
    term_fill(&tv, x, y + bs - 1, w, 1, G_LOWER, C_BLACK, base);
    term_put(&tv, x + w - 1, y, G_FULL, base, base);
}

/* A small piece picture (for next / hold) in a box at (x, y). */
static void preview(int x, int y, int type, const char *label, bool dim)
{
    int w = 4 * 2 * bs + 4, h = 2 * bs + 3;
    term_fill(&tv, x, y, w, h, ' ', C_GREY, C_BLACK);
    term_text(&tv, x + 1, y, label, C_GREY, C_BLACK);
    if (type < 0)
        return;
    int px = x + 2 + (type == I || type == O ? 0 : bs), py = y + 2 - (type == I ? bs : 0);
    for (int k = 0; k < 4; k++)
        block(px + shape[type][0][k][0] * 2 * bs, py + shape[type][0][k][1] * bs, type, dim,
              false);
}

static void draw(const struct game *g, uint64_t now, uint32_t best)
{
    term_fill(&tv, 0, 0, (int)tv.cols, (int)tv.rows, ' ', C_WHITE, C_BLACK);
    int ww = BW * 2 * bs, wh = (BH - HIDDEN) * bs;
    /* The well: a frame, then the stack. */
    term_fill(&tv, ox - 2, oy, 2, wh + 1, G_FULL, C_DARK, C_BLACK);
    term_fill(&tv, ox + ww, oy, 2, wh + 1, G_FULL, C_DARK, C_BLACK);
    term_fill(&tv, ox - 2, oy + wh, ww + 4, 1, G_UPPER, C_DARK, C_BLACK);
    bool flashing = g->ncleared && now - g->cleared_at < 180 * MS;
    for (int y = HIDDEN; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            int sx = ox + x * 2 * bs, sy = oy + (y - HIDDEN) * bs;
            if (g->board[y][x])
                block(sx, sy, g->board[y][x] - 1, false, false);
            else if (bs > 1)
                term_put(&tv, sx + bs, sy + bs / 2, '.', C_DARK, C_BLACK);
        }
    if (flashing)   /* the rows just cleared, white for a moment (the stack has moved already) */
        for (int i = 0; i < g->ncleared; i++)
            if (g->cleared[i] >= HIDDEN)
                term_fill(&tv, ox, oy + (g->cleared[i] - HIDDEN) * bs, ww, bs, ' ', C_WHITE, C_WHITE);
    if (!g->over) {
        int d = drop_distance(g);
        for (int k = 0; k < 4; k++) {   /* ghost, then the piece; only the visible rows */
            int cx = g->x + shape[g->type][g->rot][k][0], cy = g->y + shape[g->type][g->rot][k][1];
            if (cy + d >= HIDDEN && d)
                block(ox + cx * 2 * bs, oy + (cy + d - HIDDEN) * bs, g->type, true, false);
        }
        for (int k = 0; k < 4; k++) {
            int cx = g->x + shape[g->type][g->rot][k][0], cy = g->y + shape[g->type][g->rot][k][1];
            if (cy >= HIDDEN)
                block(ox + cx * 2 * bs, oy + (cy - HIDDEN) * bs, g->type, false, false);
        }
    }
    /* Left: hold and the score. Right: next. */
    int pw = 4 * 2 * bs + 4, lx = ox - 4 - pw, rx = ox + ww + 4;
    preview(lx, oy, g->hold, "HOLD  (c)", g->held);
    int ty = oy + 2 * bs + 5;
    char a[32];
    term_text(&tv, lx + 1, ty, "SCORE", C_GREY, C_BLACK);
    term_text(&tv, lx + 1, ty + 1, commas(a, sizeof(a), g->score), C_BYELLOW, C_BLACK);
    term_text(&tv, lx + 1, ty + 3, "LINES", C_GREY, C_BLACK);
    term_textf(&tv, lx + 1, ty + 4, C_WHITE, C_BLACK, "%u", g->lines);
    term_text(&tv, lx + 1, ty + 6, "LEVEL", C_GREY, C_BLACK);
    term_textf(&tv, lx + 1, ty + 7, C_BCYAN, C_BLACK, "%u", g->level);
    term_text(&tv, lx + 1, ty + 9, "BEST", C_GREY, C_BLACK);
    term_text(&tv, lx + 1, ty + 10, commas(a, sizeof(a), best), C_WHITE, C_BLACK);
    for (int i = 0; i < 3; i++)
        preview(rx, oy + i * (2 * bs + 3), g->next[i], i ? "" : "NEXT", false);
    int hy = oy + 3 * (2 * bs + 3) + 1;
    static const char *const help[] = {
        "left right  move", "up / x      rotate", "z           rotate back",
        "down        soft drop", "space       hard drop", "c           hold",
        "p           pause", "q / Esc     quit",
    };
    for (unsigned i = 0; i < sizeof(help) / sizeof(help[0]); i++)
        term_text(&tv, rx + 1, hy + (int)i, help[i], C_GREY, C_BLACK);
    term_text(&tv, ox, oy - 2, "J A M   O S   T E T R I S", C_BYELLOW, C_BLACK);
    if (g->paused || g->over) {
        const char *m1 = g->over ? "  GAME OVER  " : "   PAUSED   ";
        const char *m2 = g->over ? " Enter: again " : "  p: resume  ";
        int mx = ox + ww / 2 - 7, my = oy + wh / 2 - 1;
        term_fill(&tv, mx - 1, my - 1, 16, 4, ' ', C_WHITE, C_RED);
        term_text(&tv, mx, my, m1, C_WHITE, C_RED);
        term_text(&tv, mx, my + 1, m2, C_BYELLOW, C_RED);
    }
}

static int play(void)
{
    status_t st = term_open(&tv);
    if (st != OK) {
        say("tetris: no console screen (%s)\n", status_str(st));
        return 1;
    }
    /* Blocks as big as fit: 20 rows of bs rows, the panels beside the well. */
    bs = ((int)tv.rows - 4) / (BH - HIDDEN);
    while (bs > 1 && 2 * (BW * 2 * bs) + 60 > (int)tv.cols)
        bs--;
    if (bs < 1)
        bs = 1;
    ox = ((int)tv.cols - BW * 2 * bs) / 2;
    oy = ((int)tv.rows - (BH - HIDDEN) * bs) / 2 + 1;

    struct game g;
    uint64_t seed = now_ns() ^ 0x7e7715;
    game_new(&g, seed, now_ns());
    uint32_t best = 0, games = 0;
    bool quit = false;
    while (!quit) {
        uint64_t now = now_ns();
        game_tick(&g, now);
        if (g.score > best)
            best = g.score;
        draw(&g, now, best);
        term_flush(&tv);
        uint64_t deadline = game_deadline(&g);
        if (g.ncleared && now - g.cleared_at < 200 * MS && g.cleared_at + 200 * MS < deadline)
            deadline = g.cleared_at + 200 * MS;   /* end the flash */
        int k = term_key(&tv, deadline);
        if (k == KEY_QUIT || k == 'q' || k == 'Q')
            quit = true;
        else if (k != KEY_NONE) {
            bool was_over = g.over;
            game_key(&g, k, now_ns());
            games += was_over && !g.over;
        }
    }
    term_close(&tv);
    say("tetris: score %u, %u lines, level %u (best %u over %u game(s))\n", g.score, g.lines,
        g.level, best, games + 1);
    return 0;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return selftest();
    if (has_arg(argc, argv, "--crash-test")) {
        /* For tools/shell-tests/fun.txt: take the screen and die without
         * giving it back, as a crashing program would. The console must
         * notice (our key channel closes) and show the text screen again. */
        if (term_open(&tv) != OK)
            return 1;
        term_fill(&tv, 0, 0, (int)tv.cols, (int)tv.rows, G_MEDIUM, C_RED, C_BLACK);
        term_text(&tv, 2, 2, "tetris --crash-test: exiting without leaving the alternate screen",
                  C_WHITE, C_RED);
        term_flush(&tv);
        jam_nanosleep(now_ns() + 500000000ull);
        return 3;
    }
    return play();
}
