/* Notifications (desk.h; the owner's picks in docs/G1-PLAN.md "The
 * look"): frosted cards stacked down the top right of the screen under the
 * strip, the newest on top: a letter tile, a title and one line, cut short
 * to fit. A card without buttons fades away after LOOK_NOTE_SHOW_MS (a
 * click on it sends it sooner); one with buttons ("Reboot", "Later") stays
 * until one is pressed, which the plumbing hears (ctl_notify_answered).
 * Cards slide in from the right while they fade in, and out the same way.
 * At most NOTIFY_MAX at once: a new one pushes out the oldest without
 * buttons (or the oldest at all).
 *
 * notify_post is the compositor's own way in; the plumbing (a later track)
 * makes it a compctl method for services. Every notice still goes to the
 * first terminal too: that is the poster's to do, not the card's.
 * popdraw.c draws the cards from the boxes here. */
#include "desk.h"

struct notify_state notes;

/* ---- the cards' sizes --------------------------------------------------------------------- */

#define TITLE_H 18   /* the title's line */
#define BODY_H  17   /* the body's */
#define BTN_GAP 6

static int32_t card_h(const struct notify_card *c)
{
    int32_t text = TITLE_H + (c->body[0] ? BODY_H : 0);
    if (c->nbuttons)
        text += 8 + LOOK_NOTE_BTN_H;
    int32_t inner = text > LOOK_NOTE_TILE ? text : LOOK_NOTE_TILE;
    return 2 * LOOK_NOTE_PAD_Y + inner;
}

/* Where each card goes: down from LOOK_NOTE_TOP, slid right by its dx. */
static void place(void)
{
    int32_t y = LOOK_NOTE_TOP, x2 = scene.width - LOOK_NOTE_RIGHT;
    for (unsigned i = 0; i < notes.n; i++) {
        struct notify_card *c = &notes.cards[i];
        struct comp_box b = { x2 - LOOK_NOTE_W + c->dx, y, x2 + c->dx, y + card_h(c) };
        if (b.x1 != c->box.x1 || b.y1 != c->box.y1 || b.y2 != c->box.y2) {
            desk_damage(c->box);
            desk_damage(b);
        }
        c->box = b;
        y = b.y2 + LOOK_NOTE_GAP;
    }
}

struct comp_box notify_box(void)
{
    struct comp_box all = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < notes.n; i++)
        all = box_bounds(all, notes.cards[i].box);
    return all;
}

struct comp_box notify_button_box(const struct notify_card *c, unsigned b)
{
    if (b >= c->nbuttons)
        return (struct comp_box){ 0, 0, 0, 0 };
    int32_t x = c->box.x1 + LOOK_NOTE_PAD_X + LOOK_NOTE_TILE + 10;
    int32_t y = c->box.y1 + LOOK_NOTE_PAD_Y + TITLE_H + (c->body[0] ? BODY_H : 0) + 8;
    for (unsigned i = 0;; i++) {
        int32_t w = desk_text_w(desk_font.r12, c->buttons[i]) + 20;
        if (i == b)
            return (struct comp_box){ x, y, x + w, y + LOOK_NOTE_BTN_H };
        x += w + BTN_GAP;
    }
}

/* ---- posting and going -------------------------------------------------------------------- */

static void copy(char *to, size_t n, const char *from)
{
    snprintf(to, n, "%s", from ? from : "");
}

static void drop(unsigned i)
{
    desk_damage(notes.cards[i].box);
    memmove(&notes.cards[i], &notes.cards[i + 1], (notes.n - i - 1) * sizeof(notes.cards[0]));
    notes.n--;
    memset(&notes.cards[notes.n], 0, sizeof(notes.cards[0]));
    place();
}

/* Room for one more: the oldest without buttons goes (else the oldest). */
static void make_room(void)
{
    if (notes.n < NOTIFY_MAX)
        return;
    for (unsigned i = notes.n; i-- > 0;)
        if (!notes.cards[i].nbuttons) {
            drop(i);
            return;
        }
    drop(notes.n - 1);
}

