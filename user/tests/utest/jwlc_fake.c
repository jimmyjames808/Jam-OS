/* utest: a fake compositor for libjwl's client side (jwlcfake.h says what
 * it does). It answers through libjwl's own transport and the generated
 * tables, as the real one does, so what the client sends is checked by
 * the same codec; anything it doesn't expect is noted in f->unexpected
 * for the test to fail on. */
#include <jwl.h>
#include <keymap.h>
#include <os.h>
#include "jwlcfake.h"

static const char *const names[FAKE_GLOBALS] = {
    [FAKE_COMPOSITOR] = "wl_compositor", [FAKE_SHM] = "wl_shm", [FAKE_WM_BASE] = "xdg_wm_base",
    [FAKE_SEAT] = "wl_seat",             [FAKE_OUTPUT] = "wl_output",
};

static const struct jwl_interface *const fake_known[] = {
    &jwl_wl_compositor_interface, &jwl_wl_shm_interface,    &jwl_xdg_wm_base_interface,
    &jwl_wl_seat_interface,       &jwl_wl_output_interface,
};

static void unexpected(struct fake *f, const struct jwl_msg *m, const char *why)
{
    if (!f->unexpected[0])
        snprintf(f->unexpected, sizeof(f->unexpected), "%s.%s: %s", m->iface->name,
                 m->msg->name, why);
}

void fake_init(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->offer[FAKE_COMPOSITOR] = 6;
    f->offer[FAKE_SHM] = 1;
    f->offer[FAKE_WM_BASE] = 3;
    f->offer[FAKE_SEAT] = 7;
    f->offer[FAKE_OUTPUT] = 4;
    f->formats = 1u << JWL_WL_SHM_FORMAT_ARGB8888 | 1u << JWL_WL_SHM_FORMAT_XRGB8888;
    f->repeat_rate = 30;
    f->repeat_delay = 500;
    f->serial = 100;
}

/* Everything of this connection's goes: mappings, slots, ids. */
static void forget_conn(struct fake *f)
{
    for (unsigned i = 0; i < FAKE_POOLS; i++) {
        if (f->p[i].addr)
            (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), f->p[i].addr, f->p[i].size);
        if (f->p[i].vmo)
            jam_handle_close(f->p[i].vmo);
    }
    jwl_conn_destroy(f->conn);
    f->conn = NULL;
    memset(f->bound, 0, sizeof(f->bound));
    f->keyboard = f->pointer = f->seat = f->wm_base = 0;
    memset(f->s, 0, sizeof(f->s));
    memset(f->p, 0, sizeof(f->p));
    memset(f->b, 0, sizeof(f->b));
}

void fake_drop(struct fake *f)
{
    forget_conn(f);
}

void fake_free(struct fake *f)
{
    forget_conn(f);
    handle_t h = __atomic_exchange_n(&f->pending, HANDLE_INVALID, __ATOMIC_ACQ_REL);
    if (h != HANDLE_INVALID)
        jam_handle_close(h);
}

status_t fake_connect(void *ctx, handle_t *out)
{
    struct fake *f = ctx;
    if (f->refuse) {
        f->refuse--;
        return ERR_PEER_CLOSED;
    }
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    handle_t old = __atomic_exchange_n(&f->pending, mine, __ATOMIC_ACQ_REL);
    if (old != HANDLE_INVALID)
        jam_handle_close(old);
    f->connects++;
    *out = theirs;
    return OK;
}

struct jwl_client_config fake_config(struct fake *f)
{
    struct jwl_client_config cfg = { .connect = fake_connect, .connect_ctx = f,
                                     .name = "utest-jwl", .quiet = true };
    return cfg;
}

/* ---- the globals --------------------------------------------------------------------- */

static void send_globals(struct fake *f, uint32_t registry)
{
    for (unsigned i = 0; i < FAKE_GLOBALS; i++)
        if (f->offer[i])
            (void)jwl_wl_registry_send_global(f->conn, registry, i + 1, names[i], f->offer[i]);
}

