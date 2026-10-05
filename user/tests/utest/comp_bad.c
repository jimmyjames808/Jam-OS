/* utest: bin/compositor against clients that break the rules (comptest.h).
 *
 * t_comp_bad_requests: one connection per bad request (pools that aren't
 * kept VMOs, aren't VMOs, lack a right or overrun theirs; buffers outside
 * their pool, of an unknown format, with a short stride; pools shrunk or
 * grown past their VMO; scales and transforms; binds of globals not
 * offered, by the wrong name, too new; unknown objects and opcodes, a
 * request newer than its object, a batch that isn't one). Each gets
 * wl_display.error naming the right object and code, while a healthy
 * client keeps round-tripping; once all are closed, the compositor holds
 * exactly the handles it held before them.
 * t_comp_caps: every per-client cap refused with no_memory at its limit
 * (surfaces, pools, pool bytes, buffers, frame callbacks, regions, boxes
 * in a region and in all of a client's), the client alone disconnected.
 * t_comp_connections: COMP_CLIENTS_MAX connections, then refusals; a
 * client disconnected for an error keeps its slot until it closes its end.
 * t_comp_client_crash: a client process that crashes holding a pool, a
 * buffer and a committed surface: its job empties (the compositor let go
 * of its VMO) and the compositor's handles come back.
 * t_comp_never_reads: a client that floods requests and reads nothing is
 * disconnected with no_memory after the window and the held bytes; the
 * healthy client is served throughout. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl/wayland.h>
#include <os.h>
#include "comptest.h"
#include "utest.h"

#define SHM_INVALID_FORMAT JWL_WL_SHM_ERROR_INVALID_FORMAT
#define SHM_INVALID_STRIDE JWL_WL_SHM_ERROR_INVALID_STRIDE
#define SHM_INVALID_FD     JWL_WL_SHM_ERROR_INVALID_FD

/* ---- raw batches ------------------------------------------------------------------- */

/* One batch of n words of messages behind a proper header (or, with
 * header false, just the words: no batch at all). */
static status_t raw(struct ct_client *k, const uint32_t *words, unsigned n, bool header)
{
    uint32_t b[32];
    unsigned at = 0;
    if (header) {
        b[0] = JWL_MAGIC;
        b[1] = k->c->nread;
        b[2] = b[3] = 0;
        at = 4;
    }
    for (unsigned i = 0; i < n && at < 32; i++)
        b[at++] = words[i];
    return jam_channel_write(k->c->ch, b, at * 4, NULL, 0);
}

static uint32_t msg_word(uint32_t bytes, uint32_t opcode)
{
    return bytes << 16 | opcode;
}

/* ---- the bad requests -------------------------------------------------------------- */

/* Each makes one bad request on k (bound: ct_bind_all) and says which
 * object the error must name, with which code. */
typedef bool (*bad_fn)(struct ct_client *k, uint32_t *obj, uint32_t *code);

/* wl_shm.create_pool with handle h (consumed) and size. */
static bool pool_with(struct ct_client *k, handle_t h, int32_t size)
{
    uint32_t id = ct_new(k, &jwl_wl_shm_pool_interface, 1);
    CHECK(id && h != HANDLE_INVALID);
    CHECK_ST(jwl_wl_shm_create_pool(k->c, k->shm, id, h, size), OK);
    return true;
}

static bool bad_plain_vmo(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    handle_t v;
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &v), OK);
    handle_t d = ct_dup_for_pool(v);
    jam_handle_close(v);
    *obj = k->shm;
    *code = SHM_INVALID_FD;   /* a VMO whose pages the client could take away */
    return pool_with(k, d, PAGE_SIZE);
}

static bool bad_not_a_vmo(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    jam_handle_close(b);
    *obj = k->shm;
    *code = SHM_INVALID_FD;
    return pool_with(k, a, PAGE_SIZE);
}

static bool bad_no_map_right(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    handle_t v = ct_kept_vmo(PAGE_SIZE), d = HANDLE_INVALID;
    CHECK(v != HANDLE_INVALID);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_READ | RIGHT_TRANSFER, &d), OK);
    jam_handle_close(v);
    *obj = k->shm;
    *code = SHM_INVALID_FD;
    return pool_with(k, d, PAGE_SIZE);
}