uint32_t notify_post(const struct notify_spec *n)
{
    if (!desk_on() || !n || !n->title)
        return 0;
    make_room();
    memmove(&notes.cards[1], &notes.cards[0], notes.n * sizeof(notes.cards[0]));
    notes.n++;
    struct notify_card *c = &notes.cards[0];
    memset(c, 0, sizeof(*c));
    if (++notes.next_id == 0)
        notes.next_id = 1;
    c->id = notes.next_id;
    copy(c->title, sizeof(c->title), n->title);
    copy(c->body, sizeof(c->body), n->body);
    c->letter = n->letter;
    c->colour = n->colour ? n->colour : LOOK_JAM_BLACKCURRANT;
    c->nbuttons = n->nbuttons < NOTIFY_BUTTONS_MAX ? n->nbuttons : NOTIFY_BUTTONS_MAX;
    for (unsigned b = 0; b < c->nbuttons; b++)
        copy(c->buttons[b], sizeof(c->buttons[b]), n->buttons[b]);
    c->posted = now();
    c->alpha = anim_enabled() ? 0 : 255;
    c->dx = anim_enabled() ? 16 : 0;
    place();
    return c->id;
}

/* Card i begins to leave (it fades, then goes). */
static void leave(unsigned i)
{
    struct notify_card *c = &notes.cards[i];
    if (!anim_enabled()) {
        drop(i);
        return;
    }
    if (!c->leaving)
        c->leaving = now();
}

void notify_withdraw(uint32_t id)
{
    for (unsigned i = 0; i < notes.n; i++)
        if (notes.cards[i].id == id) {
            leave(i);
            return;
        }
}

/* ---- time --------------------------------------------------------------------------------- */

/* A fade's part (0..255) of ms done at t since t0. */
static uint32_t part(uint64_t t, uint64_t t0, uint32_t ms)
{
    uint64_t d = t - t0, all = (uint64_t)ms * NS_PER_MS;
    return d >= all ? 255 : (uint32_t)(d * 255 / all);
}

void notify_tick(uint64_t t)
{
    for (unsigned i = 0; i < notes.n;) {
        struct notify_card *c = &notes.cards[i];
        if (!c->nbuttons && !c->leaving && t >= c->posted + LOOK_NOTE_SHOW_MS * NS_PER_MS)
            leave(i);
        if (i >= notes.n || c->id == 0)
            continue;   /* dropped (animations off) */
        uint32_t a = part(t, c->posted, LOOK_NOTE_IN_MS);
        if (c->leaving) {
            uint32_t out = part(t, c->leaving, LOOK_NOTE_OUT_MS);
            if (out == 255) {
                drop(i);
                continue;
            }
            a = a < 255 - out ? a : 255 - out;
        }
        int32_t dx = 16 - (int32_t)(a * 16 / 255);
        if (a != c->alpha || dx != c->dx) {
            c->alpha = a;
            c->dx = dx;
            desk_damage(c->box);
        }
        i++;
    }
    place();
}

uint64_t notify_deadline(void)
{
    uint64_t d = DEADLINE_NEVER;
    for (unsigned i = 0; i < notes.n; i++) {
        const struct notify_card *c = &notes.cards[i];
        uint64_t next = DEADLINE_NEVER;
        if (c->alpha < 255 || c->leaving)
            next = scene.last_paint_ns + comp.period_ns;   /* fading: a frame each paint */
        else if (!c->nbuttons)
            next = c->posted + LOOK_NOTE_SHOW_MS * NS_PER_MS;
        d = next < d ? next : d;
    }
    return d;
}

/* ---- presses ------------------------------------------------------------------------------ */

bool notify_press(int32_t x, int32_t y)
{
    for (unsigned i = 0; i < notes.n; i++) {
        struct notify_card *c = &notes.cards[i];
        if (!box_contains(c->box, x, y))
            continue;
        if (!c->nbuttons) {
            leave(i);
            return true;
        }
        for (unsigned b = 0; b < c->nbuttons; b++)
            if (box_contains(notify_button_box(c, b), x, y)) {
                uint32_t id = c->id;
                leave(i);
                ctl_notify_answered(id, b);
                return true;
            }
        return true;   /* on the card, not on a button */
    }
    return false;
}
