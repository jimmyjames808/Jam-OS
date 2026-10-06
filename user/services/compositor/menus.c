/* The search box and Alt+Tab's list (desk.h; the owner's picks in
 * docs/G1-PLAN.md "The look"). Both are frosted cards (frost.c) drawn by
 * menudraw.c from the boxes here; at most one of them is open.
 *
 * The search box (the app menu): opened by tapping Super alone or clicking
 * "Jam OS", centred near the top. Empty, it lists every app, the most
 * recently run first (then the table's order), so a mouse user can just
 * click one; typing filters (names holding the text, those starting with
 * it first) and adds a last row, "Run “...” in a terminal". Up and Down
 * move the selection, Enter runs the selected row, Esc closes, Backspace
 * takes the last character back. Running goes through the plumbing's
 * hooks (desk_launch, desk_run). Every key is the box's while it is open:
 * no client sees one.
 *
 * Alt+Tab: one row per window (its letter tile and title): the current
 * screen's windows, most recently focused first; then each other screen's,
 * grouped under "Screen N" ("Full screen" for a full-screen one), in the
 * screens' order; then the minimised ones. The first Tab selects the
 * second row (the window before), each Tab the next, Shift+Tab the one
 * before, wrapping; letting go of Alt goes to the selected window (sliding
 * to its screen, restoring it), Esc cancels. The list shows only once Alt
 * has been held LOOK_ALTTAB_SHOW_MS (desk.c), so a quick Alt+Tab just
 * switches. */
#include <keymap.h>
#include "desk.h"

const struct desk_app desk_apps[DESK_APPS] = {
    { "Terminal", "Shell", 'T', LOOK_JAM_RASPBERRY },
    { "Jamjar", "Music player", 'J', LOOK_JAM_APRICOT },
};

struct search_state search;
struct alttab_state alttab;
static uint64_t runs;   /* apps run from the box: each app's `used` stamp */

/* HID usages (USB HID Usage Tables 1.4, section 10). */
#define U_ENTER     0x28
#define U_ESC       0x29
#define U_BACKSPACE 0x2a
#define U_DOWN      0x51
#define U_UP        0x52
#define U_KP_ENTER  0x58

/* ---- the search box: its rows ------------------------------------------------------------ */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
}

/* Does name hold text (case aside), and does it start with it? */
static bool holds(const char *name, const char *text, bool *starts)
{
    size_t n = strlen(text);
    for (size_t at = 0; name[at]; at++) {
        size_t i = 0;
        while (i < n && name[at + i] && lower(name[at + i]) == lower(text[i]))
            i++;
        if (i == n) {
            *starts = at == 0;
            return true;
        }
    }
    *starts = n == 0;
    return n == 0;
}

