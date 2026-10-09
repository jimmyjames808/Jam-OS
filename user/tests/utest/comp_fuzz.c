/* utest: bin/compositor against a fuzzer (docs/M12.1-REVIEW-COMPOSITOR.md:
 * the hostile tests in X1's style, comptest.h's harness).
 *
 * Each round a client sets up what a real one has (every global bound, a
 * seat with its pointer and keyboard, two surfaces, a toplevel, a pool and
 * a buffer, a region, a positioner, a clipboard source and device, a
 * cursor shape device), then stops speaking through libjwl and writes
 * JWL2 batches of its own: messages on those objects (and on ids that
 * name nothing), mostly with a real opcode and arguments of the right
 * letters but values chosen to hurt (0, -1, the int32 ends, the caps' edges,
 * other objects' ids, strings without their NUL or with bytes that aren't
 * UTF-8, handles of every kind, or none where one is due), now and then a
 * message whose size lies or a batch that is noise. It never reads. The
 * compositor may refuse anything, disconnect the client, or take it; what
 * it must do is stay up: after every round a healthy client round-trips.
 *
 * t_comp_fuzz: FUZZ_ROUNDS rounds against a compositor of our own
 * (headless, testwin, so roleless surfaces with buffers become windows):
 * afterwards it holds exactly the handles and message bytes it held
 * before. t_comp_live_fuzz: fewer rounds against the desktop's own
 * (/svc/wayland), which must keep serving (G1-PLAN's X1; g1-hostile.txt
 * runs `utest only comp_live`). The rounds are seeded: a failure prints
 * the round, and FUZZ_SEED + round makes it again. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl/cursor_shape_v1.h>
#include <jwl/jam_window_memory_v1.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>
#include <os.h>
#include "comptest.h"
#include "utest.h"

#define FUZZ_SEED        0x4a4d3132u   /* "JM12" */
#define FUZZ_ROUNDS      48u
#define FUZZ_LIVE_ROUNDS 16u
#define FUZZ_BATCHES     40u           /* batches a round, at most */
#define FUZZ_MSGS        6u            /* messages a batch, at most */
#define FUZZ_OBJS        96u

struct fobj {
    uint32_t id;
    const struct jwl_interface *iface;
    uint32_t version;
};

struct fuzz {
    struct ct_client k;
    uint32_t r;                          /* the generator's state */
    struct fobj o[FUZZ_OBJS];
    unsigned n;
    uint32_t words[JWL_BATCH_MAX / 4];   /* the batch being made */
    unsigned at;
    handle_t h[JWL_BATCH_HANDLES];
    unsigned nh;
    handle_t vmo;                        /* a kept VMO the pools' handles duplicate */
};

static uint32_t rnd(struct fuzz *f)
{
    uint32_t x = f->r;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return f->r = x;
}

static uint32_t below(struct fuzz *f, uint32_t n)
{
    return n ? rnd(f) % n : 0;
}

static void obj_add(struct fuzz *f, uint32_t id, const struct jwl_interface *iface, uint32_t version)
{
    if (id && iface && f->n < FUZZ_OBJS)
        f->o[f->n++] = (struct fobj){ id, iface, version };
}

/* ---- the setup, through libjwl --------------------------------------------------------- */

static uint32_t bind(struct fuzz *f, uint32_t name, const struct jwl_interface *iface, uint32_t v)
{
    uint32_t id = ct_new(&f->k, iface, v);
    if (id && jwl_wl_registry_bind(f->k.c, f->k.registry, name, iface->name, v, id) == OK)
        obj_add(f, id, iface, v);
    return id;
}

static uint32_t make(struct fuzz *f, const struct jwl_interface *iface, uint32_t v)
{
    uint32_t id = ct_new(&f->k, iface, v);
    obj_add(f, id, iface, v);
    return id;
}