static bool bad_past_vmo(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    handle_t v = ct_kept_vmo(PAGE_SIZE), d = v ? ct_dup_for_pool(v) : HANDLE_INVALID;
    jam_handle_close(v);
    *obj = k->shm;
    *code = SHM_INVALID_FD;
    return pool_with(k, d, 2 * PAGE_SIZE);
}

static bool bad_pool_size(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    handle_t v = ct_kept_vmo(PAGE_SIZE), d = v ? ct_dup_for_pool(v) : HANDLE_INVALID;
    jam_handle_close(v);
    *obj = k->shm;
    *code = SHM_INVALID_STRIDE;
    return pool_with(k, d, 0);
}

/* wl_shm_pool.create_buffer on a new 64 KiB pool with these numbers. */
static bool buffer_with(struct ct_client *k, uint32_t *obj, int32_t offset, int32_t w, int32_t h,
                        int32_t stride, uint32_t format)
{
    uint32_t pool = ct_pool(k, 65536, NULL), id = ct_new(k, &jwl_wl_buffer_interface, 1);
    CHECK(pool && id);
    CHECK_ST(jwl_wl_shm_pool_create_buffer(k->c, pool, id, offset, w, h, stride, format), OK);
    *obj = pool;
    return true;
}

static bool bad_buffer_past_end(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_STRIDE;   /* the last row ends 1 KiB past the pool */
    return buffer_with(k, obj, 65536 - 64 * 256 + 1024, 64, 64, 256, JWL_WL_SHM_FORMAT_XRGB8888);
}

static bool bad_buffer_overflow(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_STRIDE;   /* offset + stride * height overflows 32 bits */
    return buffer_with(k, obj, 0x7ffffffc, 8192, 8192, 0x7ffffffc, JWL_WL_SHM_FORMAT_ARGB8888);
}

static bool bad_buffer_format(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_FORMAT;
    return buffer_with(k, obj, 0, 8, 8, 32, JWL_WL_SHM_FORMAT_XRGB8888_A8);
}

static bool bad_buffer_stride(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_STRIDE;
    return buffer_with(k, obj, 0, 64, 64, 252, JWL_WL_SHM_FORMAT_XRGB8888);
}

static bool bad_buffer_width(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_STRIDE;
    return buffer_with(k, obj, 0, 8193, 1, 8193 * 4, JWL_WL_SHM_FORMAT_XRGB8888);
}

static bool bad_buffer_offset(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = SHM_INVALID_STRIDE;
    return buffer_with(k, obj, -4, 1, 1, 4, JWL_WL_SHM_FORMAT_XRGB8888);
}

static bool bad_resize_shrink(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    uint32_t pool = ct_pool(k, 65536, NULL);
    CHECK(pool);
    CHECK_ST(jwl_wl_shm_pool_resize(k->c, pool, 4096), OK);
    *obj = pool;
    *code = SHM_INVALID_STRIDE;
    return true;
}

static bool bad_resize_past_vmo(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    uint32_t pool = ct_pool(k, PAGE_SIZE, NULL);
    CHECK(pool);
    CHECK_ST(jwl_wl_shm_pool_resize(k->c, pool, 2 * PAGE_SIZE), OK);   /* the VMO didn't grow */
    *obj = pool;
    *code = SHM_INVALID_FD;
    return true;
}

/* A surface on k, for the surface cases. */
static uint32_t surface(struct ct_client *k)
{
    uint32_t s = ct_new(k, &jwl_wl_surface_interface, 4);
    if (s && jwl_wl_compositor_create_surface(k->c, k->compositor, s) != OK)
        return 0;
    return s;
}

static bool bad_scale_2(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    CHECK((*obj = surface(k)) != 0);
    *code = JWL_ERROR_IMPLEMENTATION;
    CHECK_ST(jwl_wl_surface_set_buffer_scale(k->c, *obj, 2), OK);
    return true;
}

static bool bad_scale_0(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    CHECK((*obj = surface(k)) != 0);
    *code = JWL_WL_SURFACE_ERROR_INVALID_SCALE;
    CHECK_ST(jwl_wl_surface_set_buffer_scale(k->c, *obj, 0), OK);
    return true;
}

