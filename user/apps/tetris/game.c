/* tetris: the rules (tetris.h). They never touch the screen and take the
 * time as an argument, so the self-test can drive them with a fake clock. */
#include "tetris.h"

const int8_t shape[NPIECES][4][4][2] = {
    [I] = { { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }, { { 2, 0 }, { 2, 1 }, { 2, 2 }, { 2, 3 } },
            { { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 } },
            { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 } } },
    [O] = { { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } },
            { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } },
            { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } } },
    [T] = { { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
            { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, 2 } } },
    [S] = { { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } }, { { 1, 0 }, { 1, 1 }, { 2, 1 }, { 2, 2 } },
            { { 1, 1 }, { 2, 1 }, { 0, 2 }, { 1, 2 } },
            { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 2 } } },
    [Z] = { { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } }, { { 2, 0 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 1, 2 }, { 2, 2 } },
            { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 0, 2 } } },
    [J] = { { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 2, 0 }, { 1, 1 }, { 1, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 2, 2 } },
            { { 1, 0 }, { 1, 1 }, { 0, 2 }, { 1, 2 } } },
    [L] = { { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }, { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 2, 2 } },
            { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 2 } },
            { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 1, 2 } } },
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

bool fits(const struct game *g, int type, int rot, int x, int y)
{
    for (int k = 0; k < 4; k++) {
        int cx = x + shape[type][rot][k][0], cy = y + shape[type][rot][k][1];
        if (cx < 0 || cx >= BW || cy < 0 || cy >= BH || g->board[cy][cx])
            return false;
    }
    return true;
}

int bag_take(struct game *g)
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

void appear(struct game *g, int type, uint64_t now)
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

void spawn_next(struct game *g, uint64_t now)
{
    int t = g->next[0];
    g->next[0] = g->next[1];
    g->next[1] = g->next[2];
    g->next[2] = bag_take(g);
    g->held = false;
    appear(g, t, now);
}

void game_new(struct game *g, uint64_t seed, uint64_t now)
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

int drop_distance(const struct game *g)
{
    int d = 0;
    while (fits(g, g->type, g->rot, g->x, g->y + d + 1))
        d++;
    return d;
}

/* One key press. */
void game_key(struct game *g, int key, uint64_t now)
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
void game_tick(struct game *g, uint64_t now)
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
uint64_t game_deadline(const struct game *g)
{
    if (g->over || g->paused)
        return DEADLINE_NEVER;
    uint64_t d = g->fall_at;
    if (g->lock_at && g->lock_at < d)
        d = g->lock_at;
    return d;
}

uint32_t board_cells(const struct game *g)
{
    uint32_t n = 0;
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            n += g->board[y][x] != 0;
    return n;
}