static status_t send_keymap(struct fake *f)
{
    const char *text = f->keymap_text ? f->keymap_text : keymap_us.xkb;
    uint32_t size = (uint32_t)strlen(text) + 1;
    handle_t v, ro;
    status_t st = jam_vmo_create(size, 0, HANDLE_INVALID, &v);
    if (st == OK)
        st = jam_vmo_write(v, 0, text, size);
    if (st == OK)
        st = jam_handle_duplicate(v, RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER, &ro);
    if (v)
        jam_handle_close(v);
    if (st == OK)
        st = jwl_wl_keyboard_send_keymap(f->conn, f->keyboard,
                                         JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, ro, size);
    if (st == OK && f->bound[FAKE_SEAT] >= JWL_WL_KEYBOARD_EV_REPEAT_INFO_SINCE)
        st = jwl_wl_keyboard_send_repeat_info(f->conn, f->keyboard, f->repeat_rate,
                                              f->repeat_delay);
    return st;
}

static void bound(struct fake *f, unsigned g, uint32_t id, uint32_t v)
{
    struct jwl_conn *k = f->conn;
    if (g == FAKE_SEAT) {
        f->seat = id;
        (void)jwl_wl_seat_send_capabilities(k, id, JWL_WL_SEAT_CAPABILITY_KEYBOARD |
                                                       JWL_WL_SEAT_CAPABILITY_POINTER);
    } else if (g == FAKE_SHM) {
        for (uint32_t i = 0; i < 32; i++)
            if (f->formats >> i & 1)
                (void)jwl_wl_shm_send_format(k, id, i);
    } else if (g == FAKE_OUTPUT) {
        (void)jwl_wl_output_send_geometry(k, id, 0, 0, 600, 340, 0, "Jam", "fake", 0);
        (void)jwl_wl_output_send_mode(k, id, JWL_WL_OUTPUT_MODE_CURRENT, 1280, 800, 60000);
        if (v >= 2)
            (void)jwl_wl_output_send_scale(k, id, 1);
        if (v >= 2)
            (void)jwl_wl_output_send_done(k, id);
    } else if (g == FAKE_WM_BASE) {
        f->wm_base = id;
        if (f->ping_at_bind)
            (void)fake_ping(f);
    }
}

static void display_request(struct fake *f, const struct jwl_msg *m)
{
    if (m->opcode == JWL_WL_DISPLAY_REQ_SYNC) {
        f->syncs++;
        (void)jwl_wl_callback_send_done(f->conn, m->args[0].n, f->serial++);
    } else {
        send_globals(f, m->args[0].n);
    }
}

static void registry_request(struct fake *f, const struct jwl_msg *m)
{
    uint32_t name = m->args[0].u, v = m->args[2].u;
    unsigned g = name - 1;
    if (name < 1 || name > FAKE_GLOBALS || strcmp(m->args[1].s, names[g]) || v > f->offer[g]) {
        unexpected(f, m, "a bind that doesn't fit the global");
        return;
    }
    f->binds++;
    f->bound[g] = v;
    bound(f, g, m->args[3].n, v);
}

/* ---- surfaces ------------------------------------------------------------------------ */

static struct fake_surface *by_surface(struct fake *f, uint32_t id)
{
    for (unsigned i = 0; i < FAKE_SURFACES; i++)
        if (f->s[i].id == id && id)
            return &f->s[i];
    return NULL;
}

static void commit(struct fake *f, struct fake_surface *s)
{
    s->commits++;
    if (s->attached) {
        if (s->current && s->current != s->pending)
            (void)jwl_wl_buffer_send_release(f->conn, s->current);   /* gone: NOT_FOUND */
        s->current = s->pending;
        s->attached = false;
        if (s->current)
            s->pixel = fake_pixel(f, s->current);
    }
    for (unsigned i = 0; i < s->nframes && !f->hold_frames; i++)
        (void)jwl_wl_callback_send_done(f->conn, s->frames[i], (uint32_t)(now() / NS_PER_MS));
    if (!f->hold_frames)
        s->nframes = 0;
    if (s->toplevel && !s->sent_serial)
        (void)fake_configure(f, s, f->cfg_w, f->cfg_h, f->cfg_states);
}

