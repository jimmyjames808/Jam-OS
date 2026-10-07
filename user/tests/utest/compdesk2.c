/* utest: the desktop's cards and keys (user/services/compositor: menus.c,
 * popover.c, notify.c, desk.c), on compwm.c's harness as compdesk.c is.
 *
 * t_desk_alttab: the list's order (this screen's windows most recently
 * focused first, every other screen's grouped in the screens' order, the
 * minimised last); the first Tab selects the window before, each Tab the
 * next, Shift+Tab back, wrapping; letting go of Alt goes there (to its
 * screen, restored); Esc cancels; the list shows only once Alt was held
 * LOOK_ALTTAB_SHOW_MS.
 * t_desk_search: Super tapped alone opens and closes it (not with another
 * key or a click between, nor held too long); typing filters (names
 * starting with the text first), the rows the most recently run first, the
 * last "Run ..." for what is typed; arrows, Backspace; Enter runs the
 * selected app (ctl_launch, in lower case, the busy cursor until its first
 * window) or the command in a terminal (ctl_run_in_terminal); a click on a
 * row runs it, one outside closes it; every key is the box's while open.
 * t_desk_popover: each icon's popover 2 pixels under the strip, its right
 * edge on its icon's; one at a time; a click elsewhere or Esc closes it;
 * the volume slider sets the volume (ctl_set_volume) by a click and a
 * drag; the calendar starts on Monday (known months, leap years).
 * t_desk_notify: cards stacked down from under the strip, the newest on
 * top; one without buttons goes after LOOK_NOTE_SHOW_MS, one with buttons
 * stays until one is pressed (ctl_notify_answered), a click on a plain
 * card sends it; at most NOTIFY_MAX, the oldest plain one pushed out.
 * Init refusing a terminal (desk_terminals_full) posts "No more terminals"
 * with the limit's number, and not again while that card is up (a held
 * Super+Enter): once it has gone, the next refusal posts it again.
 * t_desk_notify_held: no notice shows while the boot splash is up (the
 * wait for it, its overlay on the screen, its fade-out): each gets its id
 * and waits; a withdrawn one is dropped unseen; when the splash is over
 * they show in the order they came, their 5 s counted from then; at most
 * NOTIFY_MAX held, the oldest plain one dropped; with no splash a notice
 * shows at once, and nothing wakes the loop while one is held.
 * t_desk_overlay: a full-screen window whose client takes no keys (the
 * boot splash's) is a boot overlay: no screen of its own (nor a dot), on
 * none (no chip, no Alt+Tab row, never cycled to), over all of the output
 * and every window (raised, focused or opened after it), the strip hidden
 * under it, staying put while screens change; not minimised; it fades out
 * when it goes (over the strip, its box), ending with the strip back and
 * nothing left of it; asked out of full screen it is a window on the
 * current screen. A client with keys still gets a screen of its own. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <keymap.h>
#include "compwm.h"
#include "utest.h"

#define U_A      0x04
#define U_E      0x08
#define U_I      0x0c
#define U_M      0x10
#define U_T      0x17
#define U_ENTER  0x28
#define U_ESC    0x29
#define U_BKSP   0x2a
#define U_TAB    0x2b
#define U_DOWN   0x51
#define U_LALT   0xe2
#define U_LGUI   0xe3
#define ALT      INPUT_MOD_LALT
#define SHIFT    INPUT_MOD_LSHIFT
#define SUPER    INPUT_MOD_LGUI

static bool key(uint16_t usage, uint8_t mods)
{
    return wm_test_key(usage, mods);
}

/* Super pressed and let go alone. */
static void super_tap(void)
{
    (void)key(U_LGUI, SUPER);
    desk_key_up(U_LGUI);
}

/* ---- Alt+Tab ------------------------------------------------------------------------------ */

static bool rows_are(const unsigned *want, unsigned n)
{
    CHECK_EQ(alttab.n, n);
    for (unsigned i = 0; i < n; i++)
        CHECK(alttab.rows[i].ww == fks[want[i]].ww);
    return true;
}