static bool setup(struct ct_comp *p, struct fuzz *f, uint32_t seed)
{
    memset(f, 0, sizeof(*f));
    f->r = seed ? seed : 1;
    f->vmo = HANDLE_INVALID;
    CHECK(ct_open(p, &f->k));
    CHECK(ct_bind_all(&f->k));
    struct jwl_conn *c = f->k.c;
    obj_add(f, JWL_DISPLAY_ID, &jwl_wl_display_interface, 1);
    obj_add(f, f->k.registry, &jwl_wl_registry_interface, 1);
    obj_add(f, f->k.compositor, &jwl_wl_compositor_interface, 4);
    obj_add(f, f->k.shm, &jwl_wl_shm_interface, 1);
    obj_add(f, f->k.output, &jwl_wl_output_interface, 3);
    uint32_t seat = bind(f, 4, &jwl_wl_seat_interface, 5);
    uint32_t wm = bind(f, 5, &jwl_xdg_wm_base_interface, 1);
    uint32_t shapes = bind(f, 6, &jwl_wp_cursor_shape_manager_v1_interface, 2);
    uint32_t data = bind(f, 7, &jwl_wl_data_device_manager_interface, 3);
    (void)bind(f, 8, &jwl_jam_window_memory_v1_interface, 1);
    uint32_t ptr = make(f, &jwl_wl_pointer_interface, 5);
    CHECK_ST(jwl_wl_seat_get_pointer(c, seat, ptr), OK);
    uint32_t kb = make(f, &jwl_wl_keyboard_interface, 5);
    CHECK_ST(jwl_wl_seat_get_keyboard(c, seat, kb), OK);
    uint32_t s1 = make(f, &jwl_wl_surface_interface, 4), s2 = make(f, &jwl_wl_surface_interface, 4);
    CHECK_ST(jwl_wl_compositor_create_surface(c, f->k.compositor, s1), OK);
    CHECK_ST(jwl_wl_compositor_create_surface(c, f->k.compositor, s2), OK);
    uint32_t xs = make(f, &jwl_xdg_surface_interface, 1), top = make(f, &jwl_xdg_toplevel_interface, 1);
    CHECK_ST(jwl_xdg_wm_base_get_xdg_surface(c, wm, xs, s1), OK);
    CHECK_ST(jwl_xdg_surface_get_toplevel(c, xs, top), OK);
    uint32_t pos = make(f, &jwl_xdg_positioner_interface, 1);
    CHECK_ST(jwl_xdg_wm_base_create_positioner(c, wm, pos), OK);
    CHECK((f->vmo = ct_kept_vmo(64 * 1024)) != HANDLE_INVALID);
    uint32_t pool = make(f, &jwl_wl_shm_pool_interface, 1);
    CHECK_ST(jwl_wl_shm_create_pool(c, f->k.shm, pool, ct_dup_for_pool(f->vmo), 64 * 1024), OK);
    uint32_t buf = make(f, &jwl_wl_buffer_interface, 1);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(c, pool, buf, 0, 16, 16, 64,
                                           JWL_WL_SHM_FORMAT_ARGB8888), OK);
    uint32_t reg = make(f, &jwl_wl_region_interface, 4);   /* its parent's version */
    CHECK_ST(jwl_wl_compositor_create_region(c, f->k.compositor, reg), OK);
    uint32_t src = make(f, &jwl_wl_data_source_interface, 3);
    CHECK_ST(jwl_wl_data_device_manager_create_data_source(c, data, src), OK);
    uint32_t dev = make(f, &jwl_wl_data_device_interface, 3);
    CHECK_ST(jwl_wl_data_device_manager_get_data_device(c, data, dev, seat), OK);
    uint32_t sd = make(f, &jwl_wp_cursor_shape_device_v1_interface, 2);
    CHECK_ST(jwl_wp_cursor_shape_manager_v1_get_pointer(c, shapes, sd, ptr), OK);
    CHECK_ST(ct_roundtrip(&f->k), OK);
    CHECK(!f->k.errored);
    return true;
}

/* ---- the batches, by hand ---------------------------------------------------------------- */

static const uint32_t sharp[] = {
    0, 1, 2, 3, 4, 7, 8, 16, 63, 64, 255, 256, 4095, 4096, 8191, 8192, 8193, 65535, 65536,
    0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0xff000000, 0xfeffffff,
};

static uint32_t value(struct fuzz *f)
{
    switch (below(f, 4)) {
    case 0:
        return rnd(f);
    case 1:
        return below(f, 40) - 8;   /* small, negative too */
    default:
        return sharp[below(f, sizeof(sharp) / sizeof(sharp[0]))];
    }
}

static bool room(const struct fuzz *f, unsigned words)
{
    return f->at + words <= JWL_BATCH_MAX / 4;
}

static void put(struct fuzz *f, uint32_t w)
{
    if (room(f, 1))
        f->words[f->at++] = w;
}

