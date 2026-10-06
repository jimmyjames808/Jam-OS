/* libjwl's client: the seat (<jwl_client.h>), its keyboard and pointer.
 *
 * The keyboard. The compositor sends evdev key codes and, once, a keymap
 * VMO in XKB's text format; its first line names the layout
 * (<keymap.h>), so the client reads only that line (vmo_read, never a
 * mapping of the compositor's memory) and decodes every key with the
 * matching generated table: a keysym and a character, from the code and
 * the modifiers wl_keyboard.modifiers last said (XKB's real modifier
 * bits, which are KEYMAP_MOD_*). A keymap that isn't ours, or none, gives
 * the US layout, the only one G1 has.
 *
 * Key repeat is the client's job in Wayland: the compositor says the rate
 * and the delay (repeat_info), and the library makes JWL_KEY_REPEATED
 * events for the last key pressed while it is held, the focus stays and
 * the layout says the key repeats. A program slower than the rate gets
 * one repeat per dispatch, not a burst.
 *
 * The pointer: enter, leave, motion, buttons and the wheel, each about the
 * window under it (or holding the button's grab). A button press's
 * serial is kept on its window for jwl_window_move. axis_discrete comes
 * before the axis event it belongs to and is folded into it. */
#include <jwl_client.h>
#include <keymap.h>
#include <os.h>
#include "jwlc.h"

#define HEAD_MAX 128u   /* bytes of the keymap read: its first line */

static void queue_simple(struct jwl_client *c, uint32_t type, struct jwl_window *w)
{
    struct jwl_event ev = { .type = type, .win = w };
    jwlc_queue(c, &ev);
}

/* ---- the seat's capabilities --------------------------------------------------------- */

static status_t get_device(struct jwl_client *c, bool keyboard)
{
    const struct jwl_interface *iface = keyboard ? &jwl_wl_keyboard_interface
                                                 : &jwl_wl_pointer_interface;
    uint32_t id, seat = c->global[JWLC_SEAT];
    status_t st = jwlc_make(c, iface, c->info.seat_version, c, &id);
    if (st == OK && keyboard)
        st = jwlc_made(c, jwl_wl_seat_get_keyboard(c->conn, seat, id), id);
    else if (st == OK)
        st = jwlc_made(c, jwl_wl_seat_get_pointer(c->conn, seat, id), id);
    if (st == OK)
        *(keyboard ? &c->seat.keyboard : &c->seat.pointer) = id;
    /* the pointer's shape device, if the compositor has shapes */
    uint32_t mgr = c->global[JWLC_CURSOR_SHAPE], dev;
    if (st == OK && !keyboard && mgr &&
        jwlc_make(c, &jwl_wp_cursor_shape_device_v1_interface, c->info.cursor_shape_version, c,
                  &dev) == OK &&
        jwlc_made(c, jwl_wp_cursor_shape_manager_v1_get_pointer(c->conn, mgr, dev, id), dev) == OK)
        c->seat.shape_dev = dev;
    return st;
}

/* The shape asked for, with the pointer's last enter: OK if none is asked
 * or there is no pointer over us. */
static status_t send_shape(struct jwl_client *c)
{
    if (!c->cursor_shape || !c->seat.shape_dev || !c->seat.ptr_focus)
        return OK;
    return jwl_wp_cursor_shape_device_v1_set_shape(c->conn, c->seat.shape_dev,
                                                    c->seat.enter_serial, c->cursor_shape);
}

status_t jwl_client_set_cursor(struct jwl_client *c, uint32_t shape)
{
    c->cursor_shape = shape;
    if (c->state != JWLC_READY)
        return OK;   /* sent at the next enter */
    if (!c->global[JWLC_CURSOR_SHAPE])
        return ERR_NOT_SUPPORTED;
    status_t st = send_shape(c);
    return st == OK ? jwl_client_flush(c) : st;
}

/* The device went from the seat: release it (a destructor from version 3). */
static void put_device(struct jwl_client *c, bool keyboard)
{
    uint32_t *id = keyboard ? &c->seat.keyboard : &c->seat.pointer;
    if (c->info.seat_version >= JWL_WL_KEYBOARD_REQ_RELEASE_SINCE) {
        if (keyboard)
            (void)jwl_wl_keyboard_release(c->conn, *id);
        else
            (void)jwl_wl_pointer_release(c->conn, *id);
    }
    *id = 0;
    if (!keyboard && c->seat.shape_dev) {   /* its shape device goes with it */
        (void)jwl_wp_cursor_shape_device_v1_destroy(c->conn, c->seat.shape_dev);
        c->seat.shape_dev = 0;
    }
    struct jwl_window **focus = keyboard ? &c->seat.kb_focus : &c->seat.ptr_focus;
    if (*focus)
        queue_simple(c, keyboard ? JWL_EV_KEYBOARD_LEAVE : JWL_EV_POINTER_LEAVE, *focus);
    *focus = NULL;
    if (keyboard)
        c->seat.repeat_code = 0;
}