static bool alttab_order_steps(void)
{
    /* screen 1: windows 0, 1 (1 focused last); screen 2: 2, 3; then 4 minimised on 1 */
    CHECK(fk_open(&fks[0], 200, 100, false) && fk_open(&fks[1], 200, 100, false));
    CHECK(fk_open(&fks[4], 200, 100, false));
    seat_focus(win(0));
    seat_focus(win(1));
    wm_minimise(win(4));
    screens_step(1);
    CHECK(fk_open(&fks[2], 200, 100, false) && fk_open(&fks[3], 200, 100, false));
    seat_focus(win(3));
    seat_focus(win(2));
    screens_go(0);
    CHECK_EQ(seat.focused, win(1));   /* the screen's last focused */
    CHECK(key(U_TAB, ALT));
    CHECK(alttab.active && !alttab.shown);
    const unsigned order[] = { 1, 0, 2, 3, 4 };
    CHECK(rows_are(order, 5));
    CHECK(!alttab.rows[0].first && !alttab.rows[1].first && alttab.rows[2].first &&
          !alttab.rows[3].first && alttab.rows[4].first);
    CHECK_EQ(alttab.rows[2].group, 2);
    CHECK_EQ(alttab.sel, 1);   /* the window before */
    CHECK(box_empty(alttab_box()) == false);
    /* shown only after it is held a while */
    desk_tick(now());
    CHECK(!alttab.shown);
    desk_tick(alttab.since + (LOOK_ALTTAB_SHOW_MS + 1) * NS_PER_MS);
    CHECK(alttab.shown);
    return true;
}

static bool alttab_steps(void)
{
    CHECK(alttab_order_steps());
    /* Tab, Tab: window 3; Shift+Tab: 2; let go: screen 2, window 2 focused */
    CHECK(key(U_TAB, ALT));
    CHECK(key(U_TAB, ALT));
    CHECK_EQ(alttab.sel, 3);
    CHECK(key(U_TAB, ALT | SHIFT));
    CHECK_EQ(alttab.sel, 2);
    desk_key_up(U_LALT);
    CHECK(!alttab.active);
    CHECK_EQ(screens_cur_index(), 1);
    CHECK_EQ(seat.focused, win(2));
    /* wrapping back from the first: the minimised one, restored on screen 1 */
    CHECK(key(U_TAB, ALT | SHIFT));
    CHECK_EQ(alttab.sel, alttab.n - 1);
    CHECK(alttab.rows[alttab.sel].ww == fks[4].ww);
    desk_key_up(U_LALT);
    CHECK_EQ(screens_cur_index(), 0);
    CHECK(!fks[4].ww->minimised && seat.focused == win(4));
    /* Esc cancels: nothing moves */
    CHECK(key(U_TAB, ALT));
    CHECK(key(U_ESC, ALT));
    CHECK(!alttab.active);
    desk_key_up(U_LALT);
    CHECK_EQ(seat.focused, win(4));
    /* a quick Alt+Tab: the window before (4's before was 1) */
    CHECK(key(U_TAB, ALT));
    desk_key_up(U_LALT);
    CHECK_EQ(seat.focused, win(1));
    CHECK(!alttab.shown);
    return true;
}

bool t_desk_alttab(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = alttab_steps();
    fk_close_all();
    return ok;
}

/* ---- the search box ------------------------------------------------------------------------ */

static const char *row_name(unsigned i)
{
    return search.row[i] == DESK_APPS ? "run" : desk_apps[search.row[i]].name;
}

static bool search_open_steps(void)
{
    super_tap();
    CHECK(search.open);
    CHECK_EQ(search.nrows, DESK_APPS);   /* empty: every app */
    CHECK(!strcmp(row_name(0), "Terminal"));
    super_tap();
    CHECK(!search.open);
    /* Super with another key between: no tap */
    CHECK(!key(U_LGUI, SUPER));
    CHECK(!key(U_A, SUPER));
    desk_key_up(U_LGUI);
    CHECK(!search.open);
    /* nor with a click between */
    CHECK(!key(U_LGUI, SUPER));
    (void)desk_press(600, 600, BTN);
    desk_key_up(U_LGUI);
    CHECK(!search.open);
    return true;
}

