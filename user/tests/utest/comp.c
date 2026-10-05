/* utest: bin/compositor (user/services/compositor) headless, driven over
 * real channels by test clients speaking Wayland through libjwl.
 *
 * t_comp_globals: the registry (wl_compositor 4, wl_shm 1, wl_output 3,
 * wl_seat 5, xdg_wm_base 1), binding them (wl_shm's two
 * formats, wl_output's geometry, mode, scale and done), sync, and the
 * headless image composed with the background.
 * t_comp_surface: a surface with a shm buffer from a kept pool: commit,
 * frame callbacks (a surface no window shows gets them at most once a
 * second), wl_buffer.release when a newer buffer is committed or the
 * surface detaches, destroying everything, and the pool's VMO given back
 * once the compositor lets go of it.
 *
 * The helpers (ct_*, comptest.h) are shared with comp_bad.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <os.h>
#include "comptest.h"
#include "utest.h"

/* ---- the compositor ---------------------------------------------------------------- */

uint64_t ct_job_used(handle_t job, unsigned kind)
{
    struct job_info ji;
    return info_of(job, &ji) == OK ? ji.used[kind] : ~0ull;
}

uint64_t ct_handles(const struct ct_comp *p)
{
    return ct_job_used(p->job, JOB_LIMIT_HANDLES);
}

bool ct_handles_back(const struct ct_comp *p, uint64_t want)
{
    uint64_t until = now() + CT_WAIT;
    while (ct_handles(p) != want && now() < until)
        jam_nanosleep(now() + NS_PER_MS);
    if (ct_handles(p) != want)
        FAIL("the compositor holds %lu handles, want %lu", (unsigned long)ct_handles(p),
             (unsigned long)want);
    return true;
}

/* The image VMO: one for the compositor to compose into, mapped by us too. */
static bool image_for(struct ct_comp *p, handle_t *theirs)
{
    uint64_t size = ((uint64_t)p->w * (uint64_t)p->h * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    handle_t v;
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(size, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, size, VMAR_READ, &addr), OK);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, theirs), OK);
    jam_handle_close(v);
    p->image = (const uint32_t *)(uintptr_t)addr;
    p->image_size = size;
    return true;
}

bool ct_start(struct ct_comp *p, int32_t w, int32_t h)
{
    return ct_start_arg(p, w, h, NULL);
}

bool ct_start_arg(struct ct_comp *p, int32_t w, int32_t h, const char *arg)
{
    *p = (struct ct_comp){ .w = w, .h = h };
    handle_t server, image;
    if (!image_for(p, &image))
        return false;
    CHECK_ST(jam_channel_create(&p->svc, &server), OK);
    CHECK_ST(new_job(&p->job), OK);
    char size[32];
    snprintf(size, sizeof(size), "size=%dx%d", w, h);
    const char *argv[] = { "bin/compositor", "headless", size, "nodesk", arg };
    struct spawn_handle x[] = { { SR_USER + 0, server }, { SR_USER + 1, image } };
    struct spawn_args a = { .path = "bin/compositor", .argc = arg ? 5 : 4, .argv = argv,
                            .job = p->job, .extra = x, .nextra = 2 };
    CHECK_ST(spawn(&a, &p->proc), OK);
    return true;
}

bool ct_stop(struct ct_comp *p)
{
    struct process_info info;
    CHECK_ST(jam_handle_close(p->svc), OK);   /* /svc/wayland gone: it ends */
    CHECK_ST(spawn_wait(p->proc, CT_WAIT, &info), OK);
    CHECK_EQ(info.killed, 0);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(p->proc), OK);
    uint64_t until = now() + CT_WAIT;   /* dead is not yet freed */
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++) {
        while (ct_job_used(p->job, k) && now() < until)
            jam_nanosleep(now() + NS_PER_MS);
        if (ct_job_used(p->job, k))
            FAIL("the compositor left %lu units of job kind %u",
                 (unsigned long)ct_job_used(p->job, k), k);
    }
    CHECK_ST(jam_handle_close(p->job), OK);
    if (p->image)   /* none if the compositor was started with an image of its own */
        CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p->image,
                                p->image_size), OK);
    return true;
}

/* ---- clients ----------------------------------------------------------------------- */