static void surface_request(struct fake *f, struct fake_surface *s, const struct jwl_msg *m)
{
    const union jwl_arg *a = m->args;
    switch (m->opcode) {
    case JWL_WL_SURFACE_REQ_ATTACH:
        s->pending = a[0].o;
        s->attached = true;
        s->attaches++;
        break;
    case JWL_WL_SURFACE_REQ_DAMAGE:
    case JWL_WL_SURFACE_REQ_DAMAGE_BUFFER:
        s->damages++;
        s->last_damage = (struct jwl_rect){ a[0].i, a[1].i, a[2].i, a[3].i };
        break;
    case JWL_WL_SURFACE_REQ_FRAME:
        if (s->nframes < FAKE_FRAMES)
            s->frames[s->nframes++] = a[0].n;
        break;
    case JWL_WL_SURFACE_REQ_COMMIT:
        commit(f, s);
        break;
    case JWL_WL_SURFACE_REQ_DESTROY:
        memset(s, 0, sizeof(*s));
        break;
    default:
        unexpected(f, m, "not used by libjwl");
    }
}

static void xdg_request(struct fake *f, struct fake_surface *s, const struct jwl_msg *m)
{
    const union jwl_arg *a = m->args;
    if (m->iface == &jwl_xdg_surface_interface) {
        if (m->opcode == JWL_XDG_SURFACE_REQ_GET_TOPLEVEL) {
            s->toplevel = a[0].n;
            (void)jwl_map_set_data(&f->conn->map, s->toplevel, s);
        } else if (m->opcode == JWL_XDG_SURFACE_REQ_ACK_CONFIGURE) {
            s->acks++;
            s->acked = a[0].u;
        } else if (m->opcode == JWL_XDG_SURFACE_REQ_DESTROY) {
            s->xdg = 0;
        }
        return;
    }
    switch (m->opcode) {
    case JWL_XDG_TOPLEVEL_REQ_SET_TITLE:
        snprintf(s->title, sizeof(s->title), "%s", a[0].s);
        break;
    case JWL_XDG_TOPLEVEL_REQ_SET_MIN_SIZE:
        s->min_w = a[0].i;
        s->min_h = a[1].i;
        break;
    case JWL_XDG_TOPLEVEL_REQ_SET_MAX_SIZE:
        s->max_w = a[0].i;
        s->max_h = a[1].i;
        break;
    case JWL_XDG_TOPLEVEL_REQ_SET_FULLSCREEN:
    case JWL_XDG_TOPLEVEL_REQ_UNSET_FULLSCREEN:
        s->fullscreen = m->opcode == JWL_XDG_TOPLEVEL_REQ_SET_FULLSCREEN;
        break;
    case JWL_XDG_TOPLEVEL_REQ_SET_MAXIMIZED:
    case JWL_XDG_TOPLEVEL_REQ_UNSET_MAXIMIZED:
        s->maximized = m->opcode == JWL_XDG_TOPLEVEL_REQ_SET_MAXIMIZED;
        break;
    case JWL_XDG_TOPLEVEL_REQ_MOVE:
        s->move_serial = a[1].u;
        break;
    case JWL_XDG_TOPLEVEL_REQ_DESTROY:
        s->toplevel = 0;
        break;
    }
}

static void new_surface(struct fake *f, const struct jwl_msg *m)
{
    for (unsigned i = 0; i < FAKE_SURFACES; i++)
        if (!f->s[i].id) {
            f->s[i].id = m->args[0].n;
            (void)jwl_map_set_data(&f->conn->map, f->s[i].id, &f->s[i]);
            return;
        }
    unexpected(f, m, "more surfaces than the fake keeps");
}