static bool search_type_steps(void)
{
    super_tap();
    /* "a": the names holding it (none starts with it), then the run row */
    CHECK(key(U_A, 0));
    CHECK(!strcmp(search.text, "a"));
    CHECK_EQ(search.nrows, 3);
    CHECK(!strcmp(row_name(0), "Terminal") && !strcmp(row_name(1), "Jamjar"));
    CHECK(!strcmp(row_name(2), "run"));
    /* "m": only Jamjar holds "am"; "i": nothing holds "ai": only the run
     * row; Backspace: back */
    CHECK(key(U_M, 0));
    CHECK_EQ(search.nrows, 2);
    CHECK(!strcmp(row_name(0), "Jamjar"));
    CHECK(key(U_BKSP, 0));
    CHECK(key(U_I, 0));
    CHECK_EQ(search.nrows, 1);
    CHECK(key(U_BKSP, 0));
    CHECK_EQ(search.nrows, 3);
    /* Down, Enter: Jamjar runs (in lower case), the box closes, the cursor busy */
    CHECK(key(U_DOWN, 0));
    CHECK(key(U_ENTER, 0));
    CHECK(!search.open);
    CHECK(!strcmp(fdesk.launched, "jamjar"));
    CHECK(desk_busy());
    /* its first window: busy no more */
    CHECK(fk_open(&fks[0], 200, 100, false));
    wm_set_app_id(fks[0].ww, "jamjar");
    desk_window_mapped(fks[0].ww);
    CHECK(!desk_busy());
    /* opened again: Jamjar first (run last) */
    super_tap();
    CHECK(!strcmp(row_name(0), "Jamjar"));
    /* a command: "te" then Shift+i ("I"), Up past... the run row: Enter */
    CHECK(key(U_T, 0) && key(U_E, 0) && key(U_I, SHIFT));
    CHECK(!strcmp(search.text, "teI"));
    CHECK_EQ(search.nrows, 1);
    CHECK(key(U_ENTER, 0));
    CHECK(!strcmp(fdesk.ran, "teI"));
    return true;
}

