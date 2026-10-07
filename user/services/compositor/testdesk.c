/* The test scene's desktop commands (testscene.c, desk.h): the top bar,
 * toplevels the window manager places, a popover, a notification and the
 * search box, with no client behind any of them, for tools/comp-test.sh's
 * screenshots and utest's pictures. The desktop starts off in the test
 * scene (its windows are placed by hand); `desktop` turns it on, with no
 * animations, so every picture is an end state.
 *   desktop          the desktop on (the strip; the frosting made now)
 *   time=Y,M,D,H,MIN the clock fixed at that local time
 *   top=W,H,AARRGGBB,TITLE[,FLAGS]
 *                    a toplevel W x H of testscene.h's pixels that can't be
 *                    resized, placed by the window manager on the current
 *                    screen; FLAGS: f focused, m minimised, F full screen
 *                    from its first buffer (its client, the scene's, takes
 *                    no keys: a boot overlay, as the splash's)
 *   close=K          toplevel K (1, 2, ... in order) unmapped, as its client
 *                    would (a boot overlay fades out)
 *   animate          the animations on (`tick` moves their clock)
 *   tick=MS          the animations' and the notices' clock MS milliseconds
 *                    on from now (notices held during a splash show then)
 *   popover=volume|network|clock   that popover, opened from its icon
 *   notify=TITLE,BODY[,BUTTON[,BUTTON]]   a notification (held while a
 *                    splash is up, as notify.c holds it)
 *   search[=TEXT]    the search box, TEXT typed into it
 *   cursorshape=NAME the pointer shows that one of the set (arrow, ew, ns,
 *                    nwse, nesw, move, text, hand, busy)
 * After each paint with the desktop on, its layout goes to the log (`desk:`
 * lines: the strip's islands and items, the cards' boxes) for the checker,
 * which can't measure text. */
#include <fun.h>
#include <jwl/wayland.h>
#include "desk.h"
#include "testscene.h"

#define TOPS_MAX 6

static struct top {
    struct comp_surface s;
    struct comp_buffer b;
    struct comp_pool p;
    struct wm_window *ww;
} tops[TOPS_MAX];
static unsigned ntops;
static struct comp_client client;   /* no connection: nothing is ever sent to it */

static void ignore_configure(void *ctx, const struct wm_config *cfg)
{
    (void)ctx;
    (void)cfg;
}

static void ignore_close(void *ctx)
{
    (void)ctx;
}

static const struct wm_ops top_ops = { ignore_configure, ignore_close };

/* Up to n comma-separated fields of s into f (each cut at its comma, in
 * buf); how many. */
static unsigned fields(const char *s, char *buf, size_t cap, const char **f, unsigned n)
{
    snprintf(buf, cap, "%s", s);
    unsigned k = 0;
    for (char *p = buf; k < n && p;) {
        f[k++] = p;
        p = strchr(p, ',');
        if (p)
            *p++ = '\0';
    }
    return k;
}

static uint32_t number(const char *s, int base)
{
    uint32_t v = 0;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f'
                                                        ? (*s | 0x20) - 'a' + 10 : 99;
        if (d >= base)
            return 0;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    return v;
}

/* top=W,H,AARRGGBB,TITLE[,FLAGS] */
static bool cmd_top(const char *arg)
{
    char buf[128];
    const char *f[5];
    unsigned n = fields(arg, buf, sizeof(buf), f, 5);
    uint32_t w = n >= 4 ? number(f[0], 10) : 0, h = n >= 4 ? number(f[1], 10) : 0;
    if (ntops == TOPS_MAX || !w || !h || w > 2048 || h > 2048)
        return false;
    uint32_t argb = number(f[2], 16), *px = big_alloc((uint64_t)w * h * 4);
    if (!px)
        return false;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[y * w + x] = testscene_rgb(argb, (int32_t)x, (int32_t)y, false);
    struct top *t = &tops[ntops++];
    t->p = (struct comp_pool){ .addr = (uint64_t)(uintptr_t)px, .refs = 1 };
    t->b = (struct comp_buffer){ .pool = &t->p, .refs = 1, .busy = 1, .width = (int32_t)w,
                                 .height = (int32_t)h, .stride = w * 4,
                                 .format = JWL_WL_SHM_FORMAT_XRGB8888 };
    t->s = (struct comp_surface){ .client = &client, .buffer = &t->b, .width = (int32_t)w,
                                  .height = (int32_t)h, .input_all = true };
    if (!(t->ww = wm_create(&t->s, &top_ops, t)))
        return false;
    wm_set_title(t->ww, f[3]);
    wm_set_limits(t->ww, (int32_t)w, (int32_t)h, (int32_t)w, (int32_t)h);
    bool full = n == 5 && strchr(f[4], 'F');
    if (full)
        wm_request_fullscreen(t->ww, true);
    if (wm_commit(t->ww, full ? WM_ST_FULLSCREEN : 0) != OK)
        return false;
    for (const char *fl = n == 5 ? f[4] : ""; *fl; fl++) {
        if (*fl == 'f') {   /* as the seat marks it (it gives none to a client without a
                             * connection, so it leaves these to us) */
            t->ww->win->flags |= COMP_WIN_FOCUSED;
            wm_focus_changed(t->ww->win);
        }
        if (*fl == 'm')
            screens_minimise(t->ww);
    }
    return true;
}

