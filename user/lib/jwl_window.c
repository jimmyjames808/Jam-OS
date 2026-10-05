/* libjwl's client: windows (<jwl_client.h>), a wl_surface with the
 * xdg_toplevel role and two buffers of its own.
 *
 * The configure/ack loop, as xdg-shell has it: the compositor sends
 * xdg_toplevel.configure (a size, 0 for "yours", and states) and then
 * xdg_surface.configure (a serial); the client acks the serial with the
 * commit that shows that state. So a configure is applied here (the size
 * to draw at follows the rules below) and the program hears
 * JWL_EV_CONFIGURE; the ack goes with its next jwl_window_present. A
 * configure that changes nothing the buffer shows (a state such as
 * activated) is acked and committed at once, so the compositor never
 * waits on a program that has no reason to draw.
 *
 * The size to draw at: the compositor's when the window takes sizes (it
 * is resizable, maximised or full screen) and it named one; the size so
 * far when it said 0; the program's own size for a window that doesn't
 * take sizes. Within 1 to JWL_SIZE_MAX.
 *
 * Buffers: two slots of one pool of the window's, each a whole number of
 * pages, slot i at i * slot_bytes. A slot's buffer is made when first
 * handed out at a size; a new size destroys both and grows the pool if it
 * must (the compositor may show the old picture's pixels being
 * overwritten for that one frame).
 *
 * After a reconnect the window is made again (title, sizes, states) and
 * its first configure shows its last buffer again with all of it damaged,
 * so the window comes back as it was without the program drawing; a
 * frame callback lost with the old connection is answered then too. */
#include <jwl_client.h>
#include <os.h>
#include "jwlc.h"

#define TAKES_SIZES (JWL_STATE(JWL_XDG_TOPLEVEL_STATE_MAXIMIZED) | \
                     JWL_STATE(JWL_XDG_TOPLEVEL_STATE_FULLSCREEN))

/* src into dst (JWL_TEXT_MAX bytes), cut at a character boundary. */
static void copy_text(char *dst, const char *src)
{
    size_t n = src ? strnlen(src, JWL_TEXT_MAX - 1) : 0;
    if (src && n == JWL_TEXT_MAX - 1)
        while (n && ((uint8_t)src[n] & 0xc0) == 0x80)
            n--;   /* src[n], the first byte left out, continues a character */
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

static int32_t clamp(int32_t v)
{
    return v < 1 ? 1 : v > JWL_SIZE_MAX ? JWL_SIZE_MAX : v;
}

/* ---- making it on a connection --------------------------------------------------------- */

/* Title, app id, size limits and states, as the program asked. */
static status_t tell_state(struct jwl_window *w)
{
    struct jwl_conn *k = w->c->conn;
    status_t st = OK;
    if (w->title[0])
        st = jwl_xdg_toplevel_set_title(k, w->toplevel, w->title);
    if (st == OK && w->app_id[0])
        st = jwl_xdg_toplevel_set_app_id(k, w->toplevel, w->app_id);
    if (st == OK && !w->resizable)
        st = jwl_xdg_toplevel_set_min_size(k, w->toplevel, w->want_w, w->want_h);
    if (st == OK && !w->resizable)
        st = jwl_xdg_toplevel_set_max_size(k, w->toplevel, w->want_w, w->want_h);
    if (st == OK && w->fullscreen)
        st = jwl_xdg_toplevel_set_fullscreen(k, w->toplevel, 0);
    if (st == OK && w->maximized)
        st = jwl_xdg_toplevel_set_maximized(k, w->toplevel);
    return st;
}

status_t jwlc_window_make(struct jwl_window *w)
{
    struct jwl_client *c = w->c;
    if (!jwlc_live(c))
        return OK;   /* made when the client is ready */
    if (!c->global[JWLC_WM_BASE])
        return ERR_NOT_SUPPORTED;   /* no xdg_wm_base: no windows */
    struct jwl_conn *k = c->conn;
    uint32_t sid = 0, xid = 0, tid = 0;
    status_t st = jwlc_make(c, &jwl_wl_surface_interface, c->info.compositor_version, w, &sid);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_compositor_create_surface(k, c->global[JWLC_COMPOSITOR], sid),
                       sid);
    if (st == OK)
        w->surface = sid;
    if (st == OK)
        st = jwlc_make(c, &jwl_xdg_surface_interface, c->info.wm_base_version, w, &xid);
    if (st == OK)
        st = jwlc_made(c, jwl_xdg_wm_base_get_xdg_surface(k, c->global[JWLC_WM_BASE], xid, sid),
                       xid);
    if (st == OK)
        w->xdg_surface = xid;
    if (st == OK)
        st = jwlc_make(c, &jwl_xdg_toplevel_interface, c->info.wm_base_version, w, &tid);
    if (st == OK)
        st = jwlc_made(c, jwl_xdg_surface_get_toplevel(k, xid, tid), tid);
    if (st == OK)
        w->toplevel = tid;
    if (st == OK)
        st = tell_state(w);
    if (st == OK)
        st = jwl_wl_surface_commit(k, sid);   /* no buffer: asks for the first configure */
    return st;
}