static void wm_base_request(struct fake *f, const struct jwl_msg *m)
{
    if (m->opcode == JWL_XDG_WM_BASE_REQ_PONG) {
        f->pongs++;
    } else if (m->opcode == JWL_XDG_WM_BASE_REQ_GET_XDG_SURFACE) {
        struct fake_surface *s = by_surface(f, m->args[1].o);
        if (!s) {
            unexpected(f, m, "no such surface");
            return;
        }
        s->xdg = m->args[0].n;
        (void)jwl_map_set_data(&f->conn->map, s->xdg, s);
    } else if (m->opcode != JWL_XDG_WM_BASE_REQ_DESTROY) {
        unexpected(f, m, "not used by libjwl");
    }
}

/* ---- pools and buffers ---------------------------------------------------------------- */

static void new_pool(struct fake *f, struct jwl_msg *m)
{
    handle_t vmo = m->args[1].h;
    m->args[1].h = HANDLE_INVALID;   /* ours now */
    uint64_t size = (uint64_t)(uint32_t)m->args[2].i, addr = 0;
    struct fake_pool *p = NULL;
    for (unsigned i = 0; i < FAKE_POOLS && !p; i++)
        if (!f->p[i].id && !f->p[i].addr)
            p = &f->p[i];
    status_t st = p ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size,
                                   VMAR_READ | VMAR_KEPT_ONLY, &addr)
                    : ERR_NO_RESOURCES;
    if (st != OK) {
        jam_handle_close(vmo);
        unexpected(f, m, status_str(st));
        return;
    }
    p->id = m->args[0].n;
    p->vmo = vmo;   /* kept: a resize maps it again */
    p->addr = addr;
    p->size = size;
    f->pools_made++;
    (void)jwl_map_set_data(&f->conn->map, p->id, p);
}

/* Map the grown pool again, as the real compositor does (wl_shm_pool.resize:
 * only bigger), and drop the old mapping. */
static void resize_pool(struct fake *f, struct fake_pool *p, const struct jwl_msg *m)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR);
    uint64_t size = (uint64_t)(uint32_t)m->args[0].i, addr = 0;
    if (size < p->size) {
        unexpected(f, m, "a pool made smaller");
        return;
    }
    status_t st = jam_vmar_map(vmar, p->vmo, 0, size, VMAR_READ | VMAR_KEPT_ONLY, &addr);
    if (st != OK) {
        unexpected(f, m, status_str(st));
        return;
    }
    (void)jam_vmar_unmap(vmar, p->addr, p->size);
    p->addr = addr;
    p->size = size;
}

static void pool_request(struct fake *f, struct fake_pool *p, const struct jwl_msg *m)
{
    const union jwl_arg *a = m->args;
    if (m->opcode == JWL_WL_SHM_POOL_REQ_DESTROY) {
        p->id = 0;   /* the mapping stays: its buffers may */
        return;
    }
    if (m->opcode == JWL_WL_SHM_POOL_REQ_RESIZE) {
        resize_pool(f, p, m);
        return;
    }
    for (unsigned i = 0; i < FAKE_BUFFERS; i++)
        if (!f->b[i].id) {
            f->b[i] = (struct fake_buffer){ a[0].n, p, a[1].i, a[2].i, a[3].i, a[4].i, a[5].u };
            f->buffers_made++;
            (void)jwl_map_set_data(&f->conn->map, a[0].n, &f->b[i]);
            return;
        }
    unexpected(f, m, "more buffers than the fake keeps");
}

uint32_t fake_pixel(struct fake *f, uint32_t buffer)
{
    for (unsigned i = 0; i < FAKE_BUFFERS; i++) {
        const struct fake_buffer *b = &f->b[i];
        if (b->id == buffer && buffer && b->offset >= 0 && (uint64_t)b->offset + 4 <= b->pool->size)
            return *(const volatile uint32_t *)(uintptr_t)(b->pool->addr + (uint64_t)b->offset);
    }
    return 0;
}

/* ---- serving ------------------------------------------------------------------------- */

