/* wltest's libfun modes (wltest.h): the mouse as an app sees it, and an
 * app that hangs or crashes holding the screen.
 *
 *     wltest --mouse [noaccel] [nomouse]
 *     wltest --hang-test
 *     wltest --crash-test
 *     wltest --selftest
 *
 * These go through libfun (<fun.h>), as the apps do, not libjwl alone:
 * gfx_open opens a window when there is a compositor, and otherwise (the
 * `nocomp` boot) borrows the screen and the keys from the console, and
 * gfx_mouse_open asks for the mouse. So the same mode tests the mouse
 * through the compositor's seat and, under `nocomp`, the path that has no
 * compositor at all: usb-bus -> drv/hid -> console -> the key channel of
 * the client that asked -> libfun's pointer (mouse.c: the position from
 * relative counts, acceleration, the buttons' edges, the wheel).
 *
 * --mouse logs everything the mouse does, one "wltest: mouse: " line each
 * (the serial log shows them: tools/shell-tests/mouse.txt and apps.txt
 * wait for them), and draws it: a dot where each click was (left apricot,
 * right blackcurrant, both at once raspberry), a line for each drag, the
 * wheel's count, and the arrow. A button's press and release are a click
 * when the pointer moved less than DRAG_PX between them, else a drag.
 * `noaccel` makes one mouse count one pixel (the tests need exact
 * places); `nomouse` doesn't ask for the mouse at all, so the console
 * must send nothing a key reader could misread. Every key is logged; q or
 * Esc quits, with the totals.
 *
 * --hang-test borrows the screen and reads no keys until it is killed
 * (Ctrl+C: the shell kills its job); --crash-test dies of a page fault
 * holding it. Either way the console (or the compositor) must take the
 * screen back by itself (tools/shell-tests/apps.txt).
 *
 * --selftest checks libfun's pointer arithmetic (mouse.c's struct
 * pointer) by itself, with no mouse: where it starts, a count a pixel
 * without acceleration, the clamping, and acceleration's gains. */
#include <fun.h>
#include "wltest.h"

#define DRAG_PX   4      /* a press and release further apart than this: a drag */
#define MARKS     64     /* the clicks and drags drawn (the newest) */
#define GRID      40     /* the background's grid, pixels */

#define C_BG      0x18141f
#define C_GRID    0x2a2433
#define C_TEXT    0xf0e8f4
#define C_APRICOT 0xef9f27   /* the logo's jams (docs/logo/README.md) */
#define C_CURRANT 0x7f77dd
#define C_RASP    0xd4537e

struct mark {
    int x0, y0, x1, y1;   /* a click: x0, y0 only */
    uint32_t colour;
    bool drag;
};

static struct {
    struct mark marks[MARKS];
    unsigned nmarks;                  /* ever made */
    int down_x[3], down_y[3];         /* where each button (left, right, middle) went down */
    unsigned clicks[3], drags[3], chords, keys;
    int wheel, notches;               /* the wheel's sum, and notches turned either way */
    bool chorded;                     /* this press of both has been said */
} ms;

static const char *const button_name[3] = { "left", "right", "middle" };
static const uint32_t button_colour[3] = { C_APRICOT, C_CURRANT, C_TEXT };

static void add_mark(const struct mark *m)
{
    ms.marks[ms.nmarks++ % MARKS] = *m;
}

static void draw(const struct mouse *m)
{
    fill(&scr.s, 0, 0, scr.w, scr.h, C_BG);
    for (int x = GRID; x < scr.w; x += GRID)
        fill(&scr.s, x, 0, 1, scr.h, C_GRID);
    for (int y = GRID; y < scr.h; y += GRID)
        fill(&scr.s, 0, y, scr.w, 1, C_GRID);
    unsigned first = ms.nmarks > MARKS ? ms.nmarks - MARKS : 0;
    for (unsigned i = first; i < ms.nmarks; i++) {
        const struct mark *k = &ms.marks[i % MARKS];
        if (k->drag) {
            line_aa(&scr.s, (float)k->x0 + 0.5f, (float)k->y0 + 0.5f, (float)k->x1 + 0.5f,
                    (float)k->y1 + 0.5f, 3, k->colour, 255);
            disc_aa(&scr.s, (float)k->x1 + 0.5f, (float)k->y1 + 0.5f, 5, k->colour, 255);
        } else {
            disc_aa(&scr.s, (float)k->x0 + 0.5f, (float)k->y0 + 0.5f, 8, k->colour, 255);
        }
    }
    int u = scr.ui;
    textf(&scr.s, 16 * u, 12 * u, u, C_TEXT,
          "wltest --mouse    pointer %d,%d    clicks %u / %u / %u    drags %u    chords %u    "
          "wheel %d    q quits",
          m->x, m->y, ms.clicks[0], ms.clicks[1], ms.clicks[2], ms.drags[0] + ms.drags[1] +
          ms.drags[2], ms.chords, ms.wheel);
    gfx_present();
}

