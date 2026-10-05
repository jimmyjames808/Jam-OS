/* snake: the snake game with the keyboard, on the screen borrowed from the console.
 *
 *   run snake [hz=60]      play (hz: the frame rate)
 *   run snake --selftest   check the rules with scripted keys, exit 0 if right
 *
 * A 30 x 20 field. The snake moves a cell a step, 150 ms at first and 3%
 * less with every apple (never under 55 ms); an apple makes it one cell
 * longer and scores 10 times the speed level (one more level every five
 * apples). A wall or its own body ends the game; filling the field wins.
 *
 * Keys: the arrows or w a s d turn (two turns typed quickly are both
 * taken, a step apart), p pauses, Enter or space starts again after the
 * end, q or Esc quits. The best score is this session's.
 *
 * The game logic (struct game) never touches the screen, so the self-test
 * drives it with scripted keys and a fake clock. The picture slides the
 * snake between the rules' whole-cell steps, so frames come at `hz` while
 * the game runs; paused or over, a frame is drawn only when a key changes
 * something, and gfx_present sends only the pixels that changed. */
#include <wants.h>
#include "snake.h"

/* What it is given when the shell runs it (<wants.h>): a window. */
JAM_WANTS("svc wayland\n");

static int play(int argc, char **argv)
{
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    uint32_t hz = (uint32_t)arg_num(argc, argv, "hz", 60);
    if (hz < 20 || hz > 240)
        hz = 60;
    status_t st = gfx_open();
    if (st != OK) {
        say("snake: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    if (!draw_setup()) {
        gfx_close();
        say("snake: out of memory\n");
        return 1;
    }

    static struct game g;
    uint64_t seed = arg_num(argc, argv, "seed", 0);
    game_new(&g, FW, FH, seed ? seed : now() ^ 0x5a4b3, now());
    uint32_t best = 0, games = 1, frames = 0;
    uint64_t period = NS_PER_S / hz, t_start = now();
    bool quit = false;
    while (!quit) {
        uint64_t t = now();
        game_tick(&g, t);
        if (g.score > best)
            best = g.score;
        draw(&g, t, best);
        gfx_present();
        frames++;
        /* The next frame: at the frame rate while the snake moves, else
         * when a key comes. */
        int k = gfx_key(g.over || g.paused ? DEADLINE_NEVER : t + period);
        for (; k != KEY_NONE && !quit; k = gfx_key(0)) {
            quit = k == KEY_QUIT || k == 'q' || k == 'Q';
            bool was_over = g.over;
            game_key(&g, k, now());
            games += was_over && !g.over;
        }
    }
    uint64_t ms = (now() - t_start) / NS_PER_MS;
    gfx_close();
    say("snake: score %u, length %d (best %u over %u game(s)); %u frames in %lu ms, "
        "%lu MB to the screen\n",
        g.score, g.len, best, games, frames, (unsigned long)ms, (unsigned long)(scr.bytes >> 20));
    return 0;
}

int main(int argc, char **argv)
{
    gfx_title("snake");
    if (has_arg(argc, argv, "--selftest"))
        return snake_selftest();
    return play(argc, argv);
}
