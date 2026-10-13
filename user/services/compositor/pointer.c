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
 * sends leave. Enter, motion and leave name the topmost surface of the
 * window's tree under the pointer (window_input_at: a subsurface's own
 * input region), in its coordinates; going from one surface of a tree to
 * another is a leave and an enter. While a button is held the window the press went to keeps
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
 * The wheel, by the pointer's version: a notch is axis WHEEL_STEP, and
 * from version 5 also axis_source wheel with axis_discrete 1 a notch, or
 * from version 8 axis_value120 120 a notch in its place; from version 9
 * axis_relative_direction says identical (the wheel and the content move
 * the way the axis says: no natural scrolling).
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
static struct comp_surface *over_s;             /* ... the surface of its tree under it */
static int32_t over_x, over_y;                  /* over_s's (0, 0) on the output, as last drawn */
static bool implicit;                          /* over keeps it until the buttons are up */
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

/* over_s's place on the output brought up to date; while it isn't drawn
 * (a drag held on a subsurface that went), its last place stands. */
static void place_over(void)
{
    int32_t x, y;
    if (over_s && surface_on_output(over_s, &x, &y)) {
        over_x = x;
        over_y = y;
    }
}

/* The pointer in over_s's coordinates (wl_fixed). */
static int32_t local_x(void)
{
    return pos.x256 - (int32_t)((uint32_t)over_x << 8);
}

static int32_t local_y(void)
{
    return pos.y256 - (int32_t)((uint32_t)over_y << 8);
}

static void send_frame(struct comp_client *cl)
{
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
        if (r->version >= JWL_WL_POINTER_EV_FRAME_SINCE)
            (void)jwl_wl_pointer_send_frame(cl->conn, r->id);   /* a dead conn: torn down */
}

/* enter on over_s, to every wl_pointer of its client's (or only one). */
static void send_enter(const struct seat_res *only)
{
    struct comp_client *cl = over_s->client;
    struct seat_client *sc = seat_of(cl);
    sc->enter_serial = comp_serial();
    place_over();
    for (struct seat_res *r = sc->res[SEAT_POINTER]; r; r = r->next) {
        if (only && r != only)
            continue;
        (void)jwl_wl_pointer_send_enter(cl->conn, r->id, sc->enter_serial, over_s->id, local_x(),
                                        local_y());
        if (r->version >= JWL_WL_POINTER_EV_FRAME_SINCE)
            (void)jwl_wl_pointer_send_frame(cl->conn, r->id);
    }
}

/* motion on over_s, to its client. */
static void send_motion(void)
{
    struct comp_client *cl = over_s->client;
    if (jwl_conn_backlogged(cl->conn)) {
        seat_of(cl)->motion_owed = true;   /* the newest position, once it has caught up */
        return;
    }
    seat_of(cl)->motion_owed = false;
    place_over();
    uint32_t t = comp_ms(now());
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
        (void)jwl_wl_pointer_send_motion(cl->conn, r->id, t, local_x(), local_y());
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

/* The pointer's focus to surface s of window w's tree (both NULL: none):
 * leave the old surface, then enter s. A move between the surfaces of one
 * window is a leave and an enter too, as the protocol has it. */
static void set_over(struct comp_window *w, struct comp_surface *s)
{
    if (w == over && s == over_s)
        return;
    struct comp_surface *old = over_s;
    over = w;
    over_s = s;
    if (old && surface_live(old)) {
        struct comp_client *cl = old->client;
        uint32_t serial = comp_serial();
        for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next)
            (void)jwl_wl_pointer_send_leave(cl->conn, r->id, serial, old->id);
        send_frame(cl);
    }
    if (s && surface_live(s))
        send_enter(NULL);
    cursor_update();
}

/* The window whose surface (or a subsurface of its) is under the pointer,
 * if its client is alive, and in *s the topmost surface of its tree there:
 * none over another window's decorations (wm_window_at sees title bars and
 * frames, which hide what is below them), nor over the desktop's strip or
 * cards, nor over a gap between tiles a press would drag. */
static struct comp_window *under_pointer(struct comp_surface **s)
{
    bool on_surface;
    *s = NULL;
    if (desk_covers(cursor.x, cursor.y) || wm_gap_covers(cursor.x, cursor.y))
        return NULL;
    struct comp_window *w = wm_window_at(cursor.x, cursor.y, &on_surface);
    if (!w || !on_surface || !client_alive(w->surface->client))
        return NULL;
    *s = window_input_at(w, cursor.x, cursor.y, NULL, NULL);
    return *s ? w : NULL;
}

/* The focus to what is under the pointer now. */
static void over_what_is_under(void)
{
    struct comp_surface *s;
    struct comp_window *w = under_pointer(&s);
    set_over(w, s);
}

/* ---- what the mice send -------------------------------------------------------------- */

