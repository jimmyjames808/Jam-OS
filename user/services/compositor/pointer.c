/* wl_pointer (seat.h): the pointer's position, its focus and the implicit
 * grab, buttons, the wheel, the client's cursor, and grabs of the
 * compositor's own.
 *
 * Position: the compositor's alone. Relative counts from the mice, with
 * libfun's acceleration (pointer_move: 1:1 up to 6 counts a report, then
 * up to 2:1), in 1/256 pixel, clamped to the output; Wayland's fixed
 * numbers are 24.8, so a surface's coordinates are that minus the
 * window's place, exactly. A client learns where the pointer is only
 * while it is over one of its windows.
 *
 * Focus: the window under the pointer (wm_window_at on its surface's input
 * region, so another window's title bar or frame over it hides it; the
 * desktop's strip and cards too) gets wl_pointer.enter, motion, buttons
 * and the wheel; leaving it
 * sends leave. While a button is held the window the press went to keeps
 * every pointer event, even outside it (the implicit grab), until the last
 * button is released; then the window under the pointer gets the focus
 * again. A press on no window (the background) or on one that goes holds
 * the pointer the same way: no window gets it until the release. A press with no button held before it may first be the window
 * manager's (wm_press: a title bar, an edge), and else focuses the window
 * it lands in (focus_click). Each group of events ends with frame.
 *
 * Serials: every press's serial is kept with the client it went to while
 * its button is held, so the window manager honours xdg_toplevel.move and
 * resize only with a live one (seat_button_serial_ok). A grab of the
 * compositor's own (seat_grab_begin: a move, a resize) takes the pointer
 * from every client until the last button is released.
 *
 * A client that is behind on reading (jwl_conn_backlogged) has its motion
 * coalesced: the newest position is sent once it has caught up.
 *
 * The cursor: struct comp_cursor, read by the painting code. A client may
 * set its own (set_cursor with the serial of its latest enter; a surface
 * of the cursor role), or none, while the pointer is over its window;
 * elsewhere the default arrow. */
#include <fun.h>
#include <jwl/wayland.h>
#include "seat.h"

#define BUTTONS     3        /* left, right, middle: the input protocol's bits 0-2 */
#define BTN_LEFT    0x110    /* evdev's button codes (input-event-codes.h); +1 right, +2 middle */
#define WHEEL_STEP  10       /* axis units a notch, as other compositors send */

struct comp_cursor cursor;
static struct pointer pos;                      /* the position, 1/256 pixel */
static struct comp_window *over;                /* the pointer's focus, or NULL */
static bool implicit;                           /* over keeps it until the buttons are up */
static uint8_t buttons;                         /* held, input bits */
static uint8_t wm_buttons;                      /* presses the window manager took */
static uint32_t press_serial[BUTTONS];          /* each held button's press serial ... */
static const struct comp_client *press_client[BUTTONS];   /* ... and who got it (NULL: nobody) */
static const struct comp_grab_ops *grab_ops;    /* a grab of the compositor's own, or NULL */
static void *grab_data;

void pointer_init_position(void)
{
    pointer_init(&pos, scene.width, scene.height, true);
    cursor.x = pointer_x(&pos);
    cursor.y = pointer_y(&pos);
}

/* ---- events to the focused client --------------------------------------------------- */

static int32_t local_x(const struct comp_window *w)
{
    return pos.x256 - (int32_t)((uint32_t)window_shown_x(w) << 8);
}

static int32_t local_y(const struct comp_window *w)
{
    return pos.y256 - (int32_t)((uint32_t)window_shown_y(w) << 8);
}

static void send_frame(struct comp_client *cl)
{
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
        if (r->version >= JWL_WL_POINTER_EV_FRAME_SINCE)
            (void)jwl_wl_pointer_send_frame(cl->conn, r->id);   /* a dead conn: torn down */
}

static void send_enter(struct comp_window *w, const struct seat_res *only)
{
    struct comp_client *cl = w->surface->client;
    struct seat_client *sc = seat_of(cl);
    sc->enter_serial = comp_serial();
    for (struct seat_res *r = sc->res[SEAT_POINTER]; r; r = r->next) {
        if (only && r != only)
            continue;
        (void)jwl_wl_pointer_send_enter(cl->conn, r->id, sc->enter_serial, w->surface->id,
                                        local_x(w), local_y(w));
        if (r->version >= JWL_WL_POINTER_EV_FRAME_SINCE)
            (void)jwl_wl_pointer_send_frame(cl->conn, r->id);
    }
}