/* A string or array argument: its length word, its bytes padded. */
static void put_bytes(struct fuzz *f, bool string)
{
    unsigned len = below(f, 6) == 0 ? 0 : 1 + below(f, 48);
    if (string && below(f, 8) == 0)
        len = 0;   /* a null string */
    if (!room(f, 1 + (len + 3) / 4)) {
        put(f, 0);
        return;
    }
    put(f, below(f, 16) == 0 ? value(f) : len);   /* now and then a length that lies */
    uint8_t *b = (uint8_t *)&f->words[f->at];
    for (unsigned i = 0; i < len; i++) {
        unsigned kind = below(f, 10);
        b[i] = kind == 0 ? (uint8_t)(0x80 + below(f, 128)) : kind == 1 ? 0 : (uint8_t)(' ' + below(f, 95));
    }
    if (string && len && below(f, 8))
        b[len - 1] = 0;   /* mostly a proper end */
    for (unsigned i = len; i % 4; i++)
        b[i] = 0;
    f->at += (len + 3) / 4;
}

/* A handle for an 'h': a pool's VMO, a plain VMO, a channel's end, or
 * (now and then) none at all. */
static void put_handle(struct fuzz *f)
{
    if (f->nh == JWL_BATCH_HANDLES || below(f, 6) == 0)
        return;
    handle_t h = HANDLE_INVALID, other;
    switch (below(f, 3)) {
    case 0:
        h = ct_dup_for_pool(f->vmo);
        break;
    case 1:
        (void)jam_vmo_create(PAGE_SIZE, 0, &h);
        break;
    default:
        if (jam_channel_create(&h, &other) == OK)
            jam_handle_close(other);
        break;
    }
    if (h != HANDLE_INVALID)
        f->h[f->nh++] = h;
}

/* An object's id for an 'o' (or a target): one of ours mostly, else 0 or
 * anything. */
static uint32_t some_id(struct fuzz *f)
{
    unsigned k = below(f, 10);
    if (k < 8 && f->n)
        return f->o[below(f, f->n)].id;
    return k == 8 ? 0 : value(f);
}

/* The globals a bind may name truly (else anything). */
static const struct {
    uint32_t name;
    const struct jwl_interface *iface;
    uint32_t version;
} globals[] = {
    { 1, &jwl_wl_compositor_interface, 4 }, { 2, &jwl_wl_shm_interface, 1 },
    { 3, &jwl_wl_output_interface, 3 }, { 4, &jwl_wl_seat_interface, 5 },
    { 5, &jwl_xdg_wm_base_interface, 1 }, { 6, &jwl_wp_cursor_shape_manager_v1_interface, 2 },
    { 7, &jwl_wl_data_device_manager_interface, 3 }, { 8, &jwl_jam_window_memory_v1_interface, 1 },
};

/* wl_registry.bind ("usun"): mostly a true global, by name, at a version it has. */
static void put_bind(struct fuzz *f)
{
    unsigned g = below(f, sizeof(globals) / sizeof(globals[0]));
    bool true_one = below(f, 4) != 0;
    uint32_t v = true_one ? 1 + below(f, globals[g].version) : value(f);
    put(f, true_one ? globals[g].name : value(f));
    const char *name = globals[g].iface->name;
    uint32_t len = (uint32_t)strlen(name) + 1;
    put(f, len);
    uint8_t *b = (uint8_t *)&f->words[f->at];
    if (room(f, (len + 3) / 4)) {
        memset(b, 0, (len + 3) / 4 * 4);
        memcpy(b, name, len);
        f->at += (len + 3) / 4;
    }
    put(f, v);
    uint32_t id = ct_new(&f->k, globals[g].iface, v >= 1 && v <= globals[g].iface->version ? v : 1);
    put(f, id ? id : value(f));
    obj_add(f, id, globals[g].iface, v);
}

/* One message onto the batch: on one of our objects with a real opcode
 * and arguments of its letters mostly; else anything. */
static void put_message(struct fuzz *f)
{
    unsigned start = f->at;
    if (!room(f, 2 + 4 * JWL_ARGS_MAX + 64))
        return;
    const struct fobj *o = f->n && below(f, 10) ? &f->o[below(f, f->n)] : NULL;
    uint32_t target = o ? o->id : some_id(f);
    uint32_t opcode = o && o->iface->nrequests && below(f, 12) ? below(f, o->iface->nrequests)
                                                               : below(f, 24);
    put(f, target);
    put(f, 0);   /* the size and opcode, below */
    const struct jwl_message *m = o && opcode < o->iface->nrequests ? &o->iface->requests[opcode]
                                                                    : NULL;
    struct jwl_sig sig;
    if (m && jwl_sig_parse(m->signature, &sig) == OK && !strcmp(m->signature, "usun")) {
        put_bind(f);
    } else if (m && jwl_sig_parse(m->signature, &sig) == OK) {
        for (unsigned i = 0; i < sig.n; i++) {
            switch (sig.type[i]) {
            case 's':
            case 'a':
                put_bytes(f, sig.type[i] == 's');
                break;
            case 'o':
                put(f, some_id(f));
                break;
            case 'n': {
                const struct jwl_interface *t = m->types ? m->types[i] : NULL;
                uint32_t id = t && below(f, 8) ? ct_new(&f->k, t, o->version) : 0;
                obj_add(f, id, t, o->version);
                put(f, id ? id : value(f));
                break;
            }
            case 'h':
                put_handle(f);
                break;
            default:   /* i, u, f */
                put(f, value(f));
                break;
            }
        }
        if (below(f, 20) == 0)
            put(f, value(f));   /* a word too many */
    } else {
        for (unsigned i = below(f, 8); i > 0; i--)
            put(f, value(f));
    }
    uint32_t size = (f->at - start) * 4;
    if (below(f, 20) == 0)
        size = value(f) & 0xfffc;   /* a size that lies */
    f->words[start + 1] = size << 16 | (opcode & 0xffff);
}