static void move(int16_t dx, int16_t dy)
{
    int32_t ox = cursor.x, oy = cursor.y, ox256 = pos.x256, oy256 = pos.y256;
    pointer_move(&pos, dx, dy);
    cursor.x = pointer_x(&pos);
    cursor.y = pointer_y(&pos);
    if (cursor.x != ox || cursor.y != oy) {
        cursor.moved = true;   /* a gap under it may light from now on */
        cursor_moved(ox, oy);
    }
    if (pos.x256 == ox256 && pos.y256 == oy256)
        return;   /* against an edge */
    if (grab_ops) {
        grab_ops->motion(grab_data, cursor.x, cursor.y);
        return;
    }
    struct comp_surface *before = over_s;
    if (!implicit)
        over_what_is_under();
    cursor_update();   /* the compositor's cursor follows what is under it */
    if (over_s && over_s == before && surface_live(over_s))
        send_motion();   /* a new focus had the position in its enter */
}

static void press(unsigned b)
{
    bool first = !(buttons & ~(1u << b));
    if (first && !grab_ops) {
        over_what_is_under();
        implicit = true;   /* whatever it lands on (a window, or none) keeps the pointer */
        if (wm_press(cursor.x, cursor.y, BTN_LEFT + b, keyboard_held_mods())) {
            wm_buttons |= (uint8_t)(1u << b);   /* its release goes nowhere too */
            return;
        }
        if (over)
            focus_click(over);
    }
    if (grab_ops || !over || !surface_live(over_s))
        return;
    struct comp_client *cl = over_s->client;
    uint32_t serial = comp_serial(), t = comp_ms(now());
    seat_input_serial(cl, serial);
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
    else if (!grab_ops && over && got == over_s->client && surface_live(over_s)) {
        struct comp_client *cl = over_s->client;
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
    over_what_is_under();   /* the grab is over: whatever is under it now */
}

static void wheel_turned(int8_t wheel)
{
    if (grab_ops || !over || !surface_live(over_s))
        return;
    struct comp_client *cl = over_s->client;
    uint32_t t = comp_ms(now()), axis = JWL_WL_POINTER_AXIS_VERTICAL_SCROLL;
    int32_t notches = -wheel;   /* a wheel pushed away scrolls up; the axis is positive down */
    for (struct seat_res *r = seat_of(cl)->res[SEAT_POINTER]; r; r = r->next) {
        if (r->version >= JWL_WL_POINTER_EV_AXIS_SOURCE_SINCE)
            (void)jwl_wl_pointer_send_axis_source(cl->conn, r->id,
                                                  JWL_WL_POINTER_AXIS_SOURCE_WHEEL);
        if (r->version >= JWL_WL_POINTER_EV_AXIS_RELATIVE_DIRECTION_SINCE)
            (void)jwl_wl_pointer_send_axis_relative_direction(
                cl->conn, r->id, axis, JWL_WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        if (r->version >= JWL_WL_POINTER_EV_AXIS_VALUE120_SINCE)
            (void)jwl_wl_pointer_send_axis_value120(cl->conn, r->id, axis, notches * 120);
        else if (r->version >= JWL_WL_POINTER_EV_AXIS_DISCRETE_SINCE)
            (void)jwl_wl_pointer_send_axis_discrete(cl->conn, r->id, axis, notches);
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
        over_what_is_under();   /* a window or subsurface appeared, moved or went under it */
    if (over_s && surface_live(over_s) && seat_of(over_s->client)->motion_owed &&
        !jwl_conn_backlogged(over_s->client->conn))
        send_motion();
}

void pointer_window_gone(struct comp_window *w)
{
    if (w != over)
        return;
    set_over(NULL, NULL);   /* leave, while the client still knows the surface */
    /* A drag that started in it goes on to nobody until the buttons are up. */
}

void seat_surface_gone(struct comp_surface *s)
{
    if (s == over_s)
        set_over(NULL, NULL);   /* (its id is gone already: no leave) */
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
    set_over(NULL, NULL);   /* no client sees the pointer during the grab */
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
    /* held to a buffer's size either way: the pointer's place less it never
     * overflows (review C6); a hot spot further out shows nothing either way */
    sc->hot_x = hot_x < -COMP_BUFFER_SIDE_MAX ? -COMP_BUFFER_SIDE_MAX
                : hot_x > COMP_BUFFER_SIDE_MAX ? COMP_BUFFER_SIDE_MAX : hot_x;
    sc->hot_y = hot_y < -COMP_BUFFER_SIDE_MAX ? -COMP_BUFFER_SIDE_MAX
                : hot_y > COMP_BUFFER_SIDE_MAX ? COMP_BUFFER_SIDE_MAX : hot_y;
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
    if (st == OK && over && over_s->client == cl && surface_live(over_s))
        send_enter(r);
    return st;
}