static bool search_mouse_steps(void)
{
    /* "Jam OS": opens; a click on a row runs it; one outside closes */
    strip_click(strip_find(STRIP_JAM));
    CHECK(search.open);
    struct comp_box r = search_row_box(1);
    CHECK(!box_empty(r) && box_contains(search_box(), r.x1, r.y1));
    CHECK(press_btn((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2, BTN));
    release_at((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2);
    CHECK(!search.open);
    CHECK_EQ(fdesk.nlaunch, 2);
    strip_click(strip_find(STRIP_JAM));
    CHECK(!press_btn(10, 790, BTN));   /* outside: closed, and the press goes on */
    release_at(10, 790);
    CHECK(!search.open);
    /* its box: centred, near the top */
    super_tap();
    struct comp_box b = search_box();
    CHECK_EQ(b.x1 + b.x2, OUT_W);
    CHECK_EQ(b.y1, OUT_H * LOOK_SEARCH_TOP / 1000);
    CHECK(key(U_ESC, 0));
    CHECK(!search.open);
    return true;
}

bool t_desk_search(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = search_open_steps() && search_type_steps() && search_mouse_steps();
    fk_close_all();
    return ok;
}

/* ---- popovers ------------------------------------------------------------------------------- */

static bool placed_under(enum strip_part part, enum pop_kind kind)
{
    const struct strip_item *it = strip_find(part);
    CHECK(it);
    struct comp_box icon = it->box;
    strip_click(it);
    CHECK_EQ(pop.kind, kind);
    CHECK_EQ(pop.box.y1, LOOK_STRIP_H + LOOK_POP_GAP);
    CHECK_EQ(pop.box.x2, icon.x2);
    CHECK_EQ(pop.box.x2 - pop.box.x1, LOOK_POP_W);
    CHECK(strip_find(part)->on);
    return true;
}

static bool popover_steps(void)
{
    CHECK(placed_under(STRIP_VOL, POP_VOLUME));
    CHECK(placed_under(STRIP_NET, POP_NETWORK));   /* one at a time */
    CHECK(!strcmp(pop.net.address, "") && !pop.net.up);   /* nothing behind it yet */
    CHECK(placed_under(STRIP_CLOCK, POP_CLOCK));
    CHECK(pop.now.day == 5 && pop.now.month == 10);
    strip_click(strip_find(STRIP_CLOCK));   /* its own icon again: closed */
    CHECK_EQ(pop.kind, POP_NONE);
    CHECK(placed_under(STRIP_VOL, POP_VOLUME));
    CHECK(key(U_ESC, 0));
    CHECK_EQ(pop.kind, POP_NONE);
    /* the slider: a click at its middle, then a drag to its end */
    CHECK(placed_under(STRIP_VOL, POP_VOLUME));
    struct comp_box s = pop_slider_box();
    /* the percentage's slot: as wide as "100%" (every value fits), at the
     * popover's padding, and the knob at 100% (its radius past the
     * slider's end) LOOK_POP_PCT_GAP short of it */
    struct comp_box p = pop_pct_box();
    CHECK_EQ(p.x2, pop.box.x2 - LOOK_POP_PAD);
    for (unsigned v = 0; v <= 100; v++) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%u%%", v);
        CHECK(desk_text_w(desk_font.r12, pct) <= p.x2 - p.x1);
    }
    CHECK(desk_font.r12 != NULL);   /* measured with the real font */
    CHECK_EQ(p.x1 - (s.x2 + LOOK_POP_KNOB), LOOK_POP_PCT_GAP);
    CHECK(s.x2 - s.x1 >= 100);      /* the slider keeps room to drag */
    CHECK(press_btn((s.x1 + s.x2) / 2, (s.y1 + s.y2) / 2, BTN));
    CHECK(pop.volume >= 49 && pop.volume <= 50);
    move_to(s.x2 + 40, s.y1);
    CHECK_EQ(pop.volume, 100);
    release_at(s.x2 + 40, s.y1);
    CHECK(!pop.dragging);
    /* a click elsewhere: closed, and the press goes on */
    CHECK(!press_btn(10, 700, BTN));
    release_at(10, 700);
    CHECK_EQ(pop.kind, POP_NONE);
    return true;
}

/* The calendar of year y, month m: its first day's cell and the number of days. */
static bool month_is(int64_t y, unsigned m, unsigned first, unsigned days)
{
    struct civil c = { .year = y, .month = m, .day = 1 };
    uint8_t cells[42];
    unsigned n = pop_calendar(&c, cells);
    CHECK_EQ(n, first + days);
    CHECK_EQ(cells[first], 1);
    CHECK(first == 0 || cells[first - 1] == 0);
    CHECK_EQ(cells[n - 1], days);
    return true;
}

bool t_desk_popover(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = popover_steps();
    /* Monday first: October 2026 starts on a Thursday (cell 3), June 2026
     * on a Monday, February 2024 (leap) on a Thursday, 2100 isn't leap */
    ok = ok && month_is(2026, 10, 3, 31) && month_is(2026, 6, 0, 30) &&
         month_is(2024, 2, 3, 29) && month_is(2100, 2, 0, 28) && month_is(2026, 2, 6, 28);
    fk_close_all();
    return ok;
}

/* ---- notifications -------------------------------------------------------------------------- */

static uint32_t post(const char *title, unsigned nbuttons)
{
    struct notify_spec n = { .title = title, .body = "one line", .letter = title[0],
                             .buttons = { "Reboot", "Later" }, .nbuttons = nbuttons };
    return notify_post(&n);
}

static bool notify_steps(void)
{
    uint32_t a = post("First", 0), b = post("Second", 2);
    CHECK(a && b && a != b);
    CHECK_EQ(notes.n, 2);
    /* the newest on top, under the strip, at the right */
    CHECK_EQ(notes.cards[0].id, b);
    CHECK_EQ(notes.cards[0].box.y1, LOOK_NOTE_TOP);
    CHECK_EQ(notes.cards[0].box.x2, OUT_W - LOOK_NOTE_RIGHT);
    CHECK_EQ(notes.cards[1].box.y1, notes.cards[0].box.y2 + LOOK_NOTE_GAP);
    /* after its time the plain one goes; the one with buttons stays */
    notify_tick(now() + (LOOK_NOTE_SHOW_MS + 10) * NS_PER_MS);
    CHECK_EQ(notes.n, 1);
    CHECK_EQ(notes.cards[0].id, b);
    CHECK_EQ(notes.cards[0].box.y1, LOOK_NOTE_TOP);
    /* its second button: answered, gone */
    struct comp_box later = notify_button_box(&notes.cards[0], 1);
    CHECK(press_btn(later.x1 + 2, later.y1 + 2, BTN));
    release_at(later.x1 + 2, later.y1 + 2);
    CHECK_EQ(fdesk.nanswered, 1);
    CHECK(fdesk.answered_id == b && fdesk.answered_button == 1);
    CHECK_EQ(notes.n, 0);
    /* a click on a plain card sends it */
    a = post("Third", 0);
    struct comp_box c = notes.cards[0].box;
    CHECK(press_btn(c.x1 + 5, c.y1 + 5, BTN));
    release_at(c.x1 + 5, c.y1 + 5);
    CHECK_EQ(notes.n, 0);
    /* at most NOTIFY_MAX: the oldest plain one pushed out */
    uint32_t keep = post("Buttons", 1);
    uint32_t oldest = post("Plain 1", 0);
    for (unsigned i = 0; i < NOTIFY_MAX; i++)
        (void)post("More", 0);
    CHECK_EQ(notes.n, NOTIFY_MAX);
    bool has_keep = false, has_oldest = false;
    for (unsigned i = 0; i < notes.n; i++) {
        has_keep |= notes.cards[i].id == keep;
        has_oldest |= notes.cards[i].id == oldest;
    }
    CHECK(has_keep && !has_oldest);
    notify_withdraw(keep);
    for (unsigned i = 0; i < notes.n; i++)
        CHECK(notes.cards[i].id != keep);
    return true;
}

/* initctl.terminal refused (TERMINALS_MAX open): one notice while it is up. */
static bool terminals_full_steps(void)
{
    while (notes.n)
        notify_withdraw(notes.cards[0].id);   /* (animations off: gone at once) */
    uint32_t a = desk_terminals_full();
    CHECK(a);
    CHECK_EQ(notes.n, 1);
    CHECK_EQ(notes.cards[0].id, a);
    CHECK(!strcmp(notes.cards[0].title, "No more terminals"));
    CHECK(!strcmp(notes.cards[0].body, "16 is the most. Close one first."));
    /* the body's one line fits the card (popdraw.c: right of the tile) */
    int32_t room = LOOK_NOTE_W - 2 * LOOK_NOTE_PAD_X - LOOK_NOTE_TILE - 10;
    CHECK(desk_font.r12 && desk_text_w(desk_font.r12, notes.cards[0].body) <= room);
    CHECK(desk_text_w(desk_font.m13, notes.cards[0].title) <= room);
    CHECK(notes.cards[0].letter == 'T' && notes.cards[0].nbuttons == 0);
    /* asked again (Super+Enter held: its repeats): no second card */
    CHECK_EQ(desk_terminals_full(), 0);
    CHECK_EQ(desk_terminals_full(), 0);
    CHECK_EQ(notes.n, 1);
    /* gone after its 5 s: the next refusal shows it again */
    notify_tick(now() + (LOOK_NOTE_SHOW_MS + 10) * NS_PER_MS);
    CHECK_EQ(notes.n, 0);
    uint32_t b = desk_terminals_full();
    CHECK(b && b != a);
    CHECK_EQ(notes.n, 1);
    /* a click sends it sooner: again the next refusal shows it */
    struct comp_box c = notes.cards[0].box;
    CHECK(press_btn(c.x1 + 5, c.y1 + 5, BTN));
    release_at(c.x1 + 5, c.y1 + 5);
    CHECK_EQ(notes.n, 0);
    CHECK(desk_terminals_full() != 0);
    return true;
}

bool t_desk_notify(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = notify_steps() && terminals_full_steps();
    fk_close_all();
    /* without the desktop: no card (the log says so) */
    wm_test_start(COMP_FLOATING);
    ok = ok && desk_terminals_full() == 0;
    fk_close_all();
    return ok;
}

/* ---- notices held during the boot splash ----------------------------------------------- */

static const struct notify_card *card(uint32_t id)
{
    for (unsigned i = 0; i < notes.n; i++)
        if (notes.cards[i].id == id)
            return &notes.cards[i];
    return NULL;
}

/* The splash waited for (paint.c's comp.splash_until), then its overlay up
 * and gone with no fade: held all along, shown after, the 5 s from then. */
static bool held_steps(void)
{
    /* no splash: at once */
    uint32_t now_one = post("Now", 0);
    CHECK(card(now_one) && notes.nheld == 0);
    notify_withdraw(now_one);
    CHECK_EQ(notes.n, 0);
    /* the wait for the splash: held, none shown, the loop not woken */
    comp.splash_until = now() + 5 * NS_PER_S;
    uint32_t a = post("Connected", 0), b = post("Update written", 2), gone = post("Gone", 0);
    CHECK(a && b && gone && a != b && b != gone);
    CHECK_EQ(notes.n, 0);
    CHECK_EQ(notes.nheld, 3);
    CHECK(box_empty(notify_box()));
    CHECK(notify_live(a) && notify_live(b) && notify_live(gone));
    CHECK_EQ(notify_deadline(), DEADLINE_NEVER);
    notify_tick(now() + 6 * NS_PER_S);   /* long past a card's 5 s: still held */
    CHECK_EQ(notes.n, 0);
    /* withdrawn while held: dropped unseen */
    notify_withdraw(gone);
    CHECK(!notify_live(gone));
    CHECK_EQ(notes.nheld, 2);
    /* the splash's overlay up: still held */
    comp.splash_until = 0;
    CHECK(fk_open_full(&fks[1], &fake_keyless));
    CHECK(screens_overlay());
    uint32_t c = post("USB stick added", 0);
    notify_tick(now() + 7 * NS_PER_S);
    CHECK_EQ(notes.n, 0);
    CHECK_EQ(notes.nheld, 3);
    CHECK_EQ(notify_deadline(), DEADLINE_NEVER);
    /* it goes (no animations: no fade): the loop is woken, all shown in
     * order (the newest on top), each from then */
    wm_destroy(fks[1].ww);
    fks[1].ww = NULL;
    CHECK(!screens_overlay());
    CHECK_EQ(notify_deadline(), 0);
    uint64_t t1 = now() + 8 * NS_PER_S;
    notify_tick(t1);
    CHECK_EQ(notes.nheld, 0);
    CHECK_EQ(notes.n, 3);
    CHECK(notes.cards[0].id == c && notes.cards[1].id == b && notes.cards[2].id == a);
    CHECK(!card(gone));
    for (unsigned i = 0; i < notes.n; i++)
        CHECK_EQ(notes.cards[i].posted, t1);
    CHECK_EQ(notes.cards[0].box.y1, LOOK_NOTE_TOP);
    /* the 5 s from the reveal, not from the post */
    notify_tick(t1 + (LOOK_NOTE_SHOW_MS - 10) * NS_PER_MS);
    CHECK(card(a) && card(c) && !card(a)->leaving);
    notify_tick(t1 + (LOOK_NOTE_SHOW_MS + 10) * NS_PER_MS);
    CHECK(!card(a) && !card(c));
    CHECK(card(b));                      /* buttons: until pressed */
    struct comp_box later = notify_button_box(card(b), 1);
    CHECK(press_btn(later.x1 + 2, later.y1 + 2, BTN));
    release_at(later.x1 + 2, later.y1 + 2);
    CHECK(fdesk.answered_id == b && fdesk.answered_button == 1);
    CHECK_EQ(notes.n, 0);
    return true;
}

/* More than NOTIFY_MAX held: the oldest plain one dropped (one with buttons
 * kept); the overlay's fade-out holds them too. */
static bool held_cap_steps(void)
{
    comp.splash_until = now() + 5 * NS_PER_S;
    uint32_t keep = post("Buttons", 1), oldest = post("Plain 1", 0), last = 0;
    for (unsigned i = 0; i < NOTIFY_MAX; i++)
        last = post("More", 0);
    CHECK_EQ(notes.nheld, NOTIFY_MAX);
    CHECK(notify_live(keep) && !notify_live(oldest) && notify_live(last));
    CHECK_EQ(notes.held[0].id, keep);
    comp.splash_until = 0;
    /* the overlay fades out: held until the fade ends */
    anim_init(true);
    CHECK(fk_open_full(&fks[1], &fake_keyless));
    static struct comp_buffer shown;   /* a buffer to take its picture from */
    fks[1].s.buffer = &shown;
    wm_destroy(fks[1].ww);
    fks[1].s.buffer = NULL;
    fks[1].ww = NULL;
    CHECK_EQ(anim_running(), ANIM_FADE);
    notify_tick(now() + 100 * NS_PER_MS);
    CHECK_EQ(notes.n, 0);
    uint64_t t = now() + (LOOK_ANIM_FADE_MS + 50) * NS_PER_MS;
    anim_tick(t);   /* as desk_tick: the animation, then the notices */
    CHECK_EQ(anim_running(), ANIM_NONE);
    notify_tick(t);
    CHECK_EQ(notes.nheld, 0);
    CHECK_EQ(notes.n, NOTIFY_MAX);
    CHECK_EQ(notes.cards[0].id, last);
    CHECK_EQ(notes.cards[NOTIFY_MAX - 1].id, keep);
    CHECK(notes.cards[0].posted == t && notes.cards[0].alpha == 0);   /* slides in from now */
    anim_init(false);
    return true;
}

bool t_desk_notify_held(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = held_steps() && held_cap_steps();
    comp.splash_until = 0;
    anim_init(false);
    fk_close_all();
    return ok;
}

/* ---- a boot overlay ------------------------------------------------------------------------ */

static bool mapped(unsigned k)
{
    return fks[k].ww && fks[k].ww->win && (fks[k].ww->win->flags & COMP_WIN_MAPPED);
}


static bool covers_output(const struct comp_window *w)
{
    struct comp_box f = window_frame(w);
    return f.x1 == 0 && f.y1 == 0 && f.x2 == OUT_W && f.y2 == OUT_H;
}

static unsigned strip_count(enum strip_part part)
{
    strip_update();
    unsigned n = 0;
    for (unsigned i = 0; i < strip.n; i++)
        n += strip.items[i].part == part;
    return n;
}

static bool overlay_steps(void)
{
    CHECK(fk_open(&fks[0], 320, 200, false));   /* the first terminal */
    seat_focus(win(0));
    CHECK(fk_open_full(&fks[1], &fake_keyless));   /* the splash */
    struct wm_window *ov = fks[1].ww;
    CHECK(ov->overlay && !ov->screen && mapped(1));
    CHECK(win(1)->flags & COMP_WIN_OVERLAY);
    CHECK_EQ(screens_count(), 1);               /* no screen of its own */
    CHECK_EQ(screens_windows(screens_cur()), 1);   /* on none: no chip */
    CHECK(covers_output(win(1)));               /* the strip's rows too */
    CHECK(screens_overlay());
    CHECK_EQ(strip_count(STRIP_DOT), 0);        /* hidden under it */
    CHECK(!strip.shown && box_empty(strip_box()));
    CHECK_EQ(scene.top, win(1));
    /* the terminal focused again, a window opened after it: under it */
    seat_focus(NULL);
    seat_focus(win(0));
    CHECK_EQ(scene.top, win(1));
    CHECK(fk_open(&fks[2], 300, 180, false));
    CHECK_EQ(scene.top, win(1));
    CHECK_EQ(screens_count(), 1);
    CHECK_EQ(screens_windows(screens_cur()), 2);
    /* never cycled to, no Alt+Tab row */
    CHECK_EQ(wm_cycle(win(0), false), win(2));
    CHECK_EQ(wm_cycle(win(2), false), win(0));
    alttab_step(false);
    CHECK_EQ(alttab.n, 2);
    for (unsigned i = 0; i < alttab.n; i++)
        CHECK(alttab.rows[i].ww != ov);
    alttab_end(false);
    /* not minimised; screens change under it, it stays (no slide) */
    screens_minimise(ov);
    CHECK(!ov->minimised && mapped(1));
    screens_step(1);
    CHECK_EQ(screens_count(), 2);
    CHECK(mapped(1) && !mapped(0) && win(1)->slide_x == 0 && covers_output(win(1)));
    screens_go(0);
    CHECK_EQ(screens_count(), 1);
    /* tiling: the two windows share the room; it isn't one of them */
    wm_toggle_layout();
    struct comp_box room = screens_room(screens_cur());
    int32_t half = (room.x2 - room.x1 - 3 * WM_GAP + 1) / 2;   /* the left of two */
    CHECK(box_eq(win(0)->tile, (struct comp_box){ room.x1 + WM_GAP, room.y1 + WM_GAP,
                                                  room.x1 + WM_GAP + half, room.y2 - WM_GAP }));
    CHECK(covers_output(win(1)));
    wm_toggle_layout();
    /* a client with keys: full screen on a screen of its own, as before */
    wm_toggle_fullscreen(win(0));
    CHECK_EQ(screens_count(), 2);
    CHECK_EQ(screens_cur()->kind, SCREEN_FULL);
    CHECK_EQ(scene.top, win(1));
    wm_toggle_fullscreen(win(0));
    CHECK_EQ(screens_count(), 1);
    CHECK(fk_draw(&fks[0]));
    return true;
}

static bool overlay_fade_steps(void)
{
    anim_init(true);
    static struct comp_buffer shown;   /* a buffer to take its picture from */
    fks[1].s.buffer = &shown;
    unsigned snaps = fdesk.snaps;
    wm_destroy(fks[1].ww);
    fks[1].s.buffer = NULL;
    fks[1].ww = NULL;
    struct anim_draw d;
    CHECK_EQ(anim_running(), ANIM_FADE);
    CHECK(anim_now(&d) && d.above_strip && d.alpha == 255);
    struct comp_box out = { 0, 0, OUT_W, OUT_H };
    CHECK(box_eq(d.at, out));
    CHECK_EQ(fdesk.snaps, snaps + 1);
    CHECK(!screens_overlay());
    CHECK_EQ(strip_count(STRIP_DOT), 1);        /* the strip back, under the fade */
    CHECK_EQ(strip_count(STRIP_CHIP), 2);
    damage_clear(&scene.damage);
    anim_tick(now() + 100 * NS_PER_MS);
    CHECK(anim_now(&d) && d.alpha > 0 && d.alpha < 255 && box_eq(d.at, out));
    CHECK(scene.damage.n > 0);
    for (uint32_t i = 0; i < scene.damage.n; i++)
        CHECK(box_eq(scene.damage.b[i], out));   /* its box: nothing else */
    anim_tick(now() + 300 * NS_PER_MS);
    CHECK_EQ(anim_running(), ANIM_NONE);
    CHECK(!anim_now(&d));
    CHECK_EQ(fdesk.snaps, fdesk.snaps_freed);
    CHECK_EQ(screens_count(), 1);
    CHECK(mapped(0) && mapped(2));
    anim_init(false);
    /* one that leaves full screen: a window on the current screen */
    CHECK(fk_open_full(&fks[1], &fake_keyless));
    CHECK(fks[1].ww->overlay);
    wm_request_fullscreen(fks[1].ww, false);
    CHECK(!fks[1].ww->overlay && fks[1].ww->screen == screens_cur());
    CHECK(!(win(1)->flags & COMP_WIN_OVERLAY));
    CHECK(fk_draw(&fks[1]));
    CHECK_EQ(strip_count(STRIP_CHIP), 3);
    return true;
}

bool t_desk_overlay(void)
{
    desk_test_start(COMP_FLOATING);
    bool ok = overlay_steps() && overlay_fade_steps();
    anim_init(false);
    fk_close_all();
    return ok;
}