static bool bad_transform_9(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    CHECK((*obj = surface(k)) != 0);
    *code = JWL_WL_SURFACE_ERROR_INVALID_TRANSFORM;
    CHECK_ST(jwl_wl_surface_set_buffer_transform(k->c, *obj, 9), OK);
    return true;
}

static bool bad_transform_90(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    CHECK((*obj = surface(k)) != 0);
    *code = JWL_ERROR_IMPLEMENTATION;
    CHECK_ST(jwl_wl_surface_set_buffer_transform(k->c, *obj, 1), OK);
    return true;
}

/* wl_registry.bind of name as iface (whose table is t) at version. */
static bool bind_as(struct ct_client *k, uint32_t *obj, uint32_t name,
                    const struct jwl_interface *t, uint32_t version)
{
    uint32_t id = ct_new(k, t, version);
    CHECK(id);
    CHECK_ST(jwl_wl_registry_bind(k->c, k->registry, name, t->name, version, id), OK);
    *obj = k->registry;
    return true;
}

static bool bad_bind_seat(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = JWL_ERROR_INVALID_OBJECT;   /* we offer version 5 */
    return bind_as(k, obj, 4, &jwl_wl_seat_interface, 6);
}

static bool bad_bind_name(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = JWL_ERROR_INVALID_OBJECT;   /* global 1 is wl_compositor */
    return bind_as(k, obj, 1, &jwl_wl_shm_interface, 1);
}

static bool bad_bind_newer(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    *code = JWL_ERROR_INVALID_OBJECT;   /* we offer version 4 */
    return bind_as(k, obj, 1, &jwl_wl_compositor_interface, 5);
}

static bool bad_unknown_object(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    const uint32_t m[] = { 777, msg_word(8, 0) };
    *obj = JWL_DISPLAY_ID;
    *code = JWL_ERROR_INVALID_OBJECT;
    CHECK_ST(raw(k, m, 2, true), OK);
    return true;
}

static bool bad_opcode(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    const uint32_t m[] = { k->registry, msg_word(8, 5) };
    *obj = k->registry;
    *code = JWL_ERROR_INVALID_METHOD;
    CHECK_ST(raw(k, m, 2, true), OK);
    return true;
}

static bool bad_too_new(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    CHECK((*obj = surface(k)) != 0);
    CHECK_ST(jwl_conn_flush(k->c), OK);   /* the surface first, in a batch of its own */
    const uint32_t m[] = { *obj, msg_word(16, JWL_WL_SURFACE_REQ_OFFSET), 1, 1 };
    *code = JWL_ERROR_INVALID_METHOD;   /* wl_surface.offset is version 5's */
    CHECK_ST(raw(k, m, 4, true), OK);
    return true;
}

static bool bad_batch(struct ct_client *k, uint32_t *obj, uint32_t *code)
{
    const uint32_t m[] = { 0x6c6c6568, 0x0000006f };   /* "hello": no JWL1 header */
    *obj = JWL_DISPLAY_ID;
    *code = JWL_ERROR_INVALID_METHOD;
    CHECK_ST(raw(k, m, 2, false), OK);
    return true;
}

static const struct {
    const char *name;
    bad_fn fn;
} bad[] = {
    { "a plain VMO", bad_plain_vmo }, { "a channel for a pool", bad_not_a_vmo },
    { "no map right", bad_no_map_right }, { "a pool past its VMO", bad_past_vmo },
    { "a pool of 0 bytes", bad_pool_size }, { "a buffer past its pool", bad_buffer_past_end },
    { "a buffer's size overflowing", bad_buffer_overflow },
    { "an unknown format", bad_buffer_format }, { "a short stride", bad_buffer_stride },
    { "a buffer 8193 wide", bad_buffer_width }, { "a negative offset", bad_buffer_offset },
    { "a pool shrunk", bad_resize_shrink }, { "a pool grown past its VMO", bad_resize_past_vmo },
    { "scale 2", bad_scale_2 }, { "scale 0", bad_scale_0 }, { "transform 9", bad_transform_9 },
    { "transform 90", bad_transform_90 }, { "a seat too new", bad_bind_seat },
    { "a bind by the wrong name", bad_bind_name }, { "a bind too new", bad_bind_newer },
    { "an unknown object", bad_unknown_object }, { "an unknown opcode", bad_opcode },
    { "a request too new", bad_too_new }, { "a batch that isn't one", bad_batch },
};

