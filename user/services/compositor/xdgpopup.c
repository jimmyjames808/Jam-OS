/* xdg_popup (xdg.h, comp.h): menus, tooltips and <select> lists. A popup
 * is placed by its positioner's rules (xdgplace.c) against its parent's
 * window geometry, and shown as a window of its own (COMP_WIN_POPUP: no
 * decorations, never focused, tiled or cycled) above its parent.
 *
 * The sequence, as for a toplevel (xdgtop.c): get_popup, the initial
 * commit with no buffer (its parent must be shown: else it is dismissed),
 * the configure (xdg_popup.configure with the place, xdg_surface.configure
 * with a serial), the ack, then a commit with a buffer shows it at the
 * place it acked. A commit with no buffer hides it again (and takes it out
 * of the grab); reposition (version 3) places it by new rules: repositioned,
 * then a configure, as the protocol says.
 *
 * Following the parent. Each loop turn before the paint (xdg_popups_turn)
 * every shown popup is moved with its parent (a window manager's move, a
 * tile's glide, a screen's slide), kept above it (a click raises the
 * parent), and a reactive one is placed again if its parent's room
 * changed. A popup is dismissed (popup_done, and hidden) when its parent
 * is unmapped or goes, or its parent's role object does; the popups on it
 * go with it, the newest first, the order the protocol asks clients to
 * destroy them in.
 *
 * The grab (xdg_popup.grab). Granted for one of the last input events'
 * serials of the client (seat_input_serial_ok: a key, a button, a keyboard
 * enter) while the popup's toplevel has the keyboard focus, before the
 * popup is shown; else the popup is dismissed at once, as the protocol
 * allows. Once we dismiss a client's grab (a press outside, the focus
 * moving, another client's grab), it may grab again only after its next
 * key or button press (seat_grab_lock): its old serials, and the enter the
 * keys coming back give it, would let it meet every click outside with a
 * new grab, and the mouse could never leave it (docs/G2-REVIEW.md, 1).
 * The grabbing popups form one chain, all of one client's, each
 * placed on the one before (a menu, its submenu): a new grab on a toplevel
 * dismisses the chain there was, one on a popup of the chain dismisses
 * what is above that popup. While the chain lives:
 *   - the pointer goes only to that client's surfaces (pointer.c: the
 *     protocol's owner-events), and a press anywhere else (another window,
 *     a title bar, the background, the strip) dismisses the whole chain
 *     and goes nowhere;
 *   - the keys go to the topmost shown popup of the chain (xdg_key_surface:
 *     wl_keyboard.leave on the surface that had them, enter on the popup),
 *     and back down the chain to the toplevel as popups go; the toplevel
 *     keeps the focus (its title bar lit, activated) all along;
 *   - the focus moving to another window (Alt+Tab, a screen switched)
 *     dismisses the chain.
 * Escape is the client's: the keys reach the popup, and it closes itself.
 *
 * Caps: XDG_POPUPS_MAX popups a client, nested at most XDG_POPUP_DEPTH_MAX
 * deep (no_memory past either, as every cap). */
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include "seat.h"
#include "xdg.h"

static const struct comp_role_ops popup_role;

static struct {
    struct xdg_surf *first, *last;      /* every popup whose role object lives, oldest first */
    struct xdg_surf *chain[XDG_POPUP_DEPTH_MAX];   /* the grab: each on the one before, the
                                                    * topmost last; all one client's */
    uint32_t nchain;
} pops;

/* ---- who is whose ------------------------------------------------------------------- */

static struct comp_window *win_of(const struct xdg_surf *x)
{
    return x && x->surface ? x->surface->window : NULL;
}

/* The popup a window shows, or NULL. */
static struct xdg_surf *popup_of(const struct comp_window *w)
{
    if (!w || !(w->flags & COMP_WIN_POPUP) || w->surface->role_ops != &popup_role)
        return NULL;
    return w->surface->role_data;
}

