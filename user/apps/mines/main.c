/* mines: minesweeper with the mouse, on the screen borrowed from the console.
 *
 *   run mines [level=2] [seed=N]   play (level 1..4: Beginner 9x9 with 10
 *                                  mines, Intermediate 16x16 / 40, Expert
 *                                  30x16 / 99, Giant 40x22 / 180)
 *   run mines --selftest           check the board's rules, exit 0 if right
 *   run mines ... noaccel trace    (tests) the pointer moves one pixel a
 *                                  mouse count; every click is said on the
 *                                  console, so the serial log shows it
 *
 * The mouse plays it:
 *   left click    on a closed cell: reveal it. The first reveal of a game
 *                 is never a mine; a cell with no mines around opens
 *                 everything connected to it. The cell is the one under
 *                 the pointer when the button comes up.
 *   right click   on a closed cell: set or clear its flag.
 *                 on a number: if as many flags are around it as it says,
 *                 open the rest around it (a wrong flag loses). Otherwise
 *                 nothing opens: the cells it still covers are shown
 *                 dimmed for as long as the button is held.
 *   the face      a new game; the buttons under the board: the level.
 * Keys: n a new game, 1-4 the level, q or Esc quits. There is no keyboard
 * play: with no mouse the bottom line says the game needs one.
 *
 * The rules (struct board) never touch the screen or the mouse, so the
 * self-test plays whole games against them. The frame is drawn only when
 * something changed (a click, the pointer on another cell, a second on
 * the clock); a pointer that only moved presents just the arrow's rows. */
#include <wants.h>
#include "mines.h"

/* What it is given when the shell runs it (<wants.h>): a window. */
JAM_WANTS("svc wayland\n");

struct session {
    struct board b;
    struct view  v;
    bool     trace;       /* say every click on the console (the mouse test reads it) */
    bool     fixed_seed;  /* seed= was given: every game uses it */
    uint64_t seed;        /* the next game's seed */
    bool     peeking;     /* the right button went down on an open cell and is still held */
    bool     counted;     /* this game's end is in the session's numbers */
    bool     redraw;      /* the frame must be drawn again */
};

static const char *const state_name[] = { "playing", "won", "lost" };

static bool new_game(struct session *s, int level)
{
    if (level != s->v.level || !s->b.w) {
        s->v.level = level;
        if (!draw_setup(&levels[level]))
            return false;
    }
    if (!s->fixed_seed)
        s->seed = s->seed * 6364136223846793005ull + now();
    board_new(&s->b, &levels[level], s->seed);
    s->v.peek_x = s->v.peek_y = -1;
    s->peeking = s->counted = false;
    s->redraw = true;
    if (s->trace)
        say("mines: new game: %s %dx%d, %d mines\n", levels[level].name, levels[level].w,
            levels[level].h, levels[level].mines);
    return true;
}

/* The game just ended (once per game): the session's numbers. */
static void count_end(struct session *s)
{
    if (s->b.state == PLAYING || s->counted)
        return;
    s->counted = true;
    s->v.games++;
    if (s->b.state != WON)
        return;
    s->v.wins++;
    uint32_t secs = board_seconds(&s->b, s->b.ended);
    secs = secs ? secs : 1;
    if (!s->v.best[s->v.level] || secs < s->v.best[s->v.level])
        s->v.best[s->v.level] = secs;
}

/* The left button came up at `hit`. */
static bool left_click(struct session *s, enum hit hit, int cx, int cy)
{
    if (hit == HIT_NEW)
        return new_game(s, s->v.level);
    if (hit == HIT_LEVEL)
        return new_game(s, cx);
    if (hit != HIT_CELL)
        return true;
    int opened = board_reveal(&s->b, cx, cy, now());
    if (s->trace)
        say("mines: reveal %d,%d: %d opened, %s\n", cx, cy, opened, state_name[s->b.state]);
    return true;
}

/* The right button went down on cell (cx, cy). */
static void right_click(struct session *s, int cx, int cy)
{
    static const char *const did[] = { "nothing", "flag", "chord", "peek" };
    enum right r = board_right(&s->b, cx, cy, now());
    s->peeking = r == RIGHT_PEEK || r == RIGHT_NOTHING;
    if (s->trace)
        say("mines: right %d,%d: %s, %d opened, %d left, %s\n", cx, cy, did[r], s->b.opened,
            board_mines_left(&s->b), state_name[s->b.state]);
}