static void send_motion(struct comp_window *w)
{
    struct comp_client *cl = w->surface->client;
    if (jwl_conn_backlogged(cl->conn)) {
        seat_of(cl)->motion_owed = true;   /* the newest position, once it has caught up */
        return;
    }
    seat_of(cl)->motion_owed = false;
    uint32_t t = comp_ms(now());
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
        (void)jwl_wl_pointer_send_motion(cl->conn, r->id, t, local_x(w), local_y(w));
    send_frame(cl);
}

static void cursor_update(void);

void seat_cursor_changed(void)
{
    cursor_update();
}

enum cursor_shape cursor_shape_at(int32_t x, int32_t y)
{
    enum cursor_shape s = desk_cursor_at(x, y);
    if (s == CURSOR_SHAPES)
        s = wm_cursor_at(x, y);
    return s == CURSOR_ARROW && desk_busy() ? CURSOR_BUSY : s;
}

/* The cursor's picture: the client's under the pointer (a surface, none,
 * or one of the set), else the compositor's for what is there (and during
 * a grab of its own, what it was when the grab began: a resize's arrows). */
static void cursor_update(void)
{
    struct comp_surface *s = NULL;
    bool hidden = false;
    enum cursor_shape shape = cursor.shape;
    struct seat_client *sc = over ? seat_of(over->surface->client) : NULL;
    if (sc && sc->shape_set) {
        shape = sc->shape == CURSOR_ARROW && desk_busy() ? CURSOR_BUSY : sc->shape;
    } else if (sc && sc->cursor_set) {
        s = sc->cursor;
        hidden = !s;
    } else if (sc) {
        shape = desk_busy() ? CURSOR_BUSY : CURSOR_ARROW;
    } else if (!grab_ops) {
        shape = cursor_shape_at(cursor.x, cursor.y);
    }
    int32_t hx = s ? sc->hot_x : 0, hy = s ? sc->hot_y : 0;
    if (s == cursor.surface && hidden == cursor.hidden && hx == cursor.hot_x &&
        hy == cursor.hot_y && (s || shape == cursor.shape))
        return;
    cursor.surface = s;
    cursor.hidden = hidden;
    cursor.hot_x = hx;
    cursor.hot_y = hy;
    cursor.shape = shape;
    cursor_moved(cursor.x, cursor.y);
}

/* The pointer's focus to w (NULL: none): leave, then enter. */
static void set_over(struct comp_window *w)
{
    if (w == over)
        return;
    struct comp_window *old = over;
    over = w;
    if (old && surface_live(old->surface)) {
        struct comp_client *cl = old->surface->client;
        uint32_t serial = comp_serial();
        for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
            (void)jwl_wl_pointer_send_leave(cl->conn, r->id, serial, old->surface->id);
        send_frame(cl);
    }
    if (w && surface_live(w->surface))
        send_enter(w, NULL);
    cursor_update();
}

/* The window whose surface is under the pointer, if its client is alive:
 * none over another window's decorations (wm_window_at sees title bars and
 * frames, which hide what is below them), nor over the desktop's strip or
 * cards, nor over a gap between tiles a press would drag. */
static struct comp_window *under_pointer(void)
{
    bool on_surface;
    if (desk_covers(cursor.x, cursor.y) || wm_gap_covers(cursor.x, cursor.y))
        return NULL;
    struct comp_window *w = wm_window_at(cursor.x, cursor.y, &on_surface);
    return w && on_surface && client_alive(w->surface->client) ? w : NULL;
}

/* ---- what the mice send -------------------------------------------------------------- */