/* One batch written: messages, or (now and then) noise. Its handles go
 * with it. The channel's status. */
static status_t put_batch(struct fuzz *f)
{
    f->at = 4;
    f->nh = 0;
    if (below(f, 25) == 0) {
        for (unsigned i = below(f, 64); i > 0; i--)
            put(f, value(f));
    } else {
        for (unsigned i = 1 + below(f, FUZZ_MSGS); i > 0; i--)
            put_message(f);
    }
    f->words[0] = below(f, 30) ? JWL_MAGIC : value(f);
    f->words[1] = below(f, 30) ? f->nh : value(f);
    f->words[2] = f->words[3] = 0;
    status_t st = jam_channel_write(f->k.c->ch, f->words, f->at * 4, f->h, f->nh);
    if (st != OK)
        for (unsigned i = 0; i < f->nh; i++)
            jam_handle_close(f->h[i]);
    return st;
}

/* One round: seed's client set up, then its batches until the compositor
 * hangs up on it or the round ends; then good round-trips. */
static bool round_of(struct ct_comp *p, struct ct_client *good, uint32_t seed, unsigned r,
                     unsigned *hung_up)
{
    static struct fuzz f;
    if (!setup(p, &f, seed)) {
        ct_close(&f.k);
        if (f.vmo != HANDLE_INVALID)
            jam_handle_close(f.vmo);
        FAIL("round %u (seed %#x): the setup", r, seed);
    }
    unsigned batches = 1 + below(&f, FUZZ_BATCHES);
    for (unsigned b = 0; b < batches; b++) {
        status_t st = put_batch(&f);
        signals_t seen = 0;
        (void)jam_object_wait_one(f.k.c->ch, SIG_PEER_CLOSED, 0, &seen);
        if (seen & SIG_PEER_CLOSED) {
            (*hung_up)++;
            break;
        }
        if (st != OK && st != ERR_SHOULD_WAIT)
            break;
    }
    ct_close(&f.k);
    jam_handle_close(f.vmo);
    if (ct_roundtrip(good) != OK)
        FAIL("round %u (seed %#x): the healthy client got no answer", r, seed);
    return true;
}

static bool fuzz(struct ct_comp *p, unsigned rounds)
{
    static struct ct_client good;
    CHECK(ct_open(p, &good));
    CHECK(ct_bind_all(&good));
    uint64_t base_h = p->job ? ct_handles(p) : 0, base_m = p->job ? ct_msg_bytes(p) : 0;
    unsigned hung_up = 0;
    for (unsigned r = 0; r < rounds; r++)
        if (!round_of(p, &good, FUZZ_SEED + r, r, &hung_up)) {
            ct_close(&good);
            return false;
        }
    printf("utest: %s: %u rounds, %u hung up on\n", utest_cur, rounds, hung_up);
    CHECK_ST(ct_roundtrip(&good), OK);
    if (p->job) {   /* ours: everything the fuzzed clients had is let go */
        CHECK(ct_handles_back(p, base_h));
        CHECK(ct_msg_back(p, base_m));
    }
    ct_close(&good);
    return true;
}

bool t_comp_fuzz(void)
{
    struct ct_comp p;
    CHECK(ct_start_arg(&p, 320, 200, "testwin"));
    bool ok = fuzz(&p, FUZZ_ROUNDS);
    CHECK(ct_stop(&p));
    return ok;
}

bool t_comp_live_fuzz(void)
{
    struct ct_comp p = { .live = true };
    handle_t ch;
    if (ct_connect(&p, &ch) != OK) {
        printf("utest: %s: no /svc/wayland (a boot without the compositor): nothing to try\n",
               utest_cur);
        return true;
    }
    jam_handle_close(ch);
    return fuzz(&p, FUZZ_LIVE_ROUNDS);
}
