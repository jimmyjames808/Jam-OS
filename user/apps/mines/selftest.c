/* mines: the self-test (`run mines --selftest`): the board's rules on
 * boards laid by hand and at random, with a fake clock; and the pointer
 * arithmetic of the mouse (libfun's, tested here because this is the app
 * that lives on it). */
#include "mines.h"

/* A board from a picture: '*' a mine, anything else none. */
static void board_from(struct board *b, const char *const *rows, int h)
{
    struct level lv = { "test", (int)strlen(rows[0]), h, 0 };
    board_new(b, &lv, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < lv.w; x++)
            if (rows[y][x] == '*') {
                b->cell[y][x] |= CELL_MINE;
                b->mines++;
            }
    board_numbers(b);
}

static int count(const struct board *b, uint8_t bit)
{
    int n = 0;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++)
            n += (b->cell[y][x] & bit) != 0;
    return n;
}

/* The first reveal is never a mine, wherever it is, and on a board with
 * room nothing next to it is either; the mines are all laid. */
static void test_first_click(void)
{
    bool safe = true, zero = true, counted = true;
    for (int lv = 0; lv < NLEVELS; lv++)
        for (uint64_t seed = 1; seed <= 40; seed++) {
            struct board b;
            board_new(&b, &levels[lv], seed * 977);
            int x = (int)(seed * 7 % (uint64_t)b.w), y = (int)(seed * 13 % (uint64_t)b.h);
            int opened = board_reveal(&b, x, y, 5);
            safe &= b.state != LOST && opened >= 1 && (b.cell[y][x] & CELL_OPEN);
            zero &= b.near[y][x] == 0;
            counted &= count(&b, CELL_MINE) == levels[lv].mines && b.mines == levels[lv].mines;
        }
    fun_check(safe, "first reveal: never a mine (4 levels x 40 seeds)");
    fun_check(zero, "first reveal: no mine next to it either");
    fun_check(counted, "every level lays exactly its number of mines");
    /* A board too full to spare the neighbours still spares the cell. */
    struct level full = { "full", 3, 3, 8 };
    struct board b;
    board_new(&b, &full, 3);
    int opened = board_reveal(&b, 1, 1, 5);
    fun_check(opened == 1 && b.state == WON && b.near[1][1] == 8,
              "3x3 with 8 mines: the first reveal is the one safe cell, and wins");
}

/* Mines at (1, 0) and (5, 4). The corner (0, 0) is a number with only the
 * mine and two other numbers around it: no flood reaches it. */
static const char *const lake[] = {
    ".*....",
    "......",
    "......",
    "......",
    ".....*",
};

/* Numbers, and the flood from a cell with no mines around. */
static void test_flood(void)
{
    struct board b;
    board_from(&b, lake, 5);
    fun_check(b.near[0][0] == 1 && b.near[1][1] == 1 && b.near[3][4] == 1 && b.near[2][2] == 0,
              "numbers count the mines around a cell");
    int opened = board_reveal(&b, 0, 4, 1);
    /* Everything but the two mines and the corner. */
    bool ok = opened == 30 - 2 - 1 && !(b.cell[0][0] & CELL_OPEN) && (b.cell[1][1] & CELL_OPEN) &&
              b.state == PLAYING;
    fun_check(ok, "flood: a 0 opens all that connects to it, numbers on its shore");
    fun_check(board_reveal(&b, 0, 4, 1) == 0, "an open cell can't be revealed again");
    board_reveal(&b, 0, 0, 3);
    fun_check(b.state == WON && b.ended == 3 && b.opened == 28,
              "win: every cell without a mine is open");
    fun_check(board_right(&b, 1, 0, 4) == RIGHT_NOTHING && board_reveal(&b, 1, 0, 4) == 0,
              "after the end nothing changes");
}