bool ct_adopt(handle_t ch, struct ct_client *k)
{
    memset(k, 0, sizeof(*k));
    struct jwl_conn_config cfg = { .ch = ch, .side = JWL_CLIENT,
                                   .display = &jwl_wl_display_interface };
    CHECK_ST(jwl_conn_create(&cfg, &k->c), OK);
    return true;
}

bool ct_open(struct ct_comp *p, struct ct_client *k)
{
    handle_t ch;
    CHECK_ST(svc_connect_within(p->svc, CT_WAIT, &ch), OK);
    return ct_adopt(ch, k);
}

void ct_close(struct ct_client *k)
{
    jwl_conn_destroy(k->c);
    k->c = NULL;
    if (k->kept != HANDLE_INVALID)
        jam_handle_close(k->kept);
    k->kept = HANDLE_INVALID;
}

void ct_clear(struct ct_client *k)
{
    k->nev = 0;
}

uint32_t ct_new(struct ct_client *k, const struct jwl_interface *iface, uint32_t version)
{
    uint32_t id = 0;
    /* A typed new id takes its parent's version, which may be above its own
     * interface's (wl_surface.frame at version 4 makes a wl_callback of
     * version 4; wl_callback's table says 1), and jwl_conn_make refuses
     * that: the map's own call doesn't. */
    if (k->c->status == OK && version > iface->version)
        return jwl_map_new(&k->c->map, iface, version, NULL, &id) == OK ? id : 0;
    return jwl_conn_make(k->c, iface, version, NULL, &id) == OK ? id : 0;
}

static void record_array(struct ct_event *e, const struct jwl_array *a)
{
    e->na = a->size / 4;
    if (e->na)
        memcpy(e->a, a->data, (e->na < 8 ? e->na : 8) * 4);
}

static void record(struct ct_client *k, struct jwl_msg *m)
{
    struct ct_event e = { .iface = m->iface, .op = m->opcode, .id = m->id };
    struct jwl_sig sig;
    unsigned nu = 0;
    if (jwl_sig_parse(m->msg->signature, &sig) == OK) {
        for (unsigned i = 0; i < sig.n && i < m->nargs; i++) {
            if (sig.type[i] == 's' && m->args[i].s && !e.s[0])
                snprintf(e.s, sizeof(e.s), "%s", m->args[i].s);
            else if (sig.type[i] == 'a' && !e.na)
                record_array(&e, &m->args[i].a);
            else if (sig.type[i] != 's' && sig.type[i] != 'a' && nu < 4)
                e.u[nu++] = m->args[i].u;
            if (sig.type[i] == 'h' && k->kept == HANDLE_INVALID) {
                k->kept = m->args[i].h;   /* the test's now */
                m->args[i].h = HANDLE_INVALID;
            }
        }
    }
    jwl_msg_close_handles(m);
    if (m->iface == &jwl_wl_display_interface && m->opcode == JWL_WL_DISPLAY_EV_ERROR) {
        k->errored = true;
        k->err = e;
    }
    if (k->nev == CT_EVENTS) {
        memmove(k->ev, k->ev + 1, (CT_EVENTS - 1) * sizeof(k->ev[0]));
        k->nev--;
    }
    k->ev[k->nev++] = e;
}

/* One event into the log, waiting until deadline for it. */
static status_t pump(struct ct_client *k, uint64_t deadline)
{
    for (;;) {
        struct jwl_msg m;
        status_t st = jwl_conn_next(k->c, &m);
        if (st == OK) {
            record(k, &m);
            return OK;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;
        st = jwl_conn_flush(k->c);
        signals_t seen = 0;
        if (st == OK)
            st = jam_object_wait_one(k->c->ch, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st != OK)
            return st;
    }
}

const struct ct_event *ct_find(const struct ct_client *k, const struct jwl_interface *iface,
                               uint16_t op, uint32_t id)
{
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface == iface && k->ev[i].op == op && (!id || k->ev[i].id == id))
            return &k->ev[i];
    return NULL;
}