/* What the mouse did since the last look: each edge a line. */
static void on_mouse(void)
{
    struct mouse m;
    gfx_mouse(&m);
    for (int b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        if (m.pressed & bit) {
            ms.down_x[b] = m.x;
            ms.down_y[b] = m.y;
            say("wltest: mouse: %s down at %d,%d\n", button_name[b], m.x, m.y);
        }
    }
    bool both = (m.buttons & (MOUSE_LEFT | MOUSE_RIGHT)) == (MOUSE_LEFT | MOUSE_RIGHT);
    if (both && !ms.chorded) {
        ms.chords++;
        say("wltest: mouse: chord at %d,%d (left and right held)\n", m.x, m.y);
        add_mark(&(struct mark){ m.x, m.y, 0, 0, C_RASP, false });
    }
    ms.chorded = both || (ms.chorded && (m.buttons & (MOUSE_LEFT | MOUSE_RIGHT)));
    for (int b = 0; b < 3; b++) {
        if (!(m.released & (1u << b)))
            continue;
        int dx = m.x - ms.down_x[b], dy = m.y - ms.down_y[b];
        bool drag = dx > DRAG_PX || dx < -DRAG_PX || dy > DRAG_PX || dy < -DRAG_PX;
        if (drag) {
            ms.drags[b]++;
            say("wltest: mouse: %s drag from %d,%d to %d,%d\n", button_name[b], ms.down_x[b],
                ms.down_y[b], m.x, m.y);
        } else {
            ms.clicks[b]++;
            say("wltest: mouse: %s click at %d,%d\n", button_name[b], m.x, m.y);
        }
        add_mark(&(struct mark){ ms.down_x[b], ms.down_y[b], m.x, m.y, button_colour[b], drag });
    }
    if (m.wheel) {
        ms.wheel += m.wheel;
        ms.notches += m.wheel < 0 ? -m.wheel : m.wheel;
        say("wltest: mouse: wheel %s%d at %d,%d (now %d)\n", m.wheel > 0 ? "+" : "", m.wheel,
            m.x, m.y, ms.wheel);   /* libos's printf has no '+' flag */
    }
    draw(&m);
}

int wl_fun_mouse(int argc, char **argv)
{
    bool want = !has_arg(argc, argv, "nomouse");
    gfx_title("wltest");
    pool_start(0);
    status_t st = gfx_open_on(C_BG);
    if (st == OK && want && (st = gfx_mouse_open(!has_arg(argc, argv, "noaccel"))) != OK)
        gfx_close();
    if (st != OK) {
        say("wltest: mouse: can't open a window or borrow the screen%s (%s)\n",
            want ? " and the mouse" : "", status_str(st));
        return 1;
    }
    struct mouse m;
    gfx_mouse(&m);
    if (want)
        say("wltest: mouse: ready, %dx%d (%s), pointer at %d,%d\n", scr.w, scr.h,
            scr.windowed ? "a window" : "the screen", m.x, m.y);
    else
        say("wltest: mouse: ready, %dx%d (%s), the mouse not asked for\n", scr.w, scr.h,
            scr.windowed ? "a window" : "the screen");
    draw(&m);
    for (bool quit = false; !quit;) {
        int k = gfx_key(DEADLINE_NEVER);
        for (; k != KEY_NONE && !quit; k = gfx_key(0)) {
            if (k == KEY_MOUSE) {
                on_mouse();
                continue;
            }
            if (k == KEY_RESIZE)
                continue;
            ms.keys++;
            say("wltest: mouse: key %#x\n", (unsigned)k);
            quit = k == KEY_QUIT || k == 'q' || k == 'Q';
        }
    }
    gfx_mouse(&m);
    gfx_close();
    say("wltest: mouse: %u clicks (%u left, %u right, %u middle), %u drags, %u chords, wheel %d "
        "(%d notches), %u keys, %u mouse reports\n",
        ms.clicks[0] + ms.clicks[1] + ms.clicks[2], ms.clicks[0], ms.clicks[1], ms.clicks[2],
        ms.drags[0] + ms.drags[1] + ms.drags[2], ms.chords, ms.wheel, ms.notches, ms.keys,
        m.reports);
    return 0;
}

/* Borrow the screen (or open a window) and draw a card that says why. */
static bool hold_screen(uint32_t top, uint32_t bottom, const char *what)
{
    gfx_title("wltest");
    pool_start(0);
    if (gfx_open() != OK)
        return false;
    vgrad(&scr.s, &(struct rect){ 0, 0, scr.w, scr.h }, top, bottom);
    text_shadow(&scr.s, 40, 40, scr.ui * 2, 0xffffff, what);
    gfx_present();
    return true;
}

int wl_fun_crash(void)
{
    if (!hold_screen(0x700010, 0x100008, "wltest --crash-test: dying with the screen borrowed"))
        return 1;
    jam_nanosleep(now() + 500 * NS_PER_MS);
    say("wltest: crash test: faulting now\n");
    volatile int *p;
    __asm__("" : "=r"(p) : "0"((uintptr_t)8));   /* hide the null from the compiler */
    return *p;   /* a page fault: the kernel kills us */
}

int wl_fun_hang(void)
{
    if (!hold_screen(0x001060, 0x000818,
                     "wltest --hang-test: holding the screen until killed (Ctrl+C)"))
        return 1;
    say("wltest: hang test: holding the screen\n");
    for (;;)
        jam_nanosleep(now() + NS_PER_S);
}

/* The pointer: exact without acceleration and for slow movement with it,
 * further for quick movement, and always inside the screen. (From mines's
 * self-test, removed with mines on 2026-10-07.) */
int wl_fun_selftest(void)
{
    fun_selftest_begin("wltest", 72);
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
    fun_check(fast == 80 && mid > 10 && mid < 20, "pointer: a quick push goes up to twice as far");
    return fun_selftest_end();
}