status_t jwlc_seat_caps(struct jwl_client *c, uint32_t caps)
{
    c->info.seat_caps = caps;
    status_t st = OK;
    bool kb = (caps & JWL_WL_SEAT_CAPABILITY_KEYBOARD) && !c->cfg.no_keyboard;
    bool ptr = caps & JWL_WL_SEAT_CAPABILITY_POINTER;
    if (kb && !c->seat.keyboard)
        st = get_device(c, true);
    else if (!kb && c->seat.keyboard)
        put_device(c, true);
    if (st == OK && ptr && !c->seat.pointer)
        st = get_device(c, false);
    else if (st == OK && !ptr && c->seat.pointer)
        put_device(c, false);
    return st;
}

void jwlc_seat_lost(struct jwl_client *c)
{
    if (c->seat.kb_focus)
        queue_simple(c, JWL_EV_KEYBOARD_LEAVE, c->seat.kb_focus);
    if (c->seat.ptr_focus)
        queue_simple(c, JWL_EV_POINTER_LEAVE, c->seat.ptr_focus);
    memset(&c->seat, 0, sizeof(c->seat));
}

void jwlc_seat_window_gone(struct jwl_client *c, const struct jwl_window *w)
{
    if (c->seat.kb_focus == w) {
        c->seat.kb_focus = NULL;
        c->seat.repeat_code = 0;
    }
    if (c->seat.ptr_focus == w)
        c->seat.ptr_focus = NULL;
}

/* ---- the keyboard ------------------------------------------------------------------------ */

static void key_event(struct jwl_client *c, uint32_t code, uint32_t state, uint32_t time)
{
    struct jwl_event ev = { .type = JWL_EV_KEY, .win = c->seat.kb_focus };
    ev.key.code = code;
    ev.key.state = state;
    ev.key.mods = c->seat.mods;
    ev.key.time = time;
    struct keymap_sym s;
    if (c->info.keymap && keymap_decode(c->info.keymap, code, c->seat.mods, &s)) {
        ev.key.sym = s.sym;
        ev.key.cp = s.cp;
    }
    jwlc_queue(c, &ev);
}

static bool repeats(const struct jwl_client *c, uint32_t code)
{
    const struct keymap *km = c->info.keymap;
    return km && code < KEYMAP_CODES && km->keys[code].repeats && c->info.repeat_rate > 0;
}