const struct ct_event *ct_await(struct ct_client *k, const struct jwl_interface *iface,
                                uint16_t op, uint32_t id, uint64_t timeout)
{
    uint64_t deadline = now() + timeout;
    if (jwl_conn_flush(k->c) != OK)
        return NULL;
    const struct ct_event *e;
    while (!(e = ct_find(k, iface, op, id)) && !k->errored)
        if (pump(k, deadline) != OK)
            return NULL;
    return e;
}

/* The events of iface's object id forgotten: an id used again would find
 * its last owner's (an earlier round trip's done) and stop waiting early. */
static void forget(struct ct_client *k, const struct jwl_interface *iface, uint32_t id)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k->nev; i++)
        if (k->ev[i].iface != iface || k->ev[i].id != id)
            k->ev[n++] = k->ev[i];
    k->nev = n;
}

status_t ct_roundtrip(struct ct_client *k)
{
    uint32_t cb = ct_new(k, &jwl_wl_callback_interface, 1);
    forget(k, &jwl_wl_callback_interface, cb);
    status_t st = cb ? jwl_wl_display_sync(k->c, JWL_DISPLAY_ID, cb) : ERR_NO_RESOURCES;
    if (st != OK)
        return st;
    if (ct_await(k, &jwl_wl_callback_interface, JWL_WL_CALLBACK_EV_DONE, cb, CT_WAIT))
        return OK;
    return k->errored ? ERR_INVALID_ARGS : ERR_TIMED_OUT;
}

bool ct_expect_error(struct ct_client *k, uint32_t object, uint32_t code)
{
    CHECK(ct_await(k, &jwl_wl_display_interface, JWL_WL_DISPLAY_EV_ERROR, 0, CT_WAIT));
    if (k->err.u[0] != object || k->err.u[1] != code)
        FAIL("error on object %u code %u (\"%s\"), want object %u code %u", k->err.u[0],
             k->err.u[1], k->err.s, object, code);
    return true;
}

bool ct_bind_all(struct ct_client *k)
{
    struct jwl_conn *c = k->c;
    CHECK((k->registry = ct_new(k, &jwl_wl_registry_interface, 1)) != 0);
    CHECK_ST(jwl_wl_display_get_registry(c, JWL_DISPLAY_ID, k->registry), OK);
    CHECK((k->compositor = ct_new(k, &jwl_wl_compositor_interface, 4)) != 0);
    CHECK_ST(jwl_wl_registry_bind(c, k->registry, 1, "wl_compositor", 4, k->compositor), OK);
    CHECK((k->shm = ct_new(k, &jwl_wl_shm_interface, 1)) != 0);
    CHECK_ST(jwl_wl_registry_bind(c, k->registry, 2, "wl_shm", 1, k->shm), OK);
    CHECK((k->output = ct_new(k, &jwl_wl_output_interface, 3)) != 0);
    CHECK_ST(jwl_wl_registry_bind(c, k->registry, 3, "wl_output", 3, k->output), OK);
    CHECK_ST(ct_roundtrip(k), OK);
    return true;
}

handle_t ct_kept_vmo(uint64_t size)
{
    handle_t h = HANDLE_INVALID;
    (void)jam_vmo_create(size, VMO_KEEP_PAGES, HANDLE_INVALID, &h);   /* checked by the caller */
    return h;
}

handle_t ct_dup_for_pool(handle_t vmo)
{
    handle_t h = HANDLE_INVALID;
    (void)jam_handle_duplicate(vmo, RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER, &h);
    return h;
}

uint32_t ct_pool(struct ct_client *k, uint32_t size, handle_t *vmo)
{
    handle_t v = ct_kept_vmo(size), d = v ? ct_dup_for_pool(v) : HANDLE_INVALID;
    uint32_t id = d ? ct_new(k, &jwl_wl_shm_pool_interface, 1) : 0;
    if (id && jwl_wl_shm_create_pool(k->c, k->shm, id, d, (int32_t)size) != OK)
        id = 0;   /* the handle went with the request, sent or not */
    else if (!id && d)
        jam_handle_close(d);
    if (vmo && id)
        *vmo = v;
    else if (v)
        jam_handle_close(v);
    return id;
}

/* ---- the tests -------------------------------------------------------------------- */