bool t_comp_bad_requests(void)
{
    static struct ct_client good, k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));
    uint64_t base = ct_handles(&p);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t obj = 0, code = 0;
        CHECK(ct_open(&p, &k));
        CHECK(ct_bind_all(&k));
        if (!bad[i].fn(&k, &obj, &code) || !ct_expect_error(&k, obj, code))
            FAIL("%s: not refused as it should be", bad[i].name);
        ct_close(&k);
        CHECK_ST(ct_roundtrip(&good), OK);   /* the others are served on */
    }
    CHECK(ct_handles_back(&p, base));
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}

/* ---- caps ------------------------------------------------------------------------ */

/* n requests that each make something on k succeed (a round trip says so);
 * the next is refused with no_memory naming obj. make(k, i) sends one. */
static bool cap_at(struct ct_client *k, unsigned n, bool (*make)(struct ct_client *, unsigned),
                   uint32_t obj)
{
    for (unsigned i = 0; i < n; i++)
        CHECK(make(k, i));
    CHECK_ST(ct_roundtrip(k), OK);
    CHECK(make(k, n));
    return ct_expect_error(k, obj, JWL_ERROR_NO_MEMORY);
}

static bool make_surface(struct ct_client *k, unsigned i)
{
    (void)i;
    return surface(k) != 0;
}

static handle_t cap_vmo;      /* the VMO every pool of a cap test is made from */
static uint32_t cap_size;     /* each pool's size */
static uint32_t cap_target;   /* the pool, surface or region the next request is on */

static bool make_pool(struct ct_client *k, unsigned i)
{
    (void)i;
    return pool_with(k, ct_dup_for_pool(cap_vmo), (int32_t)cap_size);
}

static bool make_buffer(struct ct_client *k, unsigned i)
{
    uint32_t id = ct_new(k, &jwl_wl_buffer_interface, 1);
    return id && jwl_wl_shm_pool_create_buffer(k->c, cap_target, id, (int32_t)(i % 1024) * 4, 1,
                                               1, 4, JWL_WL_SHM_FORMAT_ARGB8888) == OK;
}

static bool make_frame(struct ct_client *k, unsigned i)
{
    (void)i;
    uint32_t cb = ct_new(k, &jwl_wl_callback_interface, 4);
    return cb && jwl_wl_surface_frame(k->c, cap_target, cb) == OK;
}

static bool make_region(struct ct_client *k, unsigned i)
{
    (void)i;
    uint32_t r = ct_new(k, &jwl_wl_region_interface, 4);
    return r && jwl_wl_compositor_create_region(k->c, k->compositor, r) == OK;
}

static bool make_box(struct ct_client *k, unsigned i)
{
    return jwl_wl_region_add(k->c, cap_target, (int32_t)(i % 256) * 2, (int32_t)(i / 256) * 2,
                             1, 1) == OK;   /* never touching: one box each */
}

/* Which object a cap's refusal names. */
enum who { BY_COMPOSITOR, BY_SHM, BY_TARGET };

/* One cap on a fresh connection of p. */
static bool one_cap(struct ct_comp *p, struct ct_client *k, const char *what,
                    bool (*setup)(struct ct_client *), unsigned n,
                    bool (*make)(struct ct_client *, unsigned), enum who who)
{
    CHECK(ct_open(p, k));
    CHECK(ct_bind_all(k));
    if (setup && !setup(k))
        FAIL("%s: setting up", what);
    uint32_t obj = who == BY_COMPOSITOR ? k->compositor : who == BY_SHM ? k->shm : cap_target;
    if (!cap_at(k, n, make, obj))
        FAIL("%s: the cap of %u not kept", what, n);
    ct_close(k);
    return true;
}

static bool on_pool(struct ct_client *k)
{
    return (cap_target = ct_pool(k, PAGE_SIZE, NULL)) != 0;
}

static bool on_surface(struct ct_client *k)
{
    return (cap_target = surface(k)) != 0;
}

static bool on_region(struct ct_client *k)
{
    return (cap_target = ct_new(k, &jwl_wl_region_interface, 4)) != 0 &&
           jwl_wl_compositor_create_region(k->c, k->compositor, cap_target) == OK;
}