static void seat_request(struct fake *f, const struct jwl_msg *m)
{
    if (m->iface == &jwl_wl_seat_interface && m->opcode == JWL_WL_SEAT_REQ_GET_KEYBOARD) {
        f->keyboard = m->args[0].n;
        if (send_keymap(f) != OK)
            unexpected(f, m, "can't send the keymap");
    } else if (m->iface == &jwl_wl_seat_interface && m->opcode == JWL_WL_SEAT_REQ_GET_POINTER) {
        f->pointer = m->args[0].n;
    } else if (m->iface == &jwl_wl_keyboard_interface) {
        f->keyboard = 0;   /* release */
    } else if (m->iface == &jwl_wl_pointer_interface && m->opcode == JWL_WL_POINTER_REQ_RELEASE) {
        f->pointer = 0;
    } else {
        unexpected(f, m, "not used by libjwl");
    }
}

static void serve_one(struct fake *f, struct jwl_msg *m)
{
    const struct jwl_interface *i = m->iface;
    if (i == &jwl_wl_display_interface)
        display_request(f, m);
    else if (i == &jwl_wl_registry_interface)
        registry_request(f, m);
    else if (i == &jwl_wl_compositor_interface && m->opcode == JWL_WL_COMPOSITOR_REQ_CREATE_SURFACE)
        new_surface(f, m);
    else if (i == &jwl_wl_surface_interface && m->data)
        surface_request(f, m->data, m);
    else if ((i == &jwl_xdg_surface_interface || i == &jwl_xdg_toplevel_interface) && m->data)
        xdg_request(f, m->data, m);
    else if (i == &jwl_xdg_wm_base_interface)
        wm_base_request(f, m);
    else if (i == &jwl_wl_shm_interface && m->opcode == JWL_WL_SHM_REQ_CREATE_POOL)
        new_pool(f, m);
    else if (i == &jwl_wl_shm_pool_interface && m->data)
        pool_request(f, m->data, m);
    else if (i == &jwl_wl_buffer_interface && m->data)
        ((struct fake_buffer *)m->data)->id = 0;   /* destroy */
    else if (i == &jwl_wl_seat_interface || i == &jwl_wl_keyboard_interface ||
             i == &jwl_wl_pointer_interface)
        seat_request(f, m);
    else
        unexpected(f, m, "not used by libjwl");
    jwl_msg_close_handles(m);   /* whatever wasn't taken */
}

/* A connection handed out and not taken up yet becomes the one. */
static void take_pending(struct fake *f)
{
    handle_t h = __atomic_exchange_n(&f->pending, HANDLE_INVALID, __ATOMIC_ACQ_REL);
    if (h == HANDLE_INVALID)
        return;
    forget_conn(f);
    f->syncs = f->pongs = f->binds = f->pools_made = f->buffers_made = 0;   /* this one's */
    struct jwl_conn_config cc = { .ch = h, .side = JWL_SERVER,
                                  .display = &jwl_wl_display_interface, .known = fake_known,
                                  .nknown = sizeof(fake_known) / sizeof(fake_known[0]) };
    if (jwl_conn_create(&cc, &f->conn) != OK)
        f->conn = NULL;
}

void fake_serve(struct fake *f)
{
    take_pending(f);
    while (f->conn) {
        struct jwl_msg m;
        status_t st = jwl_conn_next(f->conn, &m);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st != OK) {
            forget_conn(f);   /* the client went, or broke the protocol */
            return;
        }
        serve_one(f, &m);
    }
    if (f->conn)
        (void)jwl_conn_flush(f->conn);
}

static uint64_t activity(struct fake *f, struct jwl_client *c)
{
    uint64_t n = f->connects * 1000003ull + jwl_client_info(c)->generation * 7919ull;
    if (f->conn)
        n += f->conn->stats.msgs_in + 3 * f->conn->stats.msgs_out;
    struct jwl_conn *k = jwl_client_conn(c);
    if (k)
        n += 5 * k->stats.msgs_in + 11 * k->stats.msgs_out;
    return n;
}

bool fake_pump(struct fake *f, struct jwl_client *c)
{
    unsigned quiet = 0;
    for (unsigned i = 0; i < 64; i++) {
        uint64_t before = activity(f, c);
        fake_serve(f);
        (void)jwl_client_dispatch(c);
        quiet = activity(f, c) == before ? quiet + 1 : 0;
        if (quiet == 2)
            return true;
    }
    return false;
}