status_t jwl_window_create(struct jwl_client *c, const struct jwl_window_config *cfg,
                           struct jwl_window **out)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    if (cfg->width < 1 || cfg->height < 1 || cfg->width > JWL_SIZE_MAX ||
        cfg->height > JWL_SIZE_MAX)
        return ERR_INVALID_ARGS;
    struct jwl_window *w = calloc(1, sizeof(*w));
    if (!w)
        return ERR_NO_MEMORY;
    w->c = c;
    copy_text(w->title, cfg->title);
    copy_text(w->app_id, cfg->app_id);
    w->want_w = w->width = cfg->width;
    w->want_h = w->height = cfg->height;
    w->resizable = cfg->resizable;
    w->alpha = cfg->alpha;
    w->fullscreen = cfg->fullscreen;
    w->maximized = cfg->maximized;
    w->began = w->shown = -1;
    w->frame.win = w;
    w->next = c->windows;
    c->windows = w;
    status_t st = jwlc_window_make(w);
    if (st != OK && c->conn && c->conn->status == OK) {
        jwl_window_destroy(w);   /* refused by us, not lost with the connection */
        return st;
    }
    *out = w;
    return OK;
}

/* Forget the window's ids (destroyed, or gone with the connection). */
static void drop_ids(struct jwl_window *w)
{
    w->surface = w->xdg_surface = w->toplevel = 0;
    w->configured = w->ack_due = false;
    w->press_serial = 0;
}

void jwl_window_destroy(struct jwl_window *w)
{
    if (!w)
        return;
    struct jwl_client *c = w->c;
    struct jwl_conn *k = c->conn;
    if (k && w->frame.id)
        (void)jwl_map_set_data(&k->map, w->frame.id, NULL);   /* its done finds no window */
    if (k && w->toplevel)
        (void)jwl_xdg_toplevel_destroy(k, w->toplevel);   /* role object before the surface */
    if (k && w->xdg_surface)
        (void)jwl_xdg_surface_destroy(k, w->xdg_surface);
    for (unsigned i = 0; i < 2; i++)
        jwl_buffer_destroy(w->buf[i]);
    jwl_pool_destroy(w->pool);
    if (k && w->surface)
        (void)jwl_wl_surface_destroy(k, w->surface);
    if (k)
        (void)jwl_conn_flush(k);
    jwlc_seat_window_gone(c, w);
    jwlc_unqueue(c, w);
    for (struct jwl_window **pw = &c->windows; *pw; pw = &(*pw)->next)
        if (*pw == w) {
            *pw = w->next;
            break;
        }
    free(w);
}

void jwlc_windows_lost(struct jwl_client *c)
{
    for (struct jwl_window *w = c->windows; w; w = w->next) {
        drop_ids(w);
        w->rebuilt = true;
        if (w->frame.id)
            w->frame_lost = true;
        w->frame.id = 0;
    }
}

struct jwl_window *jwlc_window_of_surface(struct jwl_client *c, uint32_t id)
{
    for (struct jwl_window *w = c->windows; w && id; w = w->next)
        if (w->surface == id)
            return w;
    return NULL;
}