static bool has_global(const struct ct_client *k, uint32_t name, const char *iface, uint32_t v)
{
    for (unsigned i = 0; i < k->nev; i++) {
        const struct ct_event *e = &k->ev[i];
        if (e->iface == &jwl_wl_registry_interface && e->op == JWL_WL_REGISTRY_EV_GLOBAL &&
            e->u[0] == name && e->u[1] == v && !strcmp(e->s, iface))
            return true;
    }
    return false;
}

static unsigned count(const struct ct_client *k, const struct jwl_interface *iface, uint16_t op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < k->nev; i++)
        n += k->ev[i].iface == iface && k->ev[i].op == op;
    return n;
}

static bool globals_and_binds(struct ct_client *k)
{
    CHECK(ct_bind_all(k));
    CHECK(has_global(k, 1, "wl_compositor", 4));
    CHECK(has_global(k, 2, "wl_shm", 1));
    CHECK(has_global(k, 3, "wl_output", 3));
    CHECK(has_global(k, 4, "wl_seat", 5));
    CHECK(has_global(k, 5, "xdg_wm_base", 1));
    CHECK_EQ(count(k, &jwl_wl_registry_interface, JWL_WL_REGISTRY_EV_GLOBAL), 5);
    CHECK_EQ(count(k, &jwl_wl_shm_interface, JWL_WL_SHM_EV_FORMAT), 2);
    CHECK(ct_find(k, &jwl_wl_shm_interface, JWL_WL_SHM_EV_FORMAT, k->shm)->u[0] ==
          JWL_WL_SHM_FORMAT_ARGB8888);
    const struct ct_event *mode = ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_MODE,
                                          k->output);
    CHECK(mode && mode->u[0] == (JWL_WL_OUTPUT_MODE_CURRENT | JWL_WL_OUTPUT_MODE_PREFERRED));
    CHECK(mode->u[1] == 320 && mode->u[2] == 200 && mode->u[3] == 60000);
    CHECK(ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_GEOMETRY, k->output));
    const struct ct_event *scale = ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_SCALE,
                                           k->output);
    CHECK(scale && scale->u[0] == 1);
    CHECK(ct_find(k, &jwl_wl_output_interface, JWL_WL_OUTPUT_EV_DONE, k->output));
    return true;
}

bool t_comp_globals(void)
{
    static struct ct_client k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &k));
    CHECK(globals_and_binds(&k));
    /* the first paint is the wallpaper, all over */
    CHECK_EQ(p.image[0], ref_wallpaper_of(0, 0, 320, 200));
    CHECK_EQ(p.image[320 * 200 - 1], ref_wallpaper_of(319, 199, 320, 200));
    /* wl_output.release (version 3) and a second registry */
    CHECK_ST(jwl_wl_output_release(k.c, k.output), OK);
    ct_clear(&k);
    uint32_t reg2 = ct_new(&k, &jwl_wl_registry_interface, 1);
    CHECK_ST(jwl_wl_display_get_registry(k.c, JWL_DISPLAY_ID, reg2), OK);
    CHECK_ST(ct_roundtrip(&k), OK);
    CHECK_EQ(count(&k, &jwl_wl_registry_interface, JWL_WL_REGISTRY_EV_GLOBAL), 5);
    ct_close(&k);
    CHECK(ct_stop(&p));
    return true;
}

/* A 64x64 xrgb buffer at offset in pool, every pixel `colour`. */
static uint32_t buffer_at(struct ct_client *k, uint32_t pool, handle_t vmo, uint32_t offset,
                          uint32_t colour)
{
    static uint32_t px[64 * 64];
    for (unsigned i = 0; i < 64 * 64; i++)
        px[i] = colour;
    uint32_t id = ct_new(k, &jwl_wl_buffer_interface, 1);
    if (!id || jam_vmo_write(vmo, offset, px, sizeof(px)) != OK ||
        jwl_wl_shm_pool_create_buffer(k->c, pool, id, (int32_t)offset, 64, 64, 256,
                                      JWL_WL_SHM_FORMAT_XRGB8888) != OK)
        return 0;
    return id;
}

