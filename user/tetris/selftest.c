/* tetris: the self-test (`run tetris --selftest`): the rules with
 * scripted keys and a fake clock. */
#include "tetris.h"

static void force(struct game *g, int type)
{
    appear(g, type, 0);
}

/* The bag: every 7 pieces are the 7 different pieces. */
static void test_bag(void)
{
    struct game g;
    game_new(&g, 42, 0);
    g.nbag = 0;   /* start at a bag boundary (the game took 4 already) */
    bool bag_ok = true;
    for (int b = 0; b < 20; b++) {
        int seen = 0;
        for (int i = 0; i < 7; i++)
            seen |= 1 << bag_take(&g);
        bag_ok &= seen == 0x7f;
    }
    fun_check(bag_ok, "7-bag: 20 bags, each a permutation of the 7 pieces");
}

/* An I dropped hard on an empty well lands on the floor. */
static void test_hard_drop(void)
{
    struct game g;
    char what[96];
    game_new(&g, 1, 0);
    force(&g, I);
    game_key(&g, ' ', 0);
    bool ok = board_cells(&g) == 4 && g.pieces == 1;
    for (int x = 3; x < 7; x++)
        ok &= g.board[BH - 1][x] == I + 1;
    fun_check(ok, "hard drop: I lands flat on the floor, columns 3-6");
    snprintf(what, sizeof(what), "hard drop scores 2 a row (score %u)", g.score);
    fun_check(g.score == 2 * (BH - 1 - 2), what);
}

/* Walls: ten lefts stop at the wall, ten rights at the other. */
static void test_walls(void)
{
    struct game g;
    game_new(&g, 1, 0);
    force(&g, T);
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_LEFT, 0);
    bool ok = g.x == 0;
    for (int i = 0; i < 20; i++)
        game_key(&g, KEY_RIGHT, 0);
    fun_check(ok && g.x == BW - 3, "moves stop at both walls");
}

/* Rotation: four clockwise turns come back; an I stood up against the
 * right wall kicks left to lie down. */
static void test_rotation(void)
{
    struct game g;
    game_new(&g, 1, 0);
    force(&g, T);
    int x0 = g.x, y0 = g.y;
    for (int i = 0; i < 4; i++)
        game_key(&g, KEY_UP, 0);
    fun_check(g.rot == 0 && g.x == x0 && g.y == y0,
              "rotation: 4 x clockwise = where it started");
    force(&g, I);
    game_key(&g, KEY_UP, 0);   /* vertical, in box column 2 */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_RIGHT, 0);
    bool ok = g.rot == 1 && g.x + 2 == BW - 1;
    game_key(&g, KEY_UP, 0);   /* 1 -> 2 needs a kick left */
    ok &= g.rot == 2 && fits(&g, I, 2, g.x, g.y) && g.x + 3 == BW - 1;
    fun_check(ok, "SRS wall kick: I at the right wall rotates by kicking left");
    force(&g, T);
    game_key(&g, KEY_UP, 0);   /* 0 -> 1: the box's left column is empty */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_LEFT, 0);
    ok = g.rot == 1 && g.x == -1;
    game_key(&g, KEY_UP, 0);   /* 1 -> 2 at x -1 is off the well: kicks right */
    fun_check(ok && g.rot == 2 && g.x == 0, "SRS wall kick: T at the left wall kicks right");
}

/* A tetris: four rows full but for column 9, an I dropped in it. */
static void test_tetris(void)
{
    struct game g;
    char what[96];
    game_new(&g, 1, 0);
    for (int y = BH - 4; y < BH; y++)
        for (int x = 0; x < BW - 1; x++)
            g.board[y][x] = (uint8_t)(1 + (x + y) % 7);
    force(&g, I);
    game_key(&g, KEY_UP, 0);   /* vertical in box column 2 */
    for (int i = 0; i < 10; i++)
        game_key(&g, KEY_RIGHT, 0);
    game_key(&g, ' ', 0);
    snprintf(what, sizeof(what), "tetris: 4 lines, 800 + drop points, empty well (score %u)",
             g.score);
    fun_check(g.lines == 4 && g.ncleared == 4 && board_cells(&g) == 0 && g.score >= 800 &&
                  g.score < 900,
              what);
}

/* Lines above a clear fall down; a single scores 100 x level. */
static void test_single(void)
{
    struct game g;
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
    bool ok = g.lines == 1 && g.board[BH - 1][0] == 3 && g.board[BH - 1][BW - 1] == I + 1 &&
              board_cells(&g) == 4 && g.score - before >= 300;
    fun_check(ok, "single at level 3: 300, the rows above move down");
}

/* Gravity: level 1 moves one row a second; the lock delay locks it. */
static void test_gravity(void)
{
    struct game g;
    char what[96];
    game_new(&g, 7, 0);
    force(&g, O);
    int y1 = g.y;
    game_tick(&g, 999 * NS_PER_MS);
    bool ok = g.y == y1;
    game_tick(&g, 1000 * NS_PER_MS);
    ok &= g.y == y1 + 1;
    uint64_t t = 1000 * NS_PER_MS;
    uint32_t p0 = g.pieces;
    while (g.pieces == p0 && t < 60000 * NS_PER_MS)
        game_tick(&g, t += 10 * NS_PER_MS);
    fun_check(ok && g.pieces == p0 + 1,
              "gravity: 1 row/s at level 1, the piece locks at the floor");
    snprintf(what, sizeof(what), "  ... 19 rows down, it locked at %lu ms (19.5 s)",
             (unsigned long)(t / NS_PER_MS));
    fun_check(t >= 19490 * NS_PER_MS && t <= 19520 * NS_PER_MS, what);
}

/* Hold: swaps in the held piece, once per piece. */
static void test_hold(void)
{
    struct game g;
    game_new(&g, 3, 0);
    int first = g.type, second = g.next[0];
    game_key(&g, 'c', 0);
    bool ok = g.hold == first && g.type == second;
    game_key(&g, 'c', 0);   /* not again for this piece */
    ok &= g.hold == first && g.type == second;
    fun_check(ok, "hold: once per piece");
}

/* Game over when a new piece can't appear. */
static void test_game_over(void)
{
    struct game g;
    game_new(&g, 5, 0);
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            g.board[y][x] = (y + x) % 2 ? 1 : 0;
    spawn_next(&g, 0);
    fun_check(g.over, "game over when the spawn position is blocked");
    game_key(&g, KEY_ENTER, 0);
    fun_check(!g.over && board_cells(&g) == 0, "Enter starts a new game");
}

/* A long scripted game: random keys and time; the bookkeeping holds. */
static void test_random_game(void)
{
    struct game g;
    char what[96];
    game_new(&g, 2024, 0);
    uint64_t r = 99;
    static const int keys[] = { KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP, 'z', ' ', 'c',
                                KEY_LEFT, KEY_LEFT, KEY_RIGHT, KEY_RIGHT, ' ' };
    uint32_t games = 0, total_pieces = 0, total_lines = 0;
    uint64_t t = 0;
    bool ok = true;
    for (int i = 0; i < 20000; i++) {
        t += (rng_next(&r) % 300) * NS_PER_MS;
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
    fun_check(ok && games > 0, what);
}

int tetris_selftest(void)
{
    fun_selftest_begin("tetris", 60);
    test_bag();
    test_hard_drop();
    test_walls();
    test_rotation();
    test_tetris();
    test_single();
    test_gravity();
    test_hold();
    test_game_over();
    test_random_game();
    return fun_selftest_end();
}