/* 16 regions of 256 boxes: the client's 4096; then one box more. */
static bool all_boxes(struct ct_client *k)
{
    for (unsigned r = 0; r < 16; r++) {
        CHECK(on_region(k));
        for (unsigned i = 0; i < 256; i++)
            CHECK(make_box(k, i));
        CHECK_ST(ct_roundtrip(k), OK);
    }
    return on_region(k);
}

static bool pool_caps(struct ct_comp *p, struct ct_client *k)
{
    cap_size = PAGE_SIZE;
    CHECK((cap_vmo = ct_kept_vmo(cap_size)) != HANDLE_INVALID);
    CHECK(one_cap(p, k, "pools", NULL, 64, make_pool, BY_SHM));
    CHECK(one_cap(p, k, "buffers", on_pool, 256, make_buffer, BY_TARGET));
    jam_handle_close(cap_vmo);
    cap_size = 16u << 20;   /* 16 of them are 256 MiB */
    CHECK((cap_vmo = ct_kept_vmo(cap_size)) != HANDLE_INVALID);
    CHECK(one_cap(p, k, "pool bytes", NULL, 16, make_pool, BY_SHM));
    jam_handle_close(cap_vmo);
    return true;
}

bool t_comp_caps(void)
{
    static struct ct_client good, k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));   /* the compositor is up: its handles are its own now */
    uint64_t base = ct_handles(&p);
    CHECK(one_cap(&p, &k, "surfaces", NULL, 64, make_surface, BY_COMPOSITOR));
    CHECK(pool_caps(&p, &k));
    CHECK(one_cap(&p, &k, "frame callbacks", on_surface, 64, make_frame, BY_TARGET));
    CHECK(one_cap(&p, &k, "regions", NULL, 64, make_region, BY_COMPOSITOR));
    CHECK(one_cap(&p, &k, "boxes in a region", on_region, 256, make_box, BY_TARGET));
    CHECK(one_cap(&p, &k, "boxes in all", all_boxes, 0, make_box, BY_TARGET));
    CHECK_ST(ct_roundtrip(&good), OK);
    CHECK(ct_handles_back(&p, base));
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}

/* ---- connections ------------------------------------------------------------------ */

bool t_comp_connections(void)
{
    static handle_t ch[64];
    static struct ct_client good, k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));
    uint64_t base = ct_handles(&p);
    for (unsigned i = 0; i < 62; i++)
        CHECK_ST(svc_connect_within(p.svc, CT_WAIT, &ch[i]), OK);
    CHECK(ct_open(&p, &k));   /* the 64th, with good */
    handle_t extra = HANDLE_INVALID;
    CHECK_ST(svc_connect_within(p.svc, CT_WAIT, &extra), ERR_NO_RESOURCES);
    /* disconnected for an error, but not closed: its slot stays taken */
    uint32_t obj, code;
    CHECK(bad_batch(&k, &obj, &code));
    CHECK(ct_expect_error(&k, obj, code));
    CHECK_ST(svc_connect_within(p.svc, CT_WAIT, &extra), ERR_NO_RESOURCES);
    ct_close(&k);
    uint64_t until = now() + CT_WAIT;   /* the compositor sees the close on its next turn */
    status_t st;
    while ((st = svc_connect_within(p.svc, CT_WAIT, &extra)) == ERR_NO_RESOURCES &&
           now() < until)
        jam_nanosleep(now() + NS_PER_MS);
    CHECK_ST(st, OK);
    jam_handle_close(extra);
    for (unsigned i = 0; i < 62; i++)
        jam_handle_close(ch[i]);
    CHECK_ST(ct_roundtrip(&good), OK);
    CHECK(ct_handles_back(&p, base));
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}

/* ---- a client that crashes --------------------------------------------------------- */

/* The child's client: a pool, a buffer, a surface showing it with a frame
 * callback pending, all seen by the compositor (a round trip); then a
 * crash. Exits 1 if it couldn't get that far. */