/* close=K */
static bool cmd_close(const char *arg)
{
    uint32_t k = number(arg, 10);
    if (!k || k > ntops || !tops[k - 1].ww->win)
        return false;
    wm_unmap(tops[k - 1].ww);
    return true;
}

/* tick=MS */
static bool cmd_tick(const char *arg)
{
    uint64_t t = now() + (uint64_t)number(arg, 10) * NS_PER_MS;
    anim_tick(t);
    notify_tick(t);   /* held notices shown once the splash is over */
    return true;
}

/* time=Y,M,D,H,MIN */
static bool cmd_time(const char *arg)
{
    char buf[64];
    const char *f[5];
    if (fields(arg, buf, sizeof(buf), f, 5) != 5)
        return false;
    struct civil c = { .year = number(f[0], 10), .month = number(f[1], 10),
                       .day = number(f[2], 10), .hour = number(f[3], 10),
                       .minute = number(f[4], 10) };
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 || c.hour > 23 || c.minute > 59)
        return false;
    /* 1970-01-01 was a Thursday (4) */
    c.wday = (unsigned)(((civil_days(c.year, c.month, c.day) % 7) + 11) % 7);
    desk_time_fix(&c);
    return true;
}

/* notify=TITLE,BODY[,BUTTON[,BUTTON]] */
static bool cmd_notify(const char *arg)
{
    char buf[160];
    const char *f[4];
    unsigned n = fields(arg, buf, sizeof(buf), f, 4);
    if (n < 2)
        return false;
    struct notify_spec spec = { .title = f[0], .body = f[1], .letter = f[0][0],
                                .colour = LOOK_JAM_APRICOT, .nbuttons = n - 2 };
    for (unsigned i = 2; i < n; i++)
        spec.buttons[i - 2] = f[i];
    return notify_post(&spec) != 0;
}

static bool cmd_popover(const char *arg)
{
    static const struct { const char *name; enum strip_part part; enum pop_kind kind; } k[] = {
        { "volume", STRIP_VOL, POP_VOLUME },
        { "network", STRIP_NET, POP_NETWORK },
        { "clock", STRIP_CLOCK, POP_CLOCK },
    };
    for (unsigned i = 0; i < 3; i++)
        if (!strcmp(arg, k[i].name)) {
            const struct strip_item *it = strip_find(k[i].part);
            if (!it)
                return false;
            pop_toggle(k[i].kind, it->box);
            return true;
        }
    return false;
}

/* cursorshape=NAME: the pointer shows one of the set (as the seat would
 * pick it): arrow, ew, ns, nwse, nesw, move, text, hand, busy. */
static bool cmd_cursorshape(const char *arg)
{
    static const char *const names[CURSOR_SHAPES] = { "arrow", "ew", "ns", "nwse", "nesw",
                                                      "move", "text", "hand", "busy" };
    for (unsigned s = 0; s < CURSOR_SHAPES; s++)
        if (!strcmp(arg, names[s])) {
            cursor.shape = (enum cursor_shape)s;
            cursor_moved(cursor.x, cursor.y);
            return true;
        }
    return false;
}

static void desktop_on(void)
{
    desk_init(true, false);
    if (!frost_strip_row(0) && frost_init(scene.width, scene.height) != OK)
        printf("compositor: testscene: no memory for the frosting\n");
    strip_dirty();
    scene_damage((struct comp_box){ 0, 0, scene.width, scene.height });
}