/* Flags, the counter, and that a flag protects a cell. */
static void test_flags(void)
{
    struct board b;
    board_from(&b, lake, 5);
    bool ok = board_right(&b, 1, 0, 1) == RIGHT_FLAG && board_mines_left(&b) == 1;
    ok &= board_reveal(&b, 1, 0, 1) == 0 && b.state == PLAYING;   /* a flagged mine: safe */
    ok &= board_right(&b, 3, 2, 1) == RIGHT_FLAG && board_right(&b, 4, 2, 1) == RIGHT_FLAG;
    ok &= board_mines_left(&b) == -1;
    fun_check(ok, "right click on a closed cell flags it; a flagged cell can't be revealed");
    board_reveal(&b, 0, 4, 1);
    fun_check(!(b.cell[2][3] & CELL_OPEN) && (b.cell[3][3] & CELL_OPEN),
              "the flood leaves flagged cells closed");
    ok = board_right(&b, 3, 2, 1) == RIGHT_FLAG && !(b.cell[2][3] & CELL_FLAG) && b.flags == 2;
    fun_check(ok, "a second right click takes the flag off");
    board_reveal(&b, 5, 4, 9);
    fun_check(b.state == LOST && b.boom_x == 5 && b.boom_y == 4 && b.ended == 9,
              "revealing a mine loses the game and remembers which");
}

static const char *const pair[] = {
    "*....",
    ".....",
    "..*..",
    ".....",
    ".....",
};

/* A right click on a number with as many flags around it as it says. */
static void test_chord(void)
{
    struct board b;
    board_from(&b, pair, 5);
    board_reveal(&b, 1, 1, 1);   /* a 2: mines at (0,0) and (2,2) */
    bool ok = b.near[1][1] == 2 && b.opened == 1;
    board_right(&b, 0, 0, 1);
    board_right(&b, 2, 2, 1);
    ok &= board_right(&b, 1, 1, 2) == RIGHT_CHORD;
    /* (2, 0) and (0, 2) have no mine around: the chord floods on from
     * them, as a plain reveal would, to the far corner. */
    ok &= (b.cell[0][1] & CELL_OPEN) && (b.cell[1][0] & CELL_OPEN) && (b.cell[4][4] & CELL_OPEN);
    ok &= !(b.cell[0][0] & CELL_OPEN) && !(b.cell[2][2] & CELL_OPEN);
    fun_check(ok, "chord: flags equal the number: the rest around it opens, flooding from 0s");
    fun_check(b.state == WON && b.opened == 23,
              "  ... which was every safe cell of this board: won");

    board_from(&b, pair, 5);
    board_reveal(&b, 1, 1, 1);
    board_right(&b, 0, 0, 1);
    board_right(&b, 2, 1, 1);    /* wrong: the mine is at (2,2) */
    fun_check(board_right(&b, 1, 1, 2) == RIGHT_CHORD && b.state == LOST && b.boom_x == 2 &&
                  b.boom_y == 2,
              "chord with a wrong flag opens the mine: lost");
}

/* A right click on a number whose flags don't equal it changes nothing,
 * and names the cells it still covers. */
static void test_peek(void)
{
    struct board b;
    board_from(&b, pair, 5);
    board_reveal(&b, 1, 1, 1);
    board_right(&b, 0, 0, 1);   /* one flag, the number says 2 */
    struct board before = b;
    bool ok = board_right(&b, 1, 1, 2) == RIGHT_PEEK && !memcmp(&before, &b, sizeof(b));
    fun_check(ok, "peek: fewer flags than the number: nothing is revealed or flagged");
    int covered = 0;
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            covered += board_peeks(&b, 1, 1, x, y);
    ok = covered == 7 && !board_peeks(&b, 1, 1, 0, 0) && !board_peeks(&b, 1, 1, 1, 1) &&
         board_peeks(&b, 1, 1, 2, 2) && !board_peeks(&b, 1, 1, 3, 1);
    fun_check(ok, "  ... it covers its 7 closed, unflagged neighbours, no other cell");
    board_right(&b, 2, 2, 1);
    board_right(&b, 2, 1, 1);   /* three flags round a 2 */
    before = b;
    ok = board_right(&b, 1, 1, 2) == RIGHT_PEEK && !memcmp(&before, &b, sizeof(b)) &&
         board_peeks(&b, 1, 1, 0, 1) && !board_peeks(&b, 1, 1, 2, 1);
    fun_check(ok, "peek: more flags than the number: the same, nothing changes");
    board_right(&b, 2, 1, 1);   /* back to two flags: the number is met */
    fun_check(!board_peeks(&b, 1, 1, 0, 1), "a number with its flags met covers nothing");
    fun_check(!board_peeks(&b, 3, 3, 3, 4) && board_right(&b, 4, 4, 1) == RIGHT_FLAG,
              "a closed cell is not a number: no peek from it, a right click flags it");
}