/* ---- buffers --------------------------------------------------------------------------- */

static void drop_buffers(struct jwl_window *w)
{
    for (unsigned i = 0; i < 2; i++) {
        jwl_buffer_destroy(w->buf[i]);
        w->buf[i] = NULL;
    }
    w->began = w->shown = -1;
}

/* Slot i's buffer at the window's size, made if it isn't. */
static status_t slot_buffer(struct jwl_window *w, unsigned i)
{
    if (w->buf[i])
        return OK;
    int32_t stride = w->width * 4;
    uint64_t slot = ((uint64_t)stride * (uint64_t)w->height + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    status_t st = OK;
    if (!w->pool)
        st = jwl_pool_create(w->c, 2 * slot, &w->pool);
    else if (jwl_pool_size(w->pool) < 2 * slot)
        st = jwl_pool_grow(w->pool, 2 * slot);
    if (st != OK)
        return st;
    w->slot_bytes = slot;
    uint32_t fmt = w->alpha ? JWL_WL_SHM_FORMAT_ARGB8888 : JWL_WL_SHM_FORMAT_XRGB8888;
    return jwl_buffer_create(w->pool, i * slot, w->width, w->height, stride, fmt, &w->buf[i]);
}

static void frame_of(const struct jwl_window *w, unsigned i, struct jwl_frame *out)
{
    out->px = jwl_buffer_pixels(w->buf[i]);
    out->width = w->width;
    out->height = w->height;
    out->stride = w->width * 4;
    out->slot = i;
}

status_t jwl_window_begin(struct jwl_window *w, struct jwl_frame *out)
{
    if (!w->configured)
        return ERR_BAD_STATE;
    if (w->buf[0] && (w->buf[0]->width != w->width || w->buf[0]->height != w->height))
        drop_buffers(w);
    if (w->buf[1] && (w->buf[1]->width != w->width || w->buf[1]->height != w->height))
        drop_buffers(w);
    int pick = -1;
    for (int i = 0; i < 2; i++) {
        bool free_slot = !w->buf[i] || !w->buf[i]->busy;
        if (free_slot && (pick < 0 || pick == w->shown))
            pick = i;   /* the one not shown, when both are free */
    }
    if (pick < 0)
        return ERR_SHOULD_WAIT;
    status_t st = slot_buffer(w, (unsigned)pick);
    if (st != OK)
        return st;
    w->began = pick;
    frame_of(w, (unsigned)pick, out);
    return OK;
}

status_t jwl_window_slot(struct jwl_window *w, unsigned slot, struct jwl_frame *out)
{
    if (slot > 1 || !w->buf[slot] || w->buf[slot]->width != w->width ||
        w->buf[slot]->height != w->height)
        return ERR_BAD_STATE;
    frame_of(w, slot, out);
    return OK;
}

/* ---- presenting ------------------------------------------------------------------------ */

static status_t damage(struct jwl_window *w, const struct jwl_rect *rects, unsigned n)
{
    struct jwl_rect all = { 0, 0, w->width, w->height };
    if (!rects) {
        rects = &all;
        n = 1;
    }
    struct jwl_conn *k = w->c->conn;
    status_t st = OK;
    for (unsigned i = 0; i < n && st == OK; i++) {
        const struct jwl_rect *r = &rects[i];
        if (r->w <= 0 || r->h <= 0)
            continue;
        /* the same numbers either way: the scale is 1 and the transform normal */
        if (w->c->info.compositor_version >= JWL_WL_SURFACE_REQ_DAMAGE_BUFFER_SINCE)
            st = jwl_wl_surface_damage_buffer(k, w->surface, r->x, r->y, r->w, r->h);
        else
            st = jwl_wl_surface_damage(k, w->surface, r->x, r->y, r->w, r->h);
    }
    return st;
}

static status_t ask_frame(struct jwl_window *w)
{
    if (w->frame.id)
        return OK;   /* one is waiting: it comes with the next paint anyway */
    uint32_t id;   /* a child of the surface: at the surface's version */
    status_t st = jwlc_make(w->c, &jwl_wl_callback_interface, w->c->info.compositor_version,
                            &w->frame, &id);
    if (st == OK)
        st = jwlc_made(w->c, jwl_wl_surface_frame(w->c->conn, w->surface, id), id);
    if (st == OK) {
        w->frame.id = id;
        w->frame.done = false;
    }
    return st;
}

/* Attach slot's buffer, damage, maybe a frame callback, commit. */
static status_t show(struct jwl_window *w, unsigned slot, const struct jwl_rect *rects, unsigned n,
                     bool frame)
{
    struct jwl_conn *k = w->c->conn;
    status_t st = OK;
    if (w->ack_due)
        st = jwl_xdg_surface_ack_configure(k, w->xdg_surface, w->ack_serial);
    w->ack_due = false;
    if (st == OK)
        st = jwl_wl_surface_attach(k, w->surface, w->buf[slot]->id, 0, 0);
    if (st == OK)
        st = damage(w, rects, n);
    if (st == OK && frame)
        st = ask_frame(w);
    if (st == OK)
        st = jwl_wl_surface_commit(k, w->surface);
    if (st == OK) {
        w->buf[slot]->busy = true;
        w->shown = (int)slot;
        st = jwl_conn_flush(k);
    }
    return st;
}

status_t jwl_window_present(struct jwl_window *w, const struct jwl_rect *rects, unsigned n,
                            bool frame)
{
    struct jwl_client *c = w->c;
    if (c->state == JWLC_DEAD)
        return c->why;
    if (w->began < 0)
        return ERR_BAD_STATE;
    if (!jwlc_live(c) || !w->surface || !w->configured || !w->buf[w->began]->id)
        return ERR_PEER_CLOSED;   /* keep the begun one: the configure after a reconnect */
    unsigned slot = (unsigned)w->began;
    w->began = -1;
    return show(w, slot, rects, n, frame);
}

/* Ack the configure with a commit that changes nothing the buffer shows. */
static status_t ack_now(struct jwl_window *w)
{
    struct jwl_conn *k = w->c->conn;
    w->ack_due = false;
    status_t st = jwl_xdg_surface_ack_configure(k, w->xdg_surface, w->ack_serial);
    if (st == OK)
        st = jwl_wl_surface_commit(k, w->surface);
    if (st == OK)
        st = jwl_conn_flush(k);
    return st;
}

/* ---- configure ------------------------------------------------------------------------- */

void jwlc_window_frame_done(struct jwl_window *w, uint32_t time, bool made_up)
{
    struct jwl_event ev = { .type = JWL_EV_FRAME, .win = w };
    ev.frame.time = time;
    ev.frame.made_up = made_up;
    jwlc_queue(w->c, &ev);
}

/* The size to draw at after a configure asking asked_w x asked_h. */
static void new_size(struct jwl_window *w, int32_t *out_w, int32_t *out_h)
{
    bool takes = w->resizable || (w->states & TAKES_SIZES);
    if (!takes) {
        *out_w = w->want_w;
        *out_h = w->want_h;
    } else if (w->pend_w > 0 && w->pend_h > 0) {
        *out_w = clamp(w->pend_w);
        *out_h = clamp(w->pend_h);
    } else {
        *out_w = w->width;
        *out_h = w->height;
    }
}

static status_t surface_configure(struct jwl_window *w, uint32_t serial)
{
    w->states = w->pend_states;
    int32_t nw, nh;
    new_size(w, &nw, &nh);
    bool same = nw == w->width && nh == w->height;
    bool first = !w->configured, rebuilt = w->rebuilt;
    w->width = nw;
    w->height = nh;
    w->ack_serial = serial;
    w->ack_due = true;
    w->configured = true;
    w->rebuilt = false;
    struct jwl_event ev = { .type = JWL_EV_CONFIGURE, .win = w };
    ev.configure.width = nw;
    ev.configure.height = nh;
    ev.configure.asked_w = w->pend_w;
    ev.configure.asked_h = w->pend_h;
    ev.configure.states = w->states;
    ev.configure.rebuilt = rebuilt;
    jwlc_queue(w->c, &ev);
    status_t st = OK;
    bool can_show = same && w->shown >= 0 && w->buf[w->shown] && w->buf[w->shown]->id;
    if (can_show && rebuilt)
        st = show(w, (unsigned)w->shown, NULL, 0, false);   /* as it was before the loss */
    else if (can_show && !first)
        st = ack_now(w);   /* nothing to draw */
    if (w->frame_lost) {
        w->frame_lost = false;
        jwlc_window_frame_done(w, (uint32_t)(now() / NS_PER_MS), true);
    }
    return st;
}

/* xdg_toplevel.configure: kept until xdg_surface.configure applies it. */
static void toplevel_configure(struct jwl_window *w, int32_t width, int32_t height,
                               const struct jwl_array *states)
{
    w->pend_w = width;
    w->pend_h = height;
    w->pend_states = 0;
    const uint8_t *p = states->data;
    for (uint32_t i = 0; i + 4 <= states->size; i += 4) {
        uint32_t s;
        memcpy(&s, p + i, 4);
        if (s < 32)
            w->pend_states |= JWL_STATE(s);
    }
}

status_t jwlc_window_event(struct jwl_client *c, struct jwl_msg *m)
{
    struct jwl_window *w = m->data;
    if (!w)
        return OK;
    if (m->iface == &jwl_xdg_surface_interface && m->opcode == JWL_XDG_SURFACE_EV_CONFIGURE)
        return surface_configure(w, m->args[0].u);
    if (m->iface != &jwl_xdg_toplevel_interface)
        return OK;   /* wl_surface.enter and leave: one output, nothing to do */
    if (m->opcode == JWL_XDG_TOPLEVEL_EV_CONFIGURE) {
        toplevel_configure(w, m->args[0].i, m->args[1].i, &m->args[2].a);
        return OK;
    }
    if (m->opcode == JWL_XDG_TOPLEVEL_EV_CLOSE) {
        struct jwl_event ev = { .type = JWL_EV_CLOSE, .win = w };
        jwlc_queue(c, &ev);
    }
    return OK;
}

/* ---- the rest of the API ------------------------------------------------------------- */

bool jwl_window_configured(const struct jwl_window *w)
{
    return w->configured;
}

void jwl_window_size(const struct jwl_window *w, int32_t *width, int32_t *height)
{
    *width = w->width;
    *height = w->height;
}

uint32_t jwl_window_states(const struct jwl_window *w)
{
    return w->states;
}

status_t jwl_window_set_title(struct jwl_window *w, const char *title)
{
    copy_text(w->title, title);
    if (!jwlc_live(w->c) || !w->toplevel)
        return OK;   /* told when it is made again */
    return jwl_xdg_toplevel_set_title(w->c->conn, w->toplevel, w->title);
}

status_t jwl_window_set_fullscreen(struct jwl_window *w, bool on)
{
    w->fullscreen = on;
    if (!jwlc_live(w->c) || !w->toplevel)
        return OK;
    if (on)
        return jwl_xdg_toplevel_set_fullscreen(w->c->conn, w->toplevel, 0);
    return jwl_xdg_toplevel_unset_fullscreen(w->c->conn, w->toplevel);
}

status_t jwl_window_set_maximized(struct jwl_window *w, bool on)
{
    w->maximized = on;
    if (!jwlc_live(w->c) || !w->toplevel)
        return OK;
    if (on)
        return jwl_xdg_toplevel_set_maximized(w->c->conn, w->toplevel);
    return jwl_xdg_toplevel_unset_maximized(w->c->conn, w->toplevel);
}

status_t jwl_window_move(struct jwl_window *w)
{
    struct jwl_client *c = w->c;
    if (!jwlc_live(c) || !w->toplevel || !w->press_serial || !c->global[JWLC_SEAT])
        return ERR_BAD_STATE;
    return jwl_xdg_toplevel_move(c->conn, w->toplevel, c->global[JWLC_SEAT], w->press_serial);
}

void jwl_window_set_user(struct jwl_window *w, void *user)
{
    w->user = user;
}

void *jwl_window_user(const struct jwl_window *w)
{
    return w->user;
}
