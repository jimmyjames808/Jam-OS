/* mines: the board's rules (mines.h). They never touch the screen or the
 * mouse and take the time as an argument, so the self-test can play whole
 * games with a fake clock.
 *
 * The mines are laid by the first reveal, not by board_new: that is how
 * the first click is never a mine. A cell's number (near) is fixed then.
 * The game is WON when every cell without a mine is open (flags don't
 * matter), LOST when a mine is opened. */
#include "mines.h"

const struct level levels[NLEVELS] = {
    { "Beginner", 9, 9, 10 },
    { "Intermediate", 16, 16, 40 },
    { "Expert", 30, 16, 99 },
    { "Giant", MAX_BW, MAX_BH, 180 },
};

void board_new(struct board *b, const struct level *lv, uint64_t seed)
{
    memset(b, 0, sizeof(*b));
    b->w = lv->w;
    b->h = lv->h;
    b->mines = lv->mines;
    b->rng = seed | 1;
    b->boom_x = b->boom_y = -1;
}

static bool inside(const struct board *b, int x, int y)
{
    return x >= 0 && y >= 0 && x < b->w && y < b->h;
}

/* The eight cells around one, as (dx, dy). */
static const int8_t ring[8][2] = {
    { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 },
};

/* How many of the cells around (x, y) have `bit` set. */
static int count_near(const struct board *b, int x, int y, uint8_t bit)
{
    int n = 0;
    for (int k = 0; k < 8; k++) {
        int nx = x + ring[k][0], ny = y + ring[k][1];
        n += inside(b, nx, ny) && (b->cell[ny][nx] & bit);
    }
    return n;
}

int board_flags_near(const struct board *b, int x, int y)
{
    return count_near(b, x, y, CELL_FLAG);
}

/* Lay the mines: a random choice among the cells that may hold one (not
 * (sx, sy); and not its neighbours either, if the board has room). */
static void lay_mines(struct board *b, int sx, int sy)
{
    static uint16_t spot[MAX_BW * MAX_BH];
    bool spare_near = b->w * b->h - 9 >= b->mines;
    int n = 0;
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            int dx = x - sx, dy = y - sy;
            bool near = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
            if ((x == sx && y == sy) || (spare_near && near))
                continue;
            spot[n++] = (uint16_t)(y * MAX_BW + x);
        }
    int mines = b->mines < n ? b->mines : n;
    for (int i = 0; i < mines; i++) {   /* the first `mines` of a shuffle */
        int j = i + (int)(rng_next(&b->rng) % (uint64_t)(n - i));
        uint16_t t = spot[i];
        spot[i] = spot[j];
        spot[j] = t;
        b->cell[spot[i] / MAX_BW][spot[i] % MAX_BW] |= CELL_MINE;
    }
    b->mines = mines;
    board_numbers(b);
}

void board_numbers(struct board *b)
{
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++)
            b->near[y][x] = (uint8_t)count_near(b, x, y, CELL_MINE);
    b->laid = true;
}

/* The cells opened but not yet looked around. A cell goes on the list
 * once, when it is opened, so the list can't outgrow the board. */
static uint16_t todo[MAX_BW * MAX_BH];
static int ntodo;

/* Open (x, y) if it is on the board, closed and not flagged. */
static void open_cell(struct board *b, int x, int y)
{
    if (!inside(b, x, y) || (b->cell[y][x] & (CELL_OPEN | CELL_FLAG)))
        return;
    b->cell[y][x] |= CELL_OPEN;
    todo[ntodo++] = (uint16_t)(y * MAX_BW + x);
}

/* Open a closed cell without a mine and, from a cell with no mines around
 * it, everything connected to it (such a cell's neighbours are never
 * mines). Returns the cells opened. */
static int open_area(struct board *b, int x, int y)
{
    int opened = 0;
    ntodo = 0;
    open_cell(b, x, y);
    while (ntodo) {
        int at = todo[--ntodo], cx = at % MAX_BW, cy = at / MAX_BW;
        opened++;
        for (int k = 0; k < 8 && !b->near[cy][cx]; k++)
            open_cell(b, cx + ring[k][0], cy + ring[k][1]);
    }
    b->opened += opened;
    return opened;
}

int board_reveal(struct board *b, int x, int y, uint64_t now)
{
    if (b->state != PLAYING || !inside(b, x, y) || (b->cell[y][x] & (CELL_OPEN | CELL_FLAG)))
        return 0;
    if (!b->laid) {
        lay_mines(b, x, y);
        b->started = now ? now : 1;
    }
    if (b->cell[y][x] & CELL_MINE) {
        b->cell[y][x] |= CELL_OPEN;
        b->state = LOST;
        b->boom_x = x;
        b->boom_y = y;
        b->ended = now;
        return 0;
    }
    int opened = open_area(b, x, y);
    if (b->opened == b->w * b->h - b->mines) {
        b->state = WON;
        b->ended = now;
    }
    return opened;
}

/* (x, y) is an open cell with a number on it. */
static bool is_number(const struct board *b, int x, int y)
{
    return inside(b, x, y) && (b->cell[y][x] & CELL_OPEN) && !(b->cell[y][x] & CELL_MINE) &&
           b->near[y][x];
}

enum right board_right(struct board *b, int x, int y, uint64_t now)
{
    if (b->state != PLAYING || !inside(b, x, y))
        return RIGHT_NOTHING;
    if (!(b->cell[y][x] & CELL_OPEN)) {
        b->cell[y][x] ^= CELL_FLAG;
        b->flags += (b->cell[y][x] & CELL_FLAG) ? 1 : -1;
        return RIGHT_FLAG;
    }
    if (!is_number(b, x, y))
        return RIGHT_NOTHING;
    if (board_flags_near(b, x, y) != b->near[y][x])
        return RIGHT_PEEK;
    /* The chord: a wrong flag leaves a mine among these, and opening it
     * loses the game, which stops the reveals after it. */
    for (int k = 0; k < 8; k++)
        (void)board_reveal(b, x + ring[k][0], y + ring[k][1], now);   /* the count isn't needed */
    return RIGHT_CHORD;
}

bool board_peeks(const struct board *b, int x, int y, int nx, int ny)
{
    int dx = nx - x, dy = ny - y;
    if (b->state != PLAYING || !is_number(b, x, y) || !inside(b, nx, ny) || (!dx && !dy) ||
        dx < -1 || dx > 1 || dy < -1 || dy > 1)
        return false;
    return board_flags_near(b, x, y) != b->near[y][x] &&
           !(b->cell[ny][nx] & (CELL_OPEN | CELL_FLAG));
}

int board_mines_left(const struct board *b)
{
    return b->mines - b->flags;
}

uint32_t board_seconds(const struct board *b, uint64_t now)
{
    if (!b->started)
        return 0;
    uint64_t end = b->state == PLAYING ? now : b->ended;
    uint64_t s = end > b->started ? (end - b->started) / NS_PER_S : 0;
    return s > 999 ? 999 : (uint32_t)s;
}