/* The toplevel x is placed on, through its parents; NULL if one went. */
static struct xdg_surf *root_of(struct xdg_surf *x)
{
    for (unsigned d = 0; x && d <= XDG_POPUP_DEPTH_MAX; d++) {
        if (x->role != XDG_ROLE_POPUP)
            return x->role == XDG_ROLE_TOPLEVEL ? x : NULL;
        x = x->pop.parent;
    }
    return NULL;
}

/* Is x y, or placed on y through popups? */
static bool placed_on(const struct xdg_surf *x, const struct xdg_surf *y)
{
    for (unsigned d = 0; x && d <= XDG_POPUP_DEPTH_MAX; d++) {
        if (x == y)
            return true;
        x = x->role == XDG_ROLE_POPUP ? x->pop.parent : NULL;
    }
    return false;
}

static bool shown(const struct comp_window *w)
{
    return w && (w->flags & COMP_WIN_MAPPED);
}

struct comp_window *xdg_popup_root(struct comp_window *w)
{
    struct xdg_surf *x = popup_of(w);
    if (!x)
        return w;
    struct comp_window *r = win_of(root_of(x));
    return shown(r) ? r : NULL;
}

static bool listed(const struct xdg_surf *x)
{
    return x->role == XDG_ROLE_POPUP && (pops.first == x || x->pop.prev);
}

/* ---- the grab's chain ------------------------------------------------------------------ */

static bool in_chain(const struct xdg_surf *x)
{
    for (uint32_t i = 0; i < pops.nchain; i++)
        if (pops.chain[i] == x)
            return true;
    return false;
}

static void chain_drop(const struct xdg_surf *x)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pops.nchain; i++)
        if (pops.chain[i] != x)
            pops.chain[n++] = pops.chain[i];
    pops.nchain = n;
}

struct comp_client *xdg_grab_client(void)
{
    return pops.nchain ? pops.chain[0]->cl : NULL;
}

struct comp_surface *xdg_key_surface(struct comp_window *w)
{
    for (uint32_t i = pops.nchain; i-- > 0;) {
        struct comp_window *pw = win_of(pops.chain[i]);
        if (shown(pw) && xdg_popup_root(pw) == w)
            return pw->surface;
    }
    return w->surface;
}

/* ---- dismissing ------------------------------------------------------------------------ */

/* x alone: popup_done, out of the grab, hidden. Its window going tells
 * the seat (its pointer focus) and dismisses what is on x
 * (xdg_popups_window_gone), if anything still is. */
static void dismiss_one(struct xdg_surf *x)
{
    if (x->pop.dismissed)
        return;
    x->pop.dismissed = true;
    x->dirty = false;
    chain_drop(x);
    if (x->role_id && client_alive(x->cl))
        (void)jwl_xdg_popup_send_popup_done(x->cl->conn, x->role_id);   /* dead: torn down */
    struct comp_window *w = win_of(x);
    if (w)
        window_destroy(w);
}

/* x and every popup on it, the newest first. The list keeps its order and
 * its members meanwhile (only windows go), so the walk is safe whatever
 * dismissing one does. */
static void dismiss(struct xdg_surf *x)
{
    for (struct xdg_surf *y = pops.last, *before; y; y = before) {
        before = y->pop.prev;
        if (!y->pop.dismissed && placed_on(y, x))
            dismiss_one(y);
    }
}

void xdg_grab_dismiss(void)
{
    if (!pops.nchain)
        return;
    seat_grab_lock(pops.chain[0]->cl);   /* no grab again before its next press */
    dismiss(pops.chain[0]);
}

void xdg_popups_focus(struct comp_window *w)
{
    if (pops.nchain && win_of(pops.chain[0]->pop.parent) != w)
        xdg_grab_dismiss();   /* the keys go to another window: the menus close */
}

void xdg_popups_window_gone(struct comp_window *w)
{
    for (struct xdg_surf *y = pops.last, *before; y; y = before) {
        before = y->pop.prev;
        if (!y->pop.dismissed && y->pop.parent && win_of(y->pop.parent) == w)
            dismiss(y);
    }
}