static status_t kb_keymap(void *data, uint32_t self, uint32_t format, handle_t fd, uint32_t size)
{
    struct jwl_client *c = data;
    (void)self;
    const struct keymap *km = NULL;
    char head[HEAD_MAX];
    uint32_t n = size < HEAD_MAX ? size : HEAD_MAX;
    if (format == JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && n && jam_vmo_read(fd, 0, head, n) == OK)
        km = keymap_of_xkb(head, n);
    jam_handle_close(fd);
    if (!km && format == JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        jwlc_log(c, "a keymap that names no layout of ours: using US");
    c->info.keymap = km ? km : &keymap_us;
    return OK;
}

static status_t kb_enter(void *data, uint32_t self, uint32_t serial, uint32_t surface,
                         const void *keys, uint32_t keys_size)
{
    struct jwl_client *c = data;
    (void)self, (void)keys, (void)keys_size;   /* keys held: not pressed here */
    c->seat.input_serial = serial;
    c->seat.kb_focus = jwlc_window_of_surface(c, surface);
    c->seat.repeat_code = 0;
    queue_simple(c, JWL_EV_KEYBOARD_ENTER, c->seat.kb_focus);
    return OK;
}

static status_t kb_leave(void *data, uint32_t self, uint32_t serial, uint32_t surface)
{
    struct jwl_client *c = data;
    (void)self, (void)serial, (void)surface;
    queue_simple(c, JWL_EV_KEYBOARD_LEAVE, c->seat.kb_focus);
    c->seat.kb_focus = NULL;
    c->seat.repeat_code = 0;
    return OK;
}

static status_t kb_key(void *data, uint32_t self, uint32_t serial, uint32_t time, uint32_t key,
                       uint32_t state)
{
    struct jwl_client *c = data;
    (void)self;
    bool pressed = state == JWL_WL_KEYBOARD_KEY_STATE_PRESSED;
    if (pressed)
        c->seat.input_serial = serial;
    key_event(c, key, pressed ? JWL_KEY_PRESSED : JWL_KEY_RELEASED, time);
    if (pressed && repeats(c, key)) {
        c->seat.repeat_code = key;
        c->seat.repeat_next = now() + (uint64_t)c->info.repeat_delay * NS_PER_MS;
    } else if (!pressed && key == c->seat.repeat_code) {
        c->seat.repeat_code = 0;
    }
    return OK;
}

static status_t kb_modifiers(void *data, uint32_t self, uint32_t serial, uint32_t depressed,
                             uint32_t latched, uint32_t locked, uint32_t group)
{
    struct jwl_client *c = data;
    (void)self, (void)serial, (void)group;
    c->seat.mods = depressed | latched | locked;
    struct jwl_event ev = { .type = JWL_EV_MODIFIERS, .win = c->seat.kb_focus };
    ev.key.mods = c->seat.mods;
    jwlc_queue(c, &ev);
    return OK;
}

static status_t kb_repeat_info(void *data, uint32_t self, int32_t rate, int32_t delay)
{
    struct jwl_client *c = data;
    (void)self;
    c->info.repeat_rate = rate > 0 ? rate : 0;
    c->info.repeat_delay = delay > 0 ? delay : 0;
    if (!c->info.repeat_rate)
        c->seat.repeat_code = 0;
    return OK;
}

uint64_t jwlc_seat_deadline(const struct jwl_client *c)
{
    return c->seat.repeat_code && c->seat.kb_focus ? c->seat.repeat_next : DEADLINE_NEVER;
}

void jwlc_seat_tick(struct jwl_client *c)
{
    uint64_t t = now();
    if (!c->seat.repeat_code || !c->seat.kb_focus || t < c->seat.repeat_next)
        return;
    key_event(c, c->seat.repeat_code, JWL_KEY_REPEATED, (uint32_t)(t / NS_PER_MS));
    uint64_t every = NS_PER_S / (uint64_t)c->info.repeat_rate;   /* rate > 0: repeats() */
    c->seat.repeat_next = t + every;
}

/* ---- the pointer ------------------------------------------------------------------------- */

static void pointer_event(struct jwl_client *c, uint32_t type, int32_t x, int32_t y)
{
    struct jwl_event ev = { .type = type, .win = c->seat.ptr_focus };
    ev.pointer.x = c->seat.ptr_x = x;
    ev.pointer.y = c->seat.ptr_y = y;
    jwlc_queue(c, &ev);
}

static status_t ptr_enter(void *data, uint32_t self, uint32_t serial, uint32_t surface, int32_t x,
                          int32_t y)
{
    struct jwl_client *c = data;
    (void)self;
    c->seat.ptr_focus = jwlc_window_of_surface(c, surface);
    c->seat.enter_serial = serial;
    pointer_event(c, JWL_EV_POINTER_ENTER, x, y);
    return send_shape(c);   /* the program's shape, from this enter on */
}

static status_t ptr_leave(void *data, uint32_t self, uint32_t serial, uint32_t surface)
{
    struct jwl_client *c = data;
    (void)self, (void)serial, (void)surface;
    queue_simple(c, JWL_EV_POINTER_LEAVE, c->seat.ptr_focus);
    c->seat.ptr_focus = NULL;
    return OK;
}

static status_t ptr_motion(void *data, uint32_t self, uint32_t time, int32_t x, int32_t y)
{
    (void)self, (void)time;
    pointer_event(data, JWL_EV_POINTER_MOTION, x, y);
    return OK;
}

static status_t ptr_button(void *data, uint32_t self, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    struct jwl_client *c = data;
    (void)self;
    bool pressed = state == JWL_WL_POINTER_BUTTON_STATE_PRESSED;
    if (pressed)
        c->seat.input_serial = serial;
    if (pressed && c->seat.ptr_focus)
        c->seat.ptr_focus->press_serial = serial;
    struct jwl_event ev = { .type = JWL_EV_POINTER_BUTTON, .win = c->seat.ptr_focus };
    ev.button.button = button;
    ev.button.pressed = pressed;
    ev.button.time = time;
    jwlc_queue(c, &ev);
    return OK;
}

static status_t ptr_axis(void *data, uint32_t self, uint32_t time, uint32_t axis, int32_t value)
{
    struct jwl_client *c = data;
    (void)self, (void)time;
    struct jwl_event ev = { .type = JWL_EV_POINTER_AXIS, .win = c->seat.ptr_focus };
    ev.axis.axis = axis;
    ev.axis.value = value;
    if (axis < 2) {
        ev.axis.discrete = c->seat.discrete[axis];
        c->seat.discrete[axis] = 0;
    }
    jwlc_queue(c, &ev);
    return OK;
}

static status_t ptr_axis_discrete(void *data, uint32_t self, uint32_t axis, int32_t discrete)
{
    struct jwl_client *c = data;
    (void)self;
    if (axis < 2)
        c->seat.discrete[axis] = discrete;
    return OK;
}

static status_t ptr_frame(void *data, uint32_t self)
{
    (void)data, (void)self;
    return OK;   /* each event is queued as it comes */
}

static status_t ptr_axis_source(void *data, uint32_t self, uint32_t source)
{
    (void)data, (void)self, (void)source;
    return OK;
}

static status_t ptr_axis_stop(void *data, uint32_t self, uint32_t time, uint32_t axis)
{
    (void)data, (void)self, (void)time, (void)axis;
    return OK;
}

static const struct jwl_wl_keyboard_events kb_events = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
    .modifiers = kb_modifiers, .repeat_info = kb_repeat_info,
};
static const struct jwl_wl_pointer_events ptr_events = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
};

status_t jwlc_seat_event(struct jwl_client *c, struct jwl_msg *m)
{
    if (m->iface == &jwl_wl_keyboard_interface)
        return jwl_wl_keyboard_dispatch_event(&kb_events, c, m->id, m->opcode, m->args);
    return jwl_wl_pointer_dispatch_event(&ptr_events, c, m->id, m->opcode, m->args);
}