/* The rows for what is typed, the selection kept on the first if it went. */
static void filter(void)
{
    bool typed = search.text[0] != '\0';
    unsigned order[DESK_APPS], n = 0;
    for (unsigned i = 0; i < DESK_APPS; i++)
        order[n++] = i;
    for (unsigned i = 1; i < n; i++)   /* most recently run first, else the table's order */
        for (unsigned j = i; j > 0 && search.used[order[j]] > search.used[order[j - 1]]; j--) {
            unsigned t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    search.nrows = 0;
    for (int pass = 0; pass < 2; pass++)   /* starting with the text, then holding it */
        for (unsigned i = 0; i < n; i++) {
            bool starts;
            if (holds(desk_apps[order[i]].name, search.text, &starts) && starts == (pass == 0))
                search.row[search.nrows++] = order[i];
        }
    if (typed)
        search.row[search.nrows++] = DESK_APPS;   /* "Run ... in a terminal" */
    if (search.sel >= search.nrows)
        search.sel = search.nrows ? search.nrows - 1 : 0;
}

static void damage(void)
{
    desk_damage(search_box());
}

void search_toggle(void)
{
    if (search.open) {
        search_close();
        return;
    }
    alttab_end(false);
    search.open = true;
    search.text[0] = '\0';
    search.sel = 0;
    filter();
    damage();
    strip_dirty();   /* "Jam OS" lit */
}

void search_close(void)
{
    if (!search.open)
        return;
    damage();
    search.open = false;
    strip_dirty();
}

/* Row i run: an app, or the text as a command in a terminal. */
static void run(unsigned i)
{
    if (i >= search.nrows)
        return;
    unsigned app = search.row[i];
    char cmd[SEARCH_TEXT_MAX];
    snprintf(cmd, sizeof(cmd), "%s", search.text);
    search_close();
    if (app == DESK_APPS) {
        desk_run(cmd);
        return;
    }
    char name[16];
    unsigned k = 0;
    for (const char *s = desk_apps[app].name; *s && k + 1 < sizeof(name); s++)
        name[k++] = lower(*s);
    name[k] = '\0';
    search.used[app] = ++runs;
    desk_launch(name);
}

/* cp typed: its UTF-8 bytes added, if they fit. */
static void type(uint32_t cp)
{
    char u[4];
    size_t n = 0, len = strlen(search.text);
    if (cp < 0x80) {
        u[n++] = (char)cp;
    } else if (cp < 0x800) {
        u[n++] = (char)(0xc0 | cp >> 6);
        u[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        return;   /* the keymap types nothing wider */
    }
    if (len + n >= SEARCH_TEXT_MAX)
        return;
    memcpy(search.text + len, u, n);
    search.text[len + n] = '\0';
    search.sel = 0;
}

/* The last character typed taken back (a whole UTF-8 sequence). */
static void untype(void)
{
    size_t len = strlen(search.text);
    while (len > 0 && (search.text[--len] & 0xc0) == 0x80)
        ;
    search.text[len] = '\0';
    search.sel = 0;
}

void search_key(uint16_t usage, uint32_t xkb_mods)
{
    if (usage == U_ESC) {
        search_close();
        return;
    }
    if (usage == U_ENTER || usage == U_KP_ENTER) {
        run(search.sel);
        return;
    }
    damage();   /* its size may change: the old box */
    if (usage == U_DOWN && search.sel + 1 < search.nrows)
        search.sel++;
    else if (usage == U_UP && search.sel > 0)
        search.sel--;
    else if (usage == U_BACKSPACE)
        untype();
    else if (!(xkb_mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT | KEYMAP_MOD_SUPER))) {
        struct keymap_sym k;
        if (keymap_decode(&keymap_us, keymap_evdev_of_hid(usage), xkb_mods, &k) && k.cp >= 0x20 &&
            k.cp != 0x7f)
            type(k.cp);
    }
    filter();
    damage();
}

/* ---- the search box: its boxes ------------------------------------------------------------ */

#define SEARCH_LIST_Y (LOOK_SEARCH_PAD + LOOK_SEARCH_IN_H + 9)   /* the list's top in the box */
#define RUN_GAP       9                                          /* the divider over "Run ..." */

static int32_t list_h(void)
{
    int32_t h = (int32_t)search.nrows * LOOK_ROW_H;
    if (search.nrows > 1 && search.row[search.nrows - 1] == DESK_APPS)
        h += RUN_GAP;
    return h;
}

struct comp_box search_box(void)
{
    if (!search.open)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t w = scene.width - 32 < LOOK_SEARCH_W ? scene.width - 32 : LOOK_SEARCH_W;
    int32_t x = (scene.width - w) / 2, y = scene.height * LOOK_SEARCH_TOP / 1000;
    int32_t h = SEARCH_LIST_Y + list_h() + LOOK_SEARCH_PAD;
    return (struct comp_box){ x, y, x + w, y + h };
}

struct comp_box search_row_box(unsigned i)
{
    struct comp_box b = search_box();
    if (box_empty(b) || i >= search.nrows)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t y = b.y1 + SEARCH_LIST_Y + (int32_t)i * LOOK_ROW_H;
    if (search.row[i] == DESK_APPS && i > 0)
        y += RUN_GAP;
    return (struct comp_box){ b.x1 + LOOK_SEARCH_PAD, y, b.x2 - LOOK_SEARCH_PAD, y + LOOK_ROW_H };
}

bool search_press(int32_t x, int32_t y)
{
    if (!box_contains(search_box(), x, y))
        return false;
    for (unsigned i = 0; i < search.nrows; i++)
        if (box_contains(search_row_box(i), x, y)) {
            search.sel = i;
            run(i);
            break;
        }
    return true;
}

/* ---- Alt+Tab ----------------------------------------------------------------------------- */

/* The windows of group g into the rows, most recently focused first. */
static void add_group(uint32_t g, const struct desk_screen *s, bool minimised)
{
    unsigned start = alttab.n;
    for (struct wm_window *ww = wm_first(); ww && alttab.n < ALTTAB_MAX; ww = ww->next) {
        if (!ww->win || ww->minimised != minimised || (!minimised && ww->screen != s))
            continue;
        unsigned j = alttab.n++;
        while (j > start && alttab.rows[j - 1].ww->focused_at < ww->focused_at) {
            alttab.rows[j] = alttab.rows[j - 1];
            j--;
        }
        alttab.rows[j] = (struct alttab_row){ .ww = ww, .group = g };
    }
    if (alttab.n > start && start > 0)
        alttab.rows[start].first = true;
}

static void make_list(void)
{
    alttab.n = 0;
    add_group(0, screens_cur(), false);
    for (unsigned i = 0; i < screens_count(); i++)
        if (i != screens_cur_index())
            add_group(i + 1, screens_nth(i), false);
    add_group(DESK_SCREENS_MAX + 1, NULL, true);
}

void alttab_step(bool back)
{
    if (!alttab.active) {
        make_list();
        if (!alttab.n)
            return;
        alttab.active = true;
        alttab.shown = false;
        alttab.since = now();
        alttab.top = 0;
        struct wm_window *f = wm_focused();
        alttab.sel = back ? alttab.n - 1 : alttab.rows[0].ww == f && alttab.n > 1 ? 1 : 0;
        return;
    }
    if (alttab.shown)
        desk_damage(alttab_box());
    alttab.sel = (alttab.sel + (back ? alttab.n - 1 : 1)) % alttab.n;
}

void alttab_end(bool go)
{
    if (!alttab.active)
        return;
    struct wm_window *ww = alttab.rows[alttab.sel].ww;
    if (alttab.shown)
        desk_damage(alttab_box());
    alttab.active = alttab.shown = false;
    alttab.n = 0;
    if (go && ww && ww->win)
        screens_activate(ww);
}

/* Row i's top within the list (its group's divider and label before it). */
static int32_t row_y(unsigned i)
{
    int32_t y = 0;
    for (unsigned k = 0; k <= i && k < alttab.n; k++) {
        if (alttab.rows[k].first)
            y += alttab.rows[k].group == DESK_SCREENS_MAX + 1 ? 11 : 11 + LOOK_GROUP_H;
        if (k < i)
            y += LOOK_ROW_H;
    }
    return y;
}

struct comp_box alttab_box(void)
{
    if (!alttab.active || !alttab.n)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t h = row_y(alttab.n - 1) + LOOK_ROW_H, room = scene.height - 2 * LOOK_STRIP_H;
    if (h > room)
        h = room;   /* the list scrolls inside it */
    h += 2 * LOOK_ALTTAB_PAD;
    int32_t w = LOOK_ALTTAB_W, x = (scene.width - w) / 2, y = (scene.height - h) / 2;
    return (struct comp_box){ x, y, x + w, y + h };
}

/* The list's scroll: the selected row inside the box. */
static int32_t scroll(struct comp_box b)
{
    int32_t room = b.y2 - b.y1 - 2 * LOOK_ALTTAB_PAD, y = row_y(alttab.sel);
    int32_t all = row_y(alttab.n - 1) + LOOK_ROW_H;
    if (all <= room || y + LOOK_ROW_H <= room)
        return 0;
    int32_t s = y + LOOK_ROW_H - room;
    return s > all - room ? all - room : s;
}

struct comp_box alttab_row_box(unsigned i)
{
    struct comp_box b = alttab_box();
    if (box_empty(b) || i >= alttab.n)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t y = b.y1 + LOOK_ALTTAB_PAD + row_y(i) - scroll(b);
    struct comp_box r = { b.x1 + LOOK_ALTTAB_PAD, y, b.x2 - LOOK_ALTTAB_PAD, y + LOOK_ROW_H };
    if (r.y1 < b.y1 + LOOK_ALTTAB_PAD || r.y2 > b.y2 - LOOK_ALTTAB_PAD)
        return (struct comp_box){ 0, 0, 0, 0 };
    return r;
}

bool alttab_press(int32_t x, int32_t y)
{
    if (!alttab.shown || !box_contains(alttab_box(), x, y))
        return false;
    for (unsigned i = 0; i < alttab.n; i++)
        if (box_contains(alttab_row_box(i), x, y)) {
            alttab.sel = i;
            alttab_end(true);
            break;
        }
    return true;
}
