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
#include "tetris.h"

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
    if (!draw_setup()) {
        gfx_close();
        say("tetris: out of memory\n");
        return 1;
    }

    struct game g;
    game_new(&g, now() ^ 0x7e7715, now());
    uint32_t best = 0, games = 0, frames = 0;
    uint64_t period = NS_PER_S / hz, last = now(), t_start = last;
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
    uint64_t ms = (now() - t_start) / NS_PER_MS;
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
    vgrad(&scr.s, &(struct rect){ 0, 0, scr.w, scr.h }, 0x700010, 0x100008);
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
    vgrad(&scr.s, &(struct rect){ 0, 0, scr.w, scr.h }, 0x001060, 0x000818);
    text_shadow(&scr.s, 40, 40, scr.ui * 2, 0xffffff,
                "tetris --hang-test: holding the screen until killed (Ctrl+C)");
    gfx_present();
    say("tetris: hang test: holding the screen\n");
    for (;;)
        jam_nanosleep(now() + NS_PER_S);
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return tetris_selftest();
    if (has_arg(argc, argv, "--crash-test"))
        return crash_test();
    if (has_arg(argc, argv, "--hang-test"))
        return hang_test();
    return play(argc, argv);
}