static void test_clock(void)
{
    struct board b;
    board_new(&b, &levels[2], 7);
    bool ok = board_seconds(&b, 50 * NS_PER_S) == 0;
    board_reveal(&b, 3, 3, 100 * NS_PER_S);
    ok &= board_seconds(&b, 100 * NS_PER_S) == 0 && board_seconds(&b, 101 * NS_PER_S) == 1;
    ok &= board_seconds(&b, 5000 * NS_PER_S) == 999;
    fun_check(ok, "the clock starts at the first reveal and stops at 999");
}

/* Whole games at random: the counters always match the cells. */
static void test_random_games(void)
{
    uint64_t r = 2025;
    uint32_t won = 0, lost = 0, moves = 0;
    bool ok = true;
    char what[96];
    for (int game = 0; game < 300; game++) {
        struct board b;
        board_new(&b, &levels[game % NLEVELS], rng_next(&r));
        for (uint64_t t = 1; b.state == PLAYING && t < 3000; t++, moves++) {
            int x = (int)(rng_next(&r) % (uint64_t)b.w), y = (int)(rng_next(&r) % (uint64_t)b.h);
            if (rng_next(&r) % 4 == 0)
                board_right(&b, x, y, t);
            else
                board_reveal(&b, x, y, t);
            int open_safe = count(&b, CELL_OPEN) - (b.state == LOST);
            ok &= b.opened == open_safe && b.flags == count(&b, CELL_FLAG);
            ok &= (b.state == WON) == (b.laid && b.opened == b.w * b.h - b.mines);
        }
        won += b.state == WON;
        lost += b.state == LOST;
    }
    snprintf(what, sizeof(what), "300 random games, %u moves: %u lost, %u won, counters right",
             moves, lost, won);
    fun_check(ok && lost > 0, what);
}

/* The pointer: exact without acceleration and for slow movement with it,
 * further for quick movement, and always inside the screen. */
static void test_pointer(void)
{
    struct pointer p;
    pointer_init(&p, 1280, 800, false);
    bool ok = pointer_x(&p) == 640 && pointer_y(&p) == 400;
    pointer_move(&p, 100, -50);
    pointer_move(&p, -127, 127);
    ok &= pointer_x(&p) == 613 && pointer_y(&p) == 477;
    fun_check(ok, "pointer: starts mid-screen; without acceleration a count is a pixel");
    pointer_move(&p, -5000, -5000);
    ok = pointer_x(&p) == 0 && pointer_y(&p) == 0;
    pointer_move(&p, 30000, 30000);
    fun_check(ok && pointer_x(&p) == 1279 && pointer_y(&p) == 799,
              "pointer: clamped to the screen's corners");
    pointer_init(&p, 2560, 1440, true);
    for (int i = 0; i < 100; i++)
        pointer_move(&p, 3, -2);
    ok = pointer_x(&p) == 1280 + 300 && pointer_y(&p) == 720 - 200;
    fun_check(ok, "pointer: with acceleration, slow movement is still exact");
    pointer_init(&p, 2560, 1440, true);
    pointer_move(&p, 40, 0);
    int fast = pointer_x(&p) - 1280;
    pointer_init(&p, 2560, 1440, true);
    pointer_move(&p, 10, 0);
    int mid = pointer_x(&p) - 1280;
    fun_check(fast == 80 && mid > 10 && mid < 20,
              "pointer: a quick push goes up to twice as far");
}

int mines_selftest(void)
{
    fun_selftest_begin("mines", 72);
    test_first_click();
    test_flood();
    test_flags();
    test_chord();
    test_peek();
    test_clock();
    test_random_games();
    test_pointer();
    return fun_selftest_end();
}