static int crash_client(void)
{
    static struct ct_client k;
    handle_t ch = startup_handle(SR_USER);
    uint32_t pool, buf, s, cb;
    /* each new id announced before the next is made: the compositor takes
     * them in order only */
    if (ch == HANDLE_INVALID || !ct_adopt(ch, &k) || !ct_bind_all(&k) ||
        !(pool = ct_pool(&k, 65536, NULL)) || !(buf = ct_new(&k, &jwl_wl_buffer_interface, 1)) ||
        jwl_wl_shm_pool_create_buffer(k.c, pool, buf, 0, 64, 64, 256,
                                      JWL_WL_SHM_FORMAT_XRGB8888) != OK ||
        !(s = surface(&k)) || jwl_wl_surface_attach(k.c, s, buf, 0, 0) != OK ||
        jwl_wl_surface_commit(k.c, s) != OK ||
        !(cb = ct_new(&k, &jwl_wl_callback_interface, 4)) ||
        jwl_wl_surface_frame(k.c, s, cb) != OK || ct_roundtrip(&k) != OK)
        return 1;
    *(volatile int *)0 = 1;   /* a crash: the kernel kills us */
    return 2;
}

int comp_child(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "wl-crash"))
        return crash_client();
    printf("utest: unknown mode \"%s\"\n", argv[1]);
    return 127;
}

bool t_comp_client_crash(void)
{
    static struct ct_client good;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));
    uint64_t base = ct_handles(&p);
    handle_t ch, job, proc;
    CHECK_ST(svc_connect_within(p.svc, CT_WAIT, &ch), OK);
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(child("wl-crash", NULL, job, ch, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, CT_WAIT, &info), OK);
    CHECK(info.killed);   /* it got as far as the crash */
    CHECK_ST(jam_handle_close(proc), OK);
    /* its pool's pages are its job's until the compositor lets the VMO go */
    uint64_t until = now() + CT_WAIT;
    for (unsigned kind = 1; kind < JOB_LIMIT_COUNT; kind++) {
        while (ct_job_used(job, kind) && now() < until)
            jam_nanosleep(now() + NS_PER_MS);
        if (ct_job_used(job, kind))
            FAIL("the crashed client's job keeps %lu units of kind %u",
                 (unsigned long)ct_job_used(job, kind), kind);
    }
    CHECK_ST(jam_handle_close(job), OK);
    CHECK(ct_handles_back(&p, base));
    CHECK_ST(ct_roundtrip(&good), OK);
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}

/* ---- a client that never reads ----------------------------------------------------- */

#define FLOOD 3800u   /* registries: 6 globals each, ~680 KiB of events, past 32 batches + 64 KiB */

bool t_comp_never_reads(void)
{
    static struct ct_client good, k;
    struct ct_comp p;
    CHECK(ct_start(&p, 320, 200));
    CHECK(ct_open(&p, &good));
    CHECK(ct_bind_all(&good));
    uint64_t base = ct_handles(&p);
    CHECK(ct_open(&p, &k));
    for (unsigned i = 0; i < FLOOD; i++) {
        uint32_t r = ct_new(&k, &jwl_wl_registry_interface, 1);
        CHECK(r);
        CHECK_ST(jwl_wl_display_get_registry(k.c, JWL_DISPLAY_ID, r), OK);
    }
    CHECK_ST(jwl_conn_flush(k.c), OK);   /* nothing read: no acknowledgement in it */
    CHECK_ST(ct_roundtrip(&good), OK);   /* served meanwhile */
    /* Read without ever acknowledging: the window's batches, then the error. */
    uint64_t deadline = now() + CT_WAIT;
    unsigned events = 0;
    for (;;) {
        struct jwl_msg m;
        status_t st = jwl_conn_next(k.c, &m);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            CHECK_ST(jam_object_wait_one(k.c->ch, SIG_READABLE, deadline, &seen), OK);
            continue;
        }
        CHECK_ST(st, OK);
        if (m.iface == &jwl_wl_display_interface && m.opcode == JWL_WL_DISPLAY_EV_ERROR) {
            CHECK_EQ(m.args[0].o, JWL_DISPLAY_ID);
            CHECK_EQ(m.args[1].u, JWL_ERROR_NO_MEMORY);
            break;
        }
        events++;
    }
    CHECK(events < FLOOD * 6);   /* cut off before everything was sent */
    CHECK(k.c->nread <= JWL_WINDOW + 1);   /* the window, and the error past it */
    ct_close(&k);
    CHECK_ST(ct_roundtrip(&good), OK);
    CHECK(ct_handles_back(&p, base));
    ct_close(&good);
    CHECK(ct_stop(&p));
    return true;
}