void xdg_popup_detach(struct xdg_surf *x)
{
    for (struct xdg_surf *y = pops.last, *before; y; y = before) {
        before = y->pop.prev;
        if (y->pop.parent != x)
            continue;
        dismiss(y);
        y->pop.parent = NULL;   /* x may be freed next */
    }
    if (!listed(x))
        return;
    dismiss_one(x);   /* hidden, out of the grab (popup_done only to a role object
                       * that stays: its wl_surface went first, a protocol error) */
    if (x->pop.prev)
        x->pop.prev->pop.next = x->pop.next;
    else
        pops.first = x->pop.next;
    if (x->pop.next)
        x->pop.next->pop.prev = x->pop.prev;
    else
        pops.last = x->pop.prev;
    x->pop.prev = x->pop.next = NULL;
    x->xc->npopups--;
}

/* ---- placing --------------------------------------------------------------------------- */

/* Where x's parent's window geometry is shown on the output (*ox, *oy),
 * and the room x may use, in the parent's coordinates as it rests (a
 * slide or a glide moving it changes no place): the output below the
 * strip (its toplevel's floor), all of it for a full-screen toplevel.
 * False: the parent isn't shown. */
static bool parent_frame(struct xdg_surf *x, int32_t *ox, int32_t *oy, struct comp_box *room)
{
    struct comp_window *pw = win_of(x->pop.parent);
    if (!shown(pw))
        return false;
    struct comp_box g = window_geom(pw), r = { 0, 0, scene.width, scene.height };
    *ox = window_shown_x(pw) + g.x1;
    *oy = window_shown_y(pw) + g.y1;
    struct xdg_surf *root = root_of(x);
    if (root && root->ww && !(root->ww->shown & WM_ST_FULLSCREEN))
        r.y1 = wm_floor(root->ww);
    *room = box_translate(r, -(pw->x + g.x1), -(pw->y + g.y1));
    return true;
}

/* x's place now, by its rules (anywhere if its parent isn't shown). */
static struct comp_box place_now(struct xdg_surf *x)
{
    int32_t ox, oy;
    struct comp_box room;
    if (!parent_frame(x, &ox, &oy, &room))
        room = (struct comp_box){ -COMP_COORD_MAX, -COMP_COORD_MAX, COMP_COORD_MAX,
                                  COMP_COORD_MAX };
    return xdg_place(&x->pop.rules, room);
}