static void move(int16_t dx, int16_t dy)
{
    int32_t ox = cursor.x, oy = cursor.y, ox256 = pos.x256, oy256 = pos.y256;
    pointer_move(&pos, dx, dy);
    cursor.x = pointer_x(&pos);
    cursor.y = pointer_y(&pos);
    if (cursor.x != ox || cursor.y != oy)
        cursor_moved(ox, oy);
    if (pos.x256 == ox256 && pos.y256 == oy256)
        return;   /* against an edge */
    if (grab_ops) {
        grab_ops->motion(grab_data, cursor.x, cursor.y);
        return;
    }
    struct comp_window *before = over;
    if (!implicit)
        set_over(under_pointer());
    cursor_update();   /* the compositor's cursor follows what is under it */
    if (over && over == before && surface_live(over->surface))
        send_motion(over);   /* a new focus had the position in its enter */
}

static void press(unsigned b)
{
    bool first = !(buttons & ~(1u << b));
    if (first && !grab_ops) {
        set_over(under_pointer());
        implicit = true;   /* whatever it lands on (a window, or none) keeps the pointer */
        if (wm_press(cursor.x, cursor.y, BTN_LEFT + b, keyboard_held_mods())) {
            wm_buttons |= (uint8_t)(1u << b);   /* its release goes nowhere too */
            return;
        }
        if (over)
            focus_click(over);
    }
    if (grab_ops || !over || !surface_live(over->surface))
        return;
    struct comp_client *cl = over->surface->client;
    uint32_t serial = comp_serial(), t = comp_ms(now());
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
        (void)jwl_wl_pointer_send_button(cl->conn, r->id, serial, t, BTN_LEFT + b,
                                         JWL_WL_POINTER_BUTTON_STATE_PRESSED);
    send_frame(cl);
    press_serial[b] = serial;
    press_client[b] = cl;
}

static void release(unsigned b)
{
    const struct comp_client *got = press_client[b];
    press_client[b] = NULL;
    press_serial[b] = 0;
    if (wm_buttons & (1u << b))
        wm_buttons &= (uint8_t)~(1u << b);
    else if (!grab_ops && over && got == over->surface->client && surface_live(over->surface)) {
        struct comp_client *cl = over->surface->client;
        uint32_t serial = comp_serial(), t = comp_ms(now());
        for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
            (void)jwl_wl_pointer_send_button(cl->conn, r->id, serial, t, BTN_LEFT + b,
                                             JWL_WL_POINTER_BUTTON_STATE_RELEASED);
        send_frame(cl);
    }
    if (buttons)
        return;
    implicit = false;
    if (grab_ops) {
        const struct comp_grab_ops *ops = grab_ops;
        grab_ops = NULL;
        ops->end(grab_data);
    }
    set_over(under_pointer());   /* the grab is over: whatever is under it now */
}

static void wheel_turned(int8_t wheel)
{
    if (grab_ops || !over || !surface_live(over->surface))
        return;
    struct comp_client *cl = over->surface->client;
    uint32_t t = comp_ms(now()), axis = JWL_WL_POINTER_AXIS_VERTICAL_SCROLL;
    int32_t notches = -wheel;   /* a wheel pushed away scrolls up; the axis is positive down */
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next) {
        if (r->version >= JWL_WL_POINTER_EV_AXIS_SOURCE_SINCE) {
            (void)jwl_wl_pointer_send_axis_source(cl->conn, r->id,
                                                  JWL_WL_POINTER_AXIS_SOURCE_WHEEL);
            (void)jwl_wl_pointer_send_axis_discrete(cl->conn, r->id, axis, notches);
        }
        (void)jwl_wl_pointer_send_axis(cl->conn, r->id, t, axis, notches * WHEEL_STEP * 256);
    }
    send_frame(cl);
}

void pointer_report(int16_t dx, int16_t dy, int8_t wheel, uint8_t now_buttons)
{
    if (dx || dy)
        move(dx, dy);
    for (unsigned b = 0; b < BUTTONS; b++) {
        uint8_t m = (uint8_t)(1u << b);
        if ((now_buttons & m) == (buttons & m))
            continue;
        buttons ^= m;
        if (buttons & m)
            press(b);
        else
            release(b);
    }
    if (wheel)
        wheel_turned(wheel);
}

/* ---- the seat's part of a turn, and things going ------------------------------------ */

void pointer_turn(void)
{
    if (!grab_ops && !implicit)
        set_over(under_pointer());   /* a window appeared, moved or went under it */
    if (over && surface_live(over->surface) && seat_of(over->surface->client)->motion_owed &&
        !jwl_conn_backlogged(over->surface->client->conn))
        send_motion(over);
}