/* What the mouse did since the last look. false: out of memory (a new level). */
static bool on_mouse(struct session *s)
{
    struct mouse m;
    gfx_mouse(&m);
    int cx = -1, cy = -1;
    enum hit hit = draw_hit(m.x, m.y, &cx, &cy);
    int hx = hit == HIT_CELL ? cx : -1, hy = hit == HIT_CELL ? cy : -1;
    bool ok = true;
    if (s->v.no_mouse || hx != s->v.hover_x || hy != s->v.hover_y || m.pressed || m.released)
        s->redraw = true;
    s->v.no_mouse = false;
    s->v.hover_x = hx;
    s->v.hover_y = hy;
    if ((m.pressed & MOUSE_RIGHT) && hit == HIT_CELL)
        right_click(s, cx, cy);
    if ((m.released & MOUSE_LEFT) && !(m.buttons & MOUSE_RIGHT))
        ok = left_click(s, hit, cx, cy);
    if (!(m.buttons & MOUSE_RIGHT))
        s->peeking = false;
    /* A peek follows the pointer: draw dims what board_peeks says of it. */
    s->v.peek_x = s->peeking ? s->v.hover_x : -1;
    s->v.peek_y = s->peeking ? s->v.hover_y : -1;
    s->v.pressing = (m.buttons & MOUSE_LEFT) && !(m.buttons & MOUSE_RIGHT);
    count_end(s);
    return ok;
}

/* One key (or KEY_MOUSE). false: quit, or out of memory. */
static bool on_key(struct session *s, int k)
{
    if (k == KEY_QUIT || k == 'q' || k == 'Q')
        return false;
    if (k == KEY_MOUSE)
        return on_mouse(s);
    if (k == 'n' || k == 'N')
        return new_game(s, s->v.level);
    if (k >= '1' && k < '1' + NLEVELS)
        return new_game(s, k - '1');
    return true;
}

/* When the clock on the screen next changes. */
static uint64_t next_second(const struct board *b, uint64_t t)
{
    if (b->state != PLAYING || !b->started)
        return DEADLINE_NEVER;
    return b->started + ((t - b->started) / NS_PER_S + 1) * NS_PER_S;
}

static int play(int argc, char **argv)
{
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    status_t st = gfx_open();
    if (st == OK && (st = gfx_mouse_open(!has_arg(argc, argv, "noaccel"))) != OK)
        gfx_close();
    if (st != OK) {
        say("mines: can't borrow the screen and the mouse (%s)\n", status_str(st));
        return 1;
    }
    static struct session s;
    s.trace = has_arg(argc, argv, "trace");
    s.seed = arg_num(argc, argv, "seed", 0);
    s.fixed_seed = s.seed != 0;
    s.v.no_mouse = true;
    s.v.hover_x = s.v.hover_y = -1;
    s.v.level = -1;
    uint64_t level = arg_num(argc, argv, "level", 2);
    bool ok = new_game(&s, level >= 1 && level <= NLEVELS ? (int)level - 1 : 1);
    while (ok) {
        uint64_t t = now();
        if (s.redraw) {
            draw(&s.b, &s.v, t);
            gfx_present();
            s.redraw = false;
        } else {
            gfx_present_pointer();
        }
        int k = gfx_key(next_second(&s.b, t));
        if (k == KEY_NONE)
            s.redraw = true;   /* the clock */
        for (; ok && k != KEY_NONE; k = gfx_key(0))
            ok = on_key(&s, k);   /* everything that came meanwhile, then draw */
    }
    gfx_close();
    say("mines: %u game(s) finished, %u won; %lu presents, %lu MB to the screen\n", s.v.games,
        s.v.wins, (unsigned long)scr.presents, (unsigned long)(scr.bytes >> 20));
    return 0;
}

int main(int argc, char **argv)
{
    gfx_title("Mines");
    if (has_arg(argc, argv, "--selftest"))
        return mines_selftest();
    return play(argc, argv);
}