/* The search box opened with text typed (as its keys would). */
static void search_with(const char *text)
{
    if (!search.open)
        search_toggle();
    snprintf(search.text, sizeof(search.text), "%s", text);
    search_key(0, 0);   /* no key: the rows filtered for the text */
}

/* nodesktop: the toplevels and cards gone, the desktop off (the bench's
 * frames are the windows' alone). */
static void desktop_off(void)
{
    for (unsigned i = 0; i < ntops; i++)
        wm_destroy(tops[i].ww);
    ntops = 0;
    pop_close();
    search_close();
    while (notes.nheld)
        notify_withdraw(notes.held[0].id);
    while (notes.n)
        notify_withdraw(notes.cards[0].id);   /* no animations: at once */
    desk_init(false, false);
    cursor.shape = CURSOR_ARROW;
    cursor_moved(cursor.x, cursor.y);
    strip_dirty();
    scene_damage((struct comp_box){ 0, 0, scene.width, scene.height });
}

/* deskbench: the desktop's own frames, timed: the whole strip drawn again,
 * and the search box's backdrop made again (as when a window behind it
 * changes) and the box drawn on it. */
static void damage_strip(void)
{
    scene_damage_over(strip_box());
}

static void damage_behind_search(void)
{
    scene_damage(search_box());
}

static struct comp_box anim_box;   /* a minimising window's picture, at full size */

static void damage_anim(void)
{
    scene_damage_over(anim_box);
}

static void desk_bench(void)
{
    if (!desk_on())
        return;
    testscene_bench("the strip", damage_strip);
    if (!search.open)
        search_toggle();
    testscene_bench("the search box, blurred again", damage_behind_search);
    search_close();
    if (!ntops || !tops[0].ww->win)
        return;
    /* the first frame of a minimise: the window's snapshot at full size,
     * sampled and laid over the strip's layer, every pixel */
    anim_init(true);
    anim_minimise(tops[0].ww->win, strip_chip_box(tops[0].ww));
    anim_box = window_frame(tops[0].ww->win);
    testscene_bench("a minimising window's picture", damage_anim);
    anim_init(false);
}

int testdesk_command(const char *c)
{
    const char *eq = strchr(c, '=');
    size_t k = eq ? (size_t)(eq - c) : strlen(c);
    const char *arg = eq ? eq + 1 : "";
    if (!strcmp(c, "desktop")) {
        desktop_on();
        return 1;
    }
    if (!strcmp(c, "nodesktop")) {
        desktop_off();
        return 1;
    }
    if (!strcmp(c, "animate")) {
        anim_init(true);
        return 1;
    }
    if (!strcmp(c, "deskbench")) {
        desk_bench();
        return 1;
    }
    if (!strncmp(c, "search", k) && k == 6) {
        search_with(arg);
        return 1;
    }
    static const struct { const char *name; bool (*fn)(const char *); } cmds[] = {
        { "top", cmd_top }, { "time", cmd_time }, { "notify", cmd_notify },
        { "popover", cmd_popover }, { "cursorshape", cmd_cursorshape },
        { "close", cmd_close },     { "tick", cmd_tick },
    };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if (eq && k == strlen(cmds[i].name) && !strncmp(c, cmds[i].name, k))
            return cmds[i].fn(arg) ? 1 : 0;
    return -1;
}

static void box_line(const char *what, unsigned i, struct comp_box b, bool on)
{
    if (!box_empty(b))
        printf("compositor: testscene: desk: %s %u %d %d %d %d %d\n", what, i, b.x1, b.y1, b.x2,
               b.y2, on);
}

void testdesk_report(void)
{
    static const char *const parts[] = { "none", "jam", "dot", "plus", "chip", "nochip", "mode",
                                         "net", "vol", "clock" };
    if (!desk_on())
        return;
    for (unsigned i = 0; i < 3; i++)
        box_line("island", i, strip.islands[i], false);
    for (unsigned i = 0; i < strip.n; i++)
        box_line(parts[strip.items[i].part], i, strip.items[i].box, strip.items[i].on);
    box_line("popover", (unsigned)pop.kind, pop.box, false);
    box_line("search", 0, search_box(), false);
    for (unsigned i = 0; i < notes.n; i++)
        box_line("note", i, notes.cards[i].box, notes.cards[i].nbuttons > 0);
    for (unsigned i = 0; i < ntops; i++)
        if (tops[i].ww->win)
            box_line("top", i, window_frame(tops[i].ww->win), tops[i].ww->minimised);
    box_line("cursor", (unsigned)cursor.shape, cursor_box(), false);
}
