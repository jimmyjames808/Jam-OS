/* snake: the rules (snake.h). They never touch the screen and take the
 * time as an argument, so the self-test can drive them with a fake clock.
 *
 * The snake moves one cell a step. A step first takes the oldest turn
 * typed, then moves the head: into a wall or into itself ends the game
 * (the cell the tail is leaving in the same step is free), onto the apple
 * makes it one cell longer, scores, puts a new apple on a free cell and
 * shortens the step. A field with no free cell left is a win. */
#include "snake.h"

#define STEP_START_NS (150 * NS_PER_MS)
#define STEP_FLOOR_NS (55 * NS_PER_MS)

static const int8_t dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };

uint64_t step_interval(uint32_t eaten)
{
    uint64_t ns = STEP_START_NS;
    for (uint32_t i = 0; i < eaten && ns > STEP_FLOOR_NS; i++)
        ns = ns * 97 / 100;
    return ns < STEP_FLOOR_NS ? STEP_FLOOR_NS : ns;
}

/* The apple onto a free cell, each as likely as another; with none left
 * the snake fills the field: won. */
static void place_food(struct game *g, uint64_t now)
{
    int cells = g->w * g->h, free_cells = cells - g->len;
    if (free_cells <= 0) {
        g->over = g->won = true;
        g->over_at = now;
        return;
    }
    int k = (int)(rng_next(&g->rng) % (uint64_t)free_cells);
    for (int c = 0; c < cells; c++)
        if (!g->used[c] && k-- == 0) {
            g->food = (uint16_t)c;
            return;
        }
}

void game_new(struct game *g, int w, int h, uint64_t seed, uint64_t now)
{
    memset(g, 0, sizeof(*g));
    g->w = w;
    g->h = h;
    g->rng = seed | 1;
    g->dir = DIR_RIGHT;
    g->len = START_LEN;
    for (int i = 0; i < START_LEN; i++) {   /* the head first, the rest to its left */
        int c = (h / 2) * w + w / 2 - i;
        g->body[i] = (uint16_t)c;
        g->used[c] = 1;
    }
    g->left = g->body[START_LEN - 1];
    g->step_ns = step_interval(0);
    g->stepped_at = now;
    g->step_at = now + g->step_ns;
    place_food(g, now);
}

static void turn(struct game *g, int dir)
{
    int last = g->nturns ? g->turn[g->nturns - 1] : g->dir;
    if (dir == last || dir == (last + 2) % 4 || g->nturns == 2)
        return;
    g->turn[g->nturns++] = dir;
}

void game_key(struct game *g, int key, uint64_t now)
{
    if (g->over) {
        if (key == KEY_ENTER || key == ' ')
            game_new(g, g->w, g->h, g->rng, now);
        return;
    }
    if (key == 'p' || key == 'P') {
        g->paused = !g->paused;
        if (g->paused) {
            g->paused_at = now;
        } else {   /* the clock stood still meanwhile */
            g->step_at += now - g->paused_at;
            g->stepped_at += now - g->paused_at;
        }
        return;
    }
    if (g->paused)
        return;
    switch (key) {
    case KEY_UP: case 'w': case 'W': turn(g, DIR_UP); break;
    case KEY_RIGHT: case 'd': case 'D': turn(g, DIR_RIGHT); break;
    case KEY_DOWN: case 's': case 'S': turn(g, DIR_DOWN); break;
    case KEY_LEFT: case 'a': case 'A': turn(g, DIR_LEFT); break;
    }
}

static void eat(struct game *g, uint64_t now)
{
    g->eaten++;
    g->score += 10 * game_level(g);
    g->grow++;
    g->step_ns = step_interval(g->eaten);
    g->ate_cell = g->food;
    g->ate_at = now;
    place_food(g, now);
}

void game_step(struct game *g, uint64_t now)
{
    if (g->over || g->paused)
        return;
    if (g->nturns) {
        g->dir = g->turn[0];
        g->turn[0] = g->turn[1];
        g->nturns--;
    }
    int at = g->body[g->head], x = at % g->w + dx[g->dir], y = at / g->w + dy[g->dir];
    int tail = g->body[(g->head + g->len - 1) % CELLS], to = y * g->w + x;
    bool growing = g->grow > 0;
    bool wall = x < 0 || y < 0 || x >= g->w || y >= g->h;
    if (wall || (g->used[to] && (growing || to != tail))) {
        g->over = true;
        g->over_at = now;
        return;
    }
    g->left = (uint16_t)tail;
    if (growing) {
        g->grow--;
    } else {
        g->used[tail] = 0;
        g->len--;
    }
    g->head = (g->head + CELLS - 1) % CELLS;
    g->body[g->head] = (uint16_t)to;
    g->used[to] = 1;
    g->len++;
    if (to == g->food)
        eat(g, now);
}

void game_tick(struct game *g, uint64_t now)
{
    if (g->over || g->paused)
        return;
    /* Far behind (the program stood still): don't race through the steps
     * missed, carry on from now. */
    if (now > g->step_at && now - g->step_at > 4 * g->step_ns)
        g->step_at = now;
    while (!g->over && now >= g->step_at) {
        g->stepped_at = g->step_at;
        game_step(g, g->step_at);
        g->step_at += g->step_ns;
    }
}

uint64_t game_deadline(const struct game *g)
{
    return g->over || g->paused ? DEADLINE_NEVER : g->step_at;
}