static bool same_box(struct comp_box a, struct comp_box b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

/* repositioned (if owed), xdg_popup.configure and xdg_surface.configure. */
static void send(struct xdg_surf *x, struct comp_box p)
{
    struct jwl_conn *c = x->cl->conn;
    const struct jwl_object *o = jwl_conn_object(c, x->role_id);
    uint32_t serial = comp_serial();
    status_t st = OK;
    if (x->pop.repositioned && o && o->version >= JWL_XDG_POPUP_EV_REPOSITIONED_SINCE)
        st = jwl_xdg_popup_send_repositioned(c, x->role_id, x->pop.token);
    if (st == OK)
        st = jwl_xdg_popup_send_configure(c, x->role_id, p.x1, p.y1, p.x2 - p.x1, p.y2 - p.y1);
    if (st == OK)
        st = jwl_xdg_surface_send_configure(c, x->id, serial);
    if (st != OK)
        return;   /* a dead connection: torn down after this turn */
    x->pop.repositioned = false;
    x->sent[x->nsent++] = (struct xdg_sent){ .serial = serial, .place = p };
    x->pop.place = p;
    x->dirty = false;
}

void xdg_popup_configure(struct xdg_surf *x)
{
    if (x->pop.dismissed || !x->role_id)
        return;
    if (x->nsent == XDG_CONFIGS_MAX) {
        x->dirty = true;   /* sent when an ack makes room (xdgtop.c) */
        return;
    }
    send(x, place_now(x));
}

/* x's window moved to its acked place against its parent's geometry at
 * (ox, oy). */
static void move_to(struct xdg_surf *x, int32_t ox, int32_t oy)
{
    struct comp_window *w = win_of(x);
    struct comp_box g = window_geom(w);
    window_move(w, ox + x->pop.shown.x1 - g.x1, oy + x->pop.shown.y1 - g.y1);
}

/* The topmost window of x's family (its toplevel's window and every popup
 * on that toplevel), x's own aside. */
static struct comp_window *family_top(struct xdg_surf *x)
{
    struct xdg_surf *root = root_of(x);
    struct comp_window *rw = win_of(root), *self = win_of(x);
    for (struct comp_window *w = scene.top; w; w = w->below) {
        if (w == self)
            continue;
        struct xdg_surf *p = popup_of(w);
        if (w == rw || (p && root_of(p) == root))
            return w;
    }
    return rw;
}

/* Is w above `under` in the stacking order? */
static bool above(const struct comp_window *w, const struct comp_window *under)
{
    for (const struct comp_window *a = under ? under->above : NULL; a; a = a->above)
        if (a == w)
            return true;
    return false;
}

/* x's window over its family, so over its parent and over the popups
 * shown on its toplevel before it. */
static void stack(struct xdg_surf *x)
{
    struct comp_window *top = family_top(x);
    if (top)
        window_raise_above(win_of(x), top);
}

/* ---- the role: commits ---------------------------------------------------------------- */

/* The initial commit: placed, the first configure sent. */
static status_t initial_commit(struct xdg_surf *x)
{
    struct comp_surface *s = x->surface;
    if (s->buffer)
        return comp_error(x->cl, x->id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "a buffer on xdg_surface %u before its first configure", x->id);
    if (!x->pop.parent)
        return comp_error(x->cl, x->base->id, JWL_XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                          "xdg_popup %u has no parent", x->role_id);
    if (!shown(win_of(x->pop.parent))) {
        dismiss(x);   /* "the parent of an xdg_popup must be mapped before" it */
        return OK;
    }
    x->initial = true;
    x->configured = x->acked_new = false;
    if (x->nsent == XDG_CONFIGS_MAX) {   /* the client must hear this one: the oldest goes */
        x->nsent--;
        memmove(x->sent, x->sent + 1, x->nsent * sizeof(x->sent[0]));
    }
    xdg_popup_configure(x);
    return OK;
}

/* A commit with no buffer: hidden, out of the grab, and back to before its
 * initial commit. What is on it goes with its window. */
static void hide(struct xdg_surf *x)
{
    struct comp_window *w = win_of(x);
    bool grabbed = in_chain(x);
    chain_drop(x);
    x->pop.grab = false;
    if (w)
        window_destroy(w);
    x->initial = x->configured = x->acked_new = x->dirty = false;
    for (uint32_t i = 0; i < x->nsent; i++)
        x->sent[i].stale = true;
    if (grabbed)
        seat_key_surface_changed();   /* the keys back down the chain */
}

/* A commit with a buffer, configured: shown at its acked place. */
static status_t show(struct xdg_surf *x)
{
    int32_t ox, oy;
    struct comp_box room;
    if (!parent_frame(x, &ox, &oy, &room)) {
        dismiss(x);
        return OK;
    }
    struct comp_window *w = win_of(x);
    if (w) {
        window_set_geom(w, x->geom);
        move_to(x, ox, oy);
        return OK;
    }
    if (window_create(x->surface, 0, 0, &w) != OK)
        return comp_no_memory(x->cl, x->role_id, "no memory for a popup's window");
    w->flags |= COMP_WIN_POPUP;
    w->geom = x->geom;   /* (not mapped yet: nothing to damage) */
    move_to(x, ox, oy);
    stack(x);
    window_map(w, true);
    if (in_chain(x))
        seat_key_surface_changed();   /* the keys to it */
    return OK;
}

static status_t popup_commit(struct comp_surface *s)
{
    struct xdg_surf *x = s->role_data;
    if (x->geom_set)
        x->geom = x->geom_pending;
    x->geom_set = false;
    if (x->pop.dismissed)
        return OK;   /* shown nowhere until the client destroys it */
    if (!x->initial)
        return initial_commit(x);
    if (!s->buffer) {
        hide(x);
        return OK;
    }
    if (!x->configured)
        return comp_error(s->client, x->id, JWL_XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                          "a buffer on xdg_surface %u before a configure was acked", x->id);
    if (x->acked_new)
        x->pop.shown = x->acked_place;
    x->acked_new = false;
    return show(x);
}

/* The wl_surface went with its popup alive: wayland.xml's
 * defunct_role_object, unless the whole client went. */
static void popup_gone(struct comp_surface *s, bool dead)
{
    struct xdg_surf *x = s->role_data;
    xdg_popup_detach(x);
    x->surface = NULL;
    if (!dead)
        (void)comp_error(x->cl, s->id, JWL_WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                         "wl_surface %u destroyed before its xdg_popup", s->id);
}

static const struct comp_role_ops popup_role = { "xdg_popup", popup_commit, popup_gone };

status_t xdg_popup_create(struct xdg_surf *x, uint32_t id, uint32_t parent, uint32_t positioner)
{
    struct comp_client *cl = x->cl;
    struct xdg_client *xc = x->xc;
    const struct xdg_rules *r = xdg_rules_of(x, positioner);
    if (!r)
        return ERR_INVALID_ARGS;
    struct xdg_surf *px = parent ? comp_object(cl, parent, &jwl_xdg_surface_interface) : NULL;
    if (parent && (!px || px == x || !px->surface || !px->role_id))
        return comp_error(cl, x->base->id, JWL_XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                          "xdg_surface %u can't be a popup's parent", parent);
    uint32_t depth = px && px->role == XDG_ROLE_POPUP ? px->pop.depth + 1 : 1;
    if (depth > XDG_POPUP_DEPTH_MAX)
        return comp_no_memory(cl, id, "popups nested too deep (8)");
    if (xc->npopups >= XDG_POPUPS_MAX)
        return comp_no_memory(cl, id, "too many xdg_popups (32)");
    x->constructed = true;
    x->role = XDG_ROLE_POPUP;
    x->role_id = id;
    x->surface->role = COMP_ROLE_XDG_POPUP;
    x->surface->role_ops = &popup_role;
    x->pop = (struct xdg_popup_state){ .parent = px, .rules = *r, .depth = depth,
                                       .prev = pops.last };
    if (pops.last)
        pops.last->pop.next = x;
    else
        pops.first = x;
    pops.last = x;
    xc->npopups++;
    return jwl_map_set_data(&cl->conn->map, id, x);
}

/* ---- xdg_popup's requests -------------------------------------------------------------- */

/* Is a popup that isn't dismissed placed on x? */
static bool has_child(const struct xdg_surf *x)
{
    for (const struct xdg_surf *y = pops.first; y; y = y->pop.next)
        if (y->pop.parent == x && !y->pop.dismissed)
            return true;
    return false;
}

static status_t on_popup_destroy(void *data, uint32_t self)
{
    struct xdg_surf *x = data;
    if (x->surface && has_child(x))
        return comp_error(x->cl, x->base->id, JWL_XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                          "xdg_popup %u destroyed under another popup", self);
    bool grabbed = in_chain(x);
    x->role_id = 0;   /* libjwl freed the id: no popup_done to it */
    xdg_popup_detach(x);
    x->initial = x->configured = x->acked_new = x->dirty = false;
    x->nsent = 0;
    if (x->surface)
        x->surface->role_ops = &xdg_bare_ops;   /* its role stays a popup's */
    if (grabbed)
        seat_key_surface_changed();   /* the keys back to the popup below, or the toplevel */
    return OK;
}

/* May x take the grab: a serial of one of its client's last input events,
 * no grab of its dismissed by us since its last press, and its toplevel has
 * the keys (no grab for a window in the background). */
static bool grant(struct xdg_surf *x, uint32_t serial)
{
    struct comp_window *rw = win_of(root_of(x));
    return shown(rw) && seat_focused() == rw && seat_grab_serial_ok(x->cl, serial);
}

/* x (granted) into the chain: another client's grab dismissed, and every
 * popup of the chain x isn't placed on. */
static void join(struct xdg_surf *x)
{
    if (pops.nchain && pops.chain[0]->cl != x->cl)
        xdg_grab_dismiss();
    while (pops.nchain && !placed_on(x, pops.chain[pops.nchain - 1]))
        dismiss(pops.chain[pops.nchain - 1]);
    if (pops.nchain < XDG_POPUP_DEPTH_MAX)   /* (always: each is nested in the one before) */
        pops.chain[pops.nchain++] = x;
}

static status_t on_popup_grab(void *data, uint32_t self, uint32_t seat, uint32_t serial)
{
    struct xdg_surf *x = data;
    struct xdg_surf *px = x->pop.parent;
    (void)seat;   /* one seat: libjwl checked it names a wl_seat */
    if (!x->surface || x->pop.dismissed || x->pop.grab)
        return OK;
    if (win_of(x))
        return comp_error(x->cl, self, JWL_XDG_POPUP_ERROR_INVALID_GRAB,
                          "xdg_popup %u grabbed after it was mapped", self);
    if (px && px->role == XDG_ROLE_POPUP && !px->pop.grab)
        return comp_error(x->cl, self, JWL_XDG_POPUP_ERROR_INVALID_GRAB,
                          "xdg_popup %u grabbed on a popup that took no grab", self);
    x->pop.grab = true;
    bool nested_ok = !px || px->role != XDG_ROLE_POPUP || in_chain(px);
    if (!px || px->pop.dismissed || !nested_ok || !grant(x, serial))
        dismiss(x);   /* denied: dismissed at once, as the protocol says */
    else
        join(x);
    return OK;
}

static status_t on_popup_reposition(void *data, uint32_t self, uint32_t positioner,
                                    uint32_t token)
{
    struct xdg_surf *x = data;
    (void)self;
    if (!x->surface)
        return OK;
    const struct xdg_rules *r = xdg_rules_of(x, positioner);
    if (!r)
        return ERR_INVALID_ARGS;
    if (x->pop.dismissed)
        return OK;
    x->pop.rules = *r;
    x->pop.token = token;
    x->pop.repositioned = true;
    if (x->initial)
        xdg_popup_configure(x);   /* else the initial commit's says it */
    return OK;
}

static const struct jwl_xdg_popup_requests popup_ops = {
    .destroy = on_popup_destroy,
    .grab = on_popup_grab,
    .reposition = on_popup_reposition,
};

status_t xdg_popup_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_xdg_popup_dispatch_request(&popup_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- each turn ------------------------------------------------------------------------- */

void xdg_popups_turn(void)
{
    for (struct xdg_surf *x = pops.first; x; x = x->pop.next) {   /* parents first */
        struct comp_window *w = win_of(x);
        int32_t ox, oy;
        struct comp_box room;
        if (!w || x->pop.dismissed)
            continue;
        if (!parent_frame(x, &ox, &oy, &room)) {
            dismiss(x);   /* (its parent's window going dismissed it already) */
            continue;
        }
        if (x->pop.rules.reactive && x->initial && !x->dirty &&
            !same_box(xdg_place(&x->pop.rules, room), x->pop.place))
            xdg_popup_configure(x);   /* moved there once the client acks and draws */
        move_to(x, ox, oy);
        if (!above(w, win_of(x->pop.parent)))
            stack(x);
    }
    seat_key_surface_changed();
}