void pointer_window_gone(struct comp_window *w)
{
    if (w != over)
        return;
    set_over(NULL);   /* leave, while the client still knows the surface */
    /* A drag that started in it goes on to nobody until the buttons are up. */
}

void pointer_client_gone(struct comp_client *cl)
{
    for (unsigned b = 0; b < BUTTONS; b++)
        if (press_client[b] == cl)
            press_client[b] = NULL;
    if (cursor.surface && cursor.surface->client == cl) {
        cursor.surface = NULL;
        cursor_moved(cursor.x, cursor.y);
    }
}

/* ---- the window manager's side -------------------------------------------------------- */

bool seat_button_serial_ok(const struct comp_client *cl, uint32_t serial)
{
    for (unsigned b = 0; b < BUTTONS; b++)
        if ((buttons & (1u << b)) && press_client[b] == cl && press_serial[b] == serial && serial)
            return true;
    return false;
}

status_t seat_grab_begin(const struct comp_grab_ops *ops, void *data)
{
    if (!buttons || grab_ops)
        return ERR_BAD_STATE;
    set_over(NULL);   /* no client sees the pointer during the grab */
    implicit = false;
    grab_ops = ops;
    grab_data = data;
    return OK;
}

void seat_grab_cancel(void)
{
    grab_ops = NULL;
    grab_data = NULL;
}

/* ---- wl_pointer's requests --------------------------------------------------------- */

/* A cursor surface's commit: its picture may have changed. (attach's x
 * and y would move the hotspot; not built yet: set_cursor's hotspot
 * stands.) */
static status_t cursor_commit(struct comp_surface *s)
{
    if (cursor.surface == s)
        cursor_moved(cursor.x, cursor.y);
    return OK;
}

static void cursor_gone(struct comp_surface *s, bool dead)
{
    struct seat_client *sc = s->role_data;
    (void)dead;
    if (sc->cursor == s) {
        sc->cursor = NULL;
        sc->cursor_set = false;   /* the arrow again */
    }
    if (cursor.surface == s) {
        cursor.surface = NULL;
        cursor_update();
    }
}

static const struct comp_role_ops cursor_role = {
    .name = "cursor", .commit = cursor_commit, .gone = cursor_gone,
};

static status_t on_set_cursor(void *data, uint32_t self, uint32_t serial, uint32_t surface,
                              int32_t hot_x, int32_t hot_y)
{
    struct seat_res *r = data;
    struct comp_client *cl = r->client;
    struct seat_client *sc = seat_of(cl);
    if (serial != sc->enter_serial)
        return OK;   /* not after its latest enter: ignored, as the protocol says */
    struct comp_surface *s = NULL;
    if (surface && !(s = comp_object(cl, surface, &jwl_wl_surface_interface)))
        return comp_error(cl, self, JWL_ERROR_INVALID_OBJECT, "no surface %u", surface);
    if (s && s->role_ops != &cursor_role &&
        surface_set_role(s, COMP_ROLE_CURSOR, &cursor_role, sc) != OK)
        return comp_error(cl, self, JWL_WL_POINTER_ERROR_ROLE, "surface %u has another role",
                          surface);
    sc->cursor_set = true;   /* a surface replaces a shape */
    sc->shape_set = false;
    sc->cursor = s;
    sc->hot_x = hot_x;
    sc->hot_y = hot_y;
    if (over && over->surface->client == cl)
        cursor_update();
    return OK;
}

static status_t on_release(void *data, uint32_t self)
{
    (void)self;
    seat_res_free(data);   /* libjwl freed the id */
    return OK;
}

static const struct jwl_wl_pointer_requests pointer_ops = {
    .set_cursor = on_set_cursor, .release = on_release,
};

status_t pointer_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data knows its client */
    return jwl_wl_pointer_dispatch_request(&pointer_ops, m->data, m->id, m->opcode, m->args);
}

status_t pointer_create(struct comp_client *cl, uint32_t id, uint32_t version)
{
    struct seat_res *r;
    status_t st = seat_res_add(cl, SEAT_POINTER, id, version, &r);
    if (st == OK && over && over->surface->client == cl && surface_live(over->surface))
        send_enter(over, r);
    return st;
}
