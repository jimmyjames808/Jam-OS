/* snake: the self-test (`run snake --selftest`): the rules with scripted
 * keys and a fake clock. */
#include "snake.h"

static int head_x(const struct game *g) { return game_cell(g, 0) % g->w; }
static int head_y(const struct game *g) { return game_cell(g, 0) / g->w; }

/* One key, then one step. */
static void move(struct game *g, int key)
{
    game_key(g, key, 0);
    game_step(g, 0);
}

/* The snake is what the counters say: `len` different cells, each next to
 * the one before, all marked used and nothing else; the apple is off it. */
static bool consistent(const struct game *g)
{
    int used = 0;
    for (int c = 0; c < g->w * g->h; c++)
        used += g->used[c];
    bool ok = used == g->len && (g->won || !g->used[g->food]);
    for (int i = 0; i < g->len; i++) {
        int c = game_cell(g, i);
        ok &= g->used[c] == 1;
        if (i == 0)
            continue;
        int p = game_cell(g, i - 1), ddx = c % g->w - p % g->w, ddy = c / g->w - p / g->w;
        ok &= ddx * ddx + ddy * ddy == 1;
    }
    return ok;
}

static void test_start_and_moves(void)
{
    struct game g;
    game_new(&g, FW, FH, 1, 0);
    fun_check(g.len == START_LEN && g.dir == DIR_RIGHT && head_x(&g) == FW / 2 &&
                  head_y(&g) == FH / 2 && consistent(&g),
              "a new game: 4 long in the middle, heading right, the apple off the snake");
    g.food = 0;   /* out of the way */
    int tail = game_cell(&g, g.len - 1);
    game_step(&g, 0);
    fun_check(head_x(&g) == FW / 2 + 1 && g.len == START_LEN && !g.used[tail] && consistent(&g),
              "a step moves the head one cell on and the tail follows");
    move(&g, KEY_UP);
    bool ok = head_y(&g) == FH / 2 - 1 && head_x(&g) == FW / 2 + 1;
    move(&g, KEY_DOWN);   /* straight back: ignored */
    ok &= head_y(&g) == FH / 2 - 2;
    move(&g, 'a');
    move(&g, 's');
    ok &= head_x(&g) == FW / 2 && head_y(&g) == FH / 2 - 1;
    fun_check(ok && consistent(&g), "arrows and WASD turn; a turn straight back is ignored");
}

/* Two quick turns are both taken, a step apart; a third is dropped. */
static void test_turn_queue(void)
{
    struct game g;
    game_new(&g, FW, FH, 1, 0);
    g.food = 0;
    game_key(&g, KEY_UP, 0);
    game_key(&g, KEY_LEFT, 0);
    game_key(&g, KEY_DOWN, 0);   /* no room: dropped */
    game_step(&g, 0);
    bool ok = head_x(&g) == FW / 2 && head_y(&g) == FH / 2 - 1;
    game_step(&g, 0);
    ok &= head_x(&g) == FW / 2 - 1 && head_y(&g) == FH / 2 - 1;
    game_step(&g, 0);
    ok &= head_x(&g) == FW / 2 - 2 && head_y(&g) == FH / 2 - 1 && !g.over;
    fun_check(ok, "two turns typed within a step are both taken, in order; a third is dropped");
}

static void test_eating(void)
{
    struct game g;
    char what[96];
    game_new(&g, FW, FH, 1, 0);
    g.food = (uint16_t)(game_cell(&g, 0) + 1);
    game_step(&g, 0);
    bool ok = g.eaten == 1 && g.score == 10 && g.len == START_LEN && g.grow == 1 && consistent(&g);
    g.food = 0;
    game_step(&g, 0);
    fun_check(ok && g.len == START_LEN + 1 && g.grow == 0 && consistent(&g),
              "an apple scores 10 and makes the snake one cell longer, a new apple appears");
    for (int i = 0; i < 4; i++) {   /* the fifth apple is worth 20: speed 2 */
        g.food = (uint16_t)(game_cell(&g, 0) + 1);
        game_step(&g, 0);
    }
    snprintf(what, sizeof(what), "every fifth apple raises the speed and the score (score %u)",
             g.score);
    fun_check(g.eaten == 5 && game_level(&g) == 2 && g.score == 10 * 4 + 20 && !g.over, what);
    bool faster = step_interval(0) == 150 * NS_PER_MS;
    for (uint32_t n = 1; n < 80; n++)
        faster &= step_interval(n) <= step_interval(n - 1) && step_interval(n) >= 55 * NS_PER_MS;
    fun_check(faster && step_interval(10) < step_interval(0) &&
                  step_interval(79) == 55 * NS_PER_MS && g.step_ns == step_interval(5),
              "a step takes 150 ms at first, less with every apple, never under 55 ms");
}