/* Commit buffer (0: detach) on s with a frame callback: the callback's id. */
static uint32_t commit_frame(struct ct_client *k, uint32_t s, uint32_t buffer)
{
    uint32_t cb = ct_new(k, &jwl_wl_callback_interface, 4);   /* the surface's version */
    if (!cb || jwl_wl_surface_frame(k->c, s, cb) != OK ||
        jwl_wl_surface_attach(k->c, s, buffer, 0, 0) != OK ||
        jwl_wl_surface_damage_buffer(k->c, s, 0, 0, 64, 64) != OK ||
        jwl_wl_surface_commit(k->c, s) != OK)
        return 0;
    return cb;
}

/* Buffers, commits, frame callbacks and releases on one surface. */
static bool surface_frames(struct ct_client *k, uint32_t s, uint32_t a, uint32_t b)
{
    uint32_t cb1 = commit_frame(k, s, a);
    CHECK(cb1);
    const struct ct_event *d1 = ct_await(k, &jwl_wl_callback_interface, JWL_WL_CALLBACK_EV_DONE,
                                         cb1, CT_WAIT);
    CHECK(d1);
    uint32_t t1 = d1->u[0];
    CHECK(!ct_find(k, &jwl_wl_buffer_interface, JWL_WL_BUFFER_EV_RELEASE, 0));
    /* b replaces a: a is released; the next callback waits out the second */
    uint32_t cb2 = commit_frame(k, s, b);
    CHECK(cb2);
    CHECK(ct_await(k, &jwl_wl_buffer_interface, JWL_WL_BUFFER_EV_RELEASE, a, CT_WAIT));
    const struct ct_event *d2 = ct_await(k, &jwl_wl_callback_interface, JWL_WL_CALLBACK_EV_DONE,
                                         cb2, CT_WAIT);
    CHECK(d2);
    if (d2->u[0] - t1 < 999)
        FAIL("a hidden surface's callbacks %u ms apart, want a second", d2->u[0] - t1);
    /* the same buffer again: not released; then a detach releases it */
    CHECK(commit_frame(k, s, b));
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(!ct_find(k, &jwl_wl_buffer_interface, JWL_WL_BUFFER_EV_RELEASE, b));
    CHECK(commit_frame(k, s, 0));
    CHECK(ct_await(k, &jwl_wl_buffer_interface, JWL_WL_BUFFER_EV_RELEASE, b, CT_WAIT));
    return true;
}

bool t_comp_surface(void)
{
    static struct ct_client k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &k));
    CHECK(ct_bind_all(&k));
    uint64_t base = ct_handles(&p);
    uint64_t pages0 = ct_job_used(own_job(), JOB_LIMIT_PAGES);
    handle_t vmo;
    uint32_t pool = ct_pool(&k, 65536, &vmo);
    CHECK(pool);
    uint32_t a = buffer_at(&k, pool, vmo, 0, 0x00ff0000), b = buffer_at(&k, pool, vmo, 16384, 0xff);
    CHECK(a && b);
    jam_handle_close(vmo);   /* the compositor's mapping keeps the pages now */
    uint32_t s = ct_new(&k, &jwl_wl_surface_interface, 4);
    CHECK(s);
    CHECK_ST(jwl_wl_compositor_create_surface(k.c, k.compositor, s), OK);
    CHECK_ST(jwl_wl_surface_set_buffer_scale(k.c, s, 1), OK);
    CHECK_ST(jwl_wl_surface_set_buffer_transform(k.c, s, 0), OK);
    ct_clear(&k);
    CHECK(surface_frames(&k, s, a, b));
    /* everything destroyed (the pool before its buffers: they keep it) */
    CHECK_ST(jwl_wl_shm_pool_destroy(k.c, pool), OK);
    CHECK_ST(jwl_wl_buffer_destroy(k.c, a), OK);
    CHECK_ST(jwl_wl_surface_destroy(k.c, s), OK);
    CHECK_ST(jwl_wl_buffer_destroy(k.c, b), OK);
    CHECK_ST(ct_roundtrip(&k), OK);
    CHECK(!k.errored);
    CHECK(ct_handles_back(&p, base));   /* the pool's VMO handle closed */
    CHECK(ct_job_used(own_job(), JOB_LIMIT_PAGES) <= pages0 + 4);   /* its 16 pages freed */
    ct_close(&k);
    CHECK(ct_stop(&p));
    return true;
}