static void test_dying(void)
{
    struct game g;
    game_new(&g, FW, FH, 1, 0);
    g.food = 0;
    int steps = 0;
    while (!g.over && steps < 100) {
        game_step(&g, 7);
        steps++;
    }
    fun_check(g.over && !g.won && steps == FW - FW / 2 && head_x(&g) == FW - 1 && g.over_at == 7,
              "the right wall ends the game, the head stays on the last cell");
    game_key(&g, KEY_UP, 0);
    game_step(&g, 0);
    bool dead = g.over && head_x(&g) == FW - 1;
    game_key(&g, KEY_ENTER, 0);
    fun_check(dead && !g.over && g.len == START_LEN && g.score == 0,
              "after the end only Enter does anything: a new game");

    /* A square turn: 4 long, the head takes the cell the tail is leaving. */
    game_new(&g, FW, FH, 1, 0);
    g.food = 0;
    move(&g, KEY_UP);
    move(&g, KEY_LEFT);
    move(&g, KEY_DOWN);
    fun_check(!g.over && consistent(&g),
              "the head may enter the cell the tail leaves in that step");
    /* One longer, the same turn bites. */
    game_new(&g, FW, FH, 1, 0);
    g.food = (uint16_t)(game_cell(&g, 0) + 1);
    game_step(&g, 0);
    g.food = 0;
    game_step(&g, 0);
    move(&g, KEY_UP);
    move(&g, KEY_LEFT);
    move(&g, KEY_DOWN);
    fun_check(g.over && !g.won && g.len == START_LEN + 1,
              "running into its own body ends the game");
}

static void test_clock(void)
{
    struct game g;
    game_new(&g, FW, FH, 1, 0);
    g.food = 0;
    game_tick(&g, 149 * NS_PER_MS);
    bool ok = head_x(&g) == FW / 2;
    game_tick(&g, 150 * NS_PER_MS);
    ok &= head_x(&g) == FW / 2 + 1 && game_deadline(&g) == 300 * NS_PER_MS;
    fun_check(ok, "the clock: a step every 150 ms at the start");
    game_key(&g, 'p', 200 * NS_PER_MS);
    game_tick(&g, 5000 * NS_PER_MS);
    ok = g.paused && head_x(&g) == FW / 2 + 1 && game_deadline(&g) == DEADLINE_NEVER;
    game_key(&g, KEY_UP, 5000 * NS_PER_MS);   /* ignored while paused */
    game_key(&g, 'p', 5000 * NS_PER_MS);
    game_tick(&g, 5099 * NS_PER_MS);
    ok &= head_x(&g) == FW / 2 + 1;
    game_tick(&g, 5100 * NS_PER_MS);
    fun_check(ok && head_x(&g) == FW / 2 + 2 && head_y(&g) == FH / 2,
              "p pauses: no steps, no turns; the next step comes as long after as it was due");
}

/* Where a snake that fills an 8 x 4 field goes: along row 0 to the right,
 * down the rows in a zigzag through columns 1..7, back up column 0. */
static int cycle_key(int x, int y, int w, int h)
{
    if (x == 0)
        return y > 0 ? KEY_UP : KEY_RIGHT;
    if (y % 2 == 0)
        return x < w - 1 ? KEY_RIGHT : KEY_DOWN;
    if (x > 1)
        return KEY_LEFT;
    return y == h - 1 ? KEY_LEFT : KEY_DOWN;
}

static void test_win(void)
{
    struct game g;
    char what[96];
    game_new(&g, 8, 4, 11, 0);
    bool ok = true;
    int steps = 0;
    while (!g.over && steps < 5000) {
        move(&g, cycle_key(head_x(&g), head_y(&g), 8, 4));
        ok &= consistent(&g);
        steps++;
    }
    snprintf(what, sizeof(what), "a snake that never errs fills an 8x4 field and wins (%d steps)",
             steps);
    /* The last apple is eaten with the field already full: one more than the growth. */
    fun_check(ok && g.won && g.len == 32 && g.eaten == 32 - START_LEN + 1, what);
}

/* A long game of random keys: the bookkeeping holds through every death. */
static void test_random_game(void)
{
    struct game g;
    char what[96];
    game_new(&g, FW, FH, 2024, 0);
    static const int keys[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, 'w', 'a', 's', 'd', 'p', 0,
                                0, 0, 0, 0 };
    uint64_t r = 77, t = 0;
    uint32_t games = 0, apples = 0;
    bool ok = true;
    for (int i = 0; i < 40000; i++) {
        t += (rng_next(&r) % 120) * NS_PER_MS;
        int k = keys[rng_next(&r) % (sizeof(keys) / sizeof(keys[0]))];
        if (k)
            game_key(&g, k, t);
        game_tick(&g, t);
        ok &= consistent(&g) && g.len == START_LEN + (int)g.eaten - g.grow;
        if (g.over) {
            games++;
            apples += g.eaten;
            game_key(&g, KEY_ENTER, t);
        }
    }
    snprintf(what, sizeof(what), "40000 random keys: %u games, %u apples, consistent", games,
             apples);
    fun_check(ok && games > 0 && apples > 0, what);
}

int snake_selftest(void)
{
    fun_selftest_begin("snake", 78);
    test_start_and_moves();
    test_turn_queue();
    test_eating();
    test_dying();
    test_clock();
    test_win();
    test_random_game();
    return fun_selftest_end();
}
