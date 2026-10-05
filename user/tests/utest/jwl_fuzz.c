/* utest: fuzzing libjwl (<jwl.h>). The decoder is fed random batches and
 * mutated good ones (both directions: requests to the compositor's map,
 * events to a client's), each placed so its last byte is the last byte
 * before an unmapped page: a read past the batch faults. Every message
 * taken must lie inside the batch, its strings and arrays too, use only
 * the batch's handles, and encode again to the same size; every refusal
 * must name an object and a wl_display code and use nothing up. Then the
 * transport: random and mutated batches with real handles written into a
 * compositor connection, which must take them or refuse them with the
 * error it wrote, and leave no handle or message byte behind.
 *
 * JWL_FUZZ_SCALE multiplies the counts (the build on the Mac with ASan
 * and UBSan runs it much higher). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwltest.h"
#include "utest.h"

#ifndef JWL_FUZZ_SCALE
#define JWL_FUZZ_SCALE 1
#endif

#define ROOM (2 * PAGE_SIZE)   /* mapped bytes before the hole */

struct tally {
    unsigned batches, msgs, refused;
    unsigned codes[4];
};

/* ---- making batches -------------------------------------------------------------- */

/* A word a hostile peer might choose: often small, sometimes a plausible
 * length or id, sometimes anything. */
static uint32_t any_word(uint32_t *seed)
{
    uint32_t x = jt_rng(seed);
    switch (x % 8) {
    case 0: case 1: return x >> 28;
    case 2: return (x >> 8) % 5000;
    case 3: return JWL_SERVER_ID_BASE + ((x >> 8) & 7);
    case 4: return 0x6c5f746a;   /* "jt_l" */
    default: return jt_rng(seed);
    }
}

/* Random messages: plausible headers, random words. */
static size_t random_batch(uint8_t *b, uint32_t *seed)
{
    struct jt_wr w = { .b = b };
    size_t want = jt_rng(seed) % 1024;
    while (w.n + 8 <= want) {
        uint32_t x = jt_rng(seed);
        jt_begin(&w, x & 1 ? 1 + (x >> 1) % 6 : any_word(seed), (uint16_t)((x >> 8) % 12));
        unsigned words = (x >> 16) % 12;
        for (unsigned i = 0; i < words && w.n + 4 <= want; i++)
            jt_w32(&w, any_word(seed));
        if (x >> 30)
            jt_end(&w);   /* else the size word is the random opcode's */
    }
    return w.n;
}

/* Good requests of every kind, ids made as a client would make them. */
static size_t good_requests(uint8_t *b, uint32_t *seed, unsigned *nh)
{
    struct jt_pair g;
    if (!jt_pair_init(&g, 3))
        return 0;
    struct jwl_out out = { .buf = b, .cap = ROOM, .hcap = JWL_BATCH_HANDLES };
    handle_t hs[JWL_BATCH_HANDLES];
    out.h = hs;
    static const uint8_t blob[5] = { 1, 2, 3, 4, 5 };
    for (unsigned k = 0, n = 1 + jt_rng(seed) % 6; k < n; k++) {
        union jwl_arg a[JWL_ARGS_MAX] = { { .u = jt_rng(seed) } };
        uint32_t op = jt_rng(seed) % 10, id = 3;
        const struct jwl_message *m = &jt_all.requests[op % 9];
        switch (op) {
        case JT_ARGS:
            a[3].s = op & 1 ? NULL : "fuzz";
            a[4].o = 4;
            a[5].a = (struct jwl_array){ sizeof(blob), blob };
            break;
        case JT_MAKE:
            (void)jwl_map_new(&g.client, &jt_thing, 3, NULL, &a[0].n);
            break;
        case JT_GIVE:
            a[0].h = 21;
            a[1].h = 22;
            break;
        case JT_STRS:
            a[0].s = "a";
            a[1].s = "longer string";
            break;
        case JT_ANY_OBJ:
            a[0].o = 1 + jt_rng(seed) % 4;
            break;
        case 9:   /* wl_registry.bind */
            id = 2;
            m = &jt_registry.requests[0];
            op = 0;
            a[1].any = (struct jwl_new_any){ jt_known[jt_rng(seed) % JT_NKNOWN], 1, 0 };
            (void)jwl_map_new(&g.client, a[1].any.iface, 1, NULL, &a[1].any.id);
            break;
        }
        struct jwl_sig sig;
        (void)jwl_sig_parse(m->signature, &sig);
        (void)jwl_encode(&out, m, id, (uint16_t)op, a, sig.n);
    }
    jt_pair_free(&g);
    *nh = out.nh;
    return out.len;
}

/* Good events: to jt_all, a thing, the registry, the display. */
static size_t good_events(uint8_t *b, uint32_t *seed, unsigned *nh)
{
    struct jwl_out out = { .buf = b, .cap = ROOM, .hcap = JWL_BATCH_HANDLES };
    handle_t hs[JWL_BATCH_HANDLES];
    out.h = hs;
    uint32_t next_server = JWL_SERVER_ID_BASE;
    for (unsigned k = 0, n = 1 + jt_rng(seed) % 6; k < n; k++) {
        union jwl_arg a[6] = { { .u = jt_rng(seed) } };
        uint32_t op = jt_rng(seed) % 7, id = 3;
        const struct jwl_message *m = op < 4 ? &jt_all.events[op] : NULL;
        if (op == JT_EV_ARGS) {
            a[3].s = "ev";
            a[4].o = 4;
        } else if (op == JT_EV_MAKE) {
            a[0].n = next_server++;
        } else if (op == JT_EV_HANDLE) {
            a[0].h = 31;
        } else if (op == 4) {
            id = 4, op = 0, m = &jt_thing.events[0];
        } else if (op == 5) {
            id = 2, op = 0, m = &jt_registry.events[0], a[1].s = "jt_all";
        } else if (op == 6) {
            id = 1, op = 0, m = &jt_display.events[0], a[0].o = 3, a[2].s = "an error";
        }
        struct jwl_sig sig;
        (void)jwl_sig_parse(m->signature, &sig);
        (void)jwl_encode(&out, m, id, (uint16_t)op, a, sig.n);
    }
    *nh = out.nh;
    return out.len;
}

/* Up to four changes: a byte, a word, a cut, a word in or out, the handles. */
static size_t mutate(uint8_t *b, size_t n, unsigned *nh, uint32_t *seed)
{
    for (unsigned k = 0, times = 1 + jt_rng(seed) % 4; k < times && n; k++) {
        uint32_t x = jt_rng(seed), at = (x >> 8) % (uint32_t)n;
        switch (x % 6) {
        case 0: b[at] ^= (uint8_t)(1u << (x >> 29)); break;
        case 1: b[at] = (uint8_t)(x >> 16); break;
        case 2: if ((at & ~3u) + 4 <= n) { uint32_t v = any_word(seed); memcpy(b + (at & ~3u), &v, 4); } break;
        case 3: n = at; break;
        case 4: if (n + 4 <= ROOM) { memmove(b + (at & ~3u) + 4, b + (at & ~3u), n - (at & ~3u)); n += 4; } break;
        case 5: *nh = (*nh + (x >> 16) % 3 + JWL_BATCH_HANDLES - 1) % JWL_BATCH_HANDLES; break;
        }
    }
    return n;
}

/* ---- checking ---------------------------------------------------------------------- */

static bool inside(const void *p, size_t len, const uint8_t *lo, const uint8_t *hi)
{
    const uint8_t *q = p;
    return q >= lo && q <= hi && len <= (size_t)(hi - q);
}

/* A message taken from b[at .. end): every argument inside it, and it
 * encodes again to its own size. */
static bool taken_ok(const struct jwl_msg *m, const uint8_t *lo, const uint8_t *hi)
{
    struct jwl_sig sig;
    CHECK_ST(jwl_sig_parse(m->msg->signature, &sig), OK);
    CHECK_EQ(m->nargs, sig.n);
    for (unsigned i = 0; i < sig.n; i++) {
        const union jwl_arg *a = &m->args[i];
        if (sig.type[i] == 's' && a->s)
            CHECK(inside(a->s, strnlen(a->s, (size_t)(hi - (const uint8_t *)a->s)) + 1, lo, hi));
        if (sig.type[i] == 'a' && a->a.size)
            CHECK(inside(a->a.data, a->a.size, lo, hi));
        if (sig.type[i] == 'n' && !(m->msg->types && m->msg->types[i]))
            CHECK(a->any.iface && inside(a->any.iface, 0, (const uint8_t *)0, (const uint8_t *)-1));
    }
    static uint8_t again[JWL_MSG_MAX];
    handle_t hs[JWL_ARGS_MAX];
    struct jwl_out out = { .buf = again, .cap = sizeof(again), .h = hs, .hcap = JWL_ARGS_MAX };
    CHECK_ST(jwl_encode(&out, m->msg, m->id, m->opcode, m->args, m->nargs), OK);
    CHECK_EQ(out.len, (size_t)(hi - lo));
    return true;
}

/* Decode all of b[0..n) with nh handles on side's map of a fresh pair. */
static bool decode_all(const uint8_t *b, size_t n, unsigned nh, enum jwl_side side,
                       struct tally *t)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jwl_map *map = side == JWL_SERVER ? &p.server : &p.client;
    handle_t hs[JWL_BATCH_HANDLES];
    for (unsigned i = 0; i < JWL_BATCH_HANDLES; i++)
        hs[i] = 1000 + i;
    struct jwl_in in = { .buf = b, .len = n, .h = hs, .nh = nh };
    struct jwl_msg m;
    struct jwl_error err;
    t->batches++;
    for (unsigned guard = 0; guard <= ROOM / 8; guard++) {
        size_t at = in.at;
        unsigned hat = in.hat;
        status_t st = jwl_decode(&in, map, &m, &err);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st != OK) {
            CHECK_ST(st, ERR_INVALID_ARGS);
            CHECK(err.code <= JWL_ERROR_IMPLEMENTATION && err.object != 0);
            CHECK(in.at == at && in.hat == hat);
            t->refused++;
            t->codes[err.code]++;
            break;
        }
        CHECK(in.at > at && in.at <= n && (in.at - at) % 4 == 0 && in.at - at <= JWL_MSG_MAX);
        CHECK(in.hat <= nh && m.nhandles == in.hat - hat);
        if (!taken_ok(&m, b + at, b + in.at))
            FAIL("message %u.%u from byte %u", m.id, m.opcode, (unsigned)at);
        t->msgs++;
    }
    jt_pair_free(&p);
    return true;
}

/* Two pages mapped with a hole after them. */
static uint8_t *guarded(void)
{
    handle_t vmo, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t addr;
    if (jam_vmo_create(ROOM + PAGE_SIZE, 0, HANDLE_INVALID, &vmo) != OK)
        return NULL;
    status_t st = jam_vmar_map(vmar, vmo, 0, ROOM + PAGE_SIZE, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);
    if (st != OK)
        return NULL;
    if (jam_vmar_unmap(vmar, addr + ROOM, PAGE_SIZE) != OK)
        return NULL;
    return (uint8_t *)(uintptr_t)addr;
}

bool t_jwl_fuzz_decode(void)
{
    uint8_t *page = guarded();
    CHECK(page != NULL);
    static uint8_t b[ROOM];
    uint32_t seed = 0x4a574c31u;
    struct tally t = { 0 };
    unsigned nrandom = 6000 * JWL_FUZZ_SCALE, nmutated = 12000 * JWL_FUZZ_SCALE;
    for (unsigned i = 0; i < nrandom + 2 * nmutated; i++) {
        unsigned nh = jt_rng(&seed) % 4;
        size_t n;
        enum jwl_side side = i & 1 ? JWL_SERVER : JWL_CLIENT;
        if (i < nrandom) {
            n = random_batch(b, &seed);
        } else {
            side = i < nrandom + nmutated ? JWL_SERVER : JWL_CLIENT;
            n = side == JWL_SERVER ? good_requests(b, &seed, &nh) : good_events(b, &seed, &nh);
            if (jt_rng(&seed) % 8)   /* some go unchanged */
                n = mutate(b, n, &nh, &seed);
        }
        uint8_t *at = page + ROOM - n;   /* the batch ends at the hole */
        memcpy(at, b, n);
        if (!decode_all(at, n, nh, side, &t))
            FAIL("batch %u (%u bytes, %u handles)", i, (unsigned)n, nh);
    }
    printf("utest: jwl_fuzz_decode: %u batches (%u random), %u messages taken, %u refused "
           "(invalid_object %u, invalid_method %u, no_memory %u, implementation %u)\n",
           t.batches, nrandom, t.msgs, t.refused, t.codes[0], t.codes[1], t.codes[2], t.codes[3]);
    CHECK(t.msgs > t.batches / 2 && t.refused > t.batches / 4 && t.codes[3] == 0);
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)page, ROOM), OK);
    return true;
}

/* ---- the transport ----------------------------------------------------------------- */

/* One batch (a JWL1 header unless mutated, then b[0..n)) with nh fresh
 * VMOs into a fresh compositor connection: it takes every message or dies
 * with the error it wrote. */
static bool feed_conn(const uint8_t *b, size_t n, unsigned nh, uint32_t *seed, struct tally *t)
{
    static uint8_t raw[JWL_BATCH_MAX];
    uint32_t hdr[4] = { JWL_MAGIC, 0, nh, 0 };
    if (jt_rng(seed) % 16 == 0)
        hdr[jt_rng(seed) % 4] ^= 1u << (jt_rng(seed) % 32);
    if (n > sizeof(raw) - 16)
        n = sizeof(raw) - 16;
    memcpy(raw, hdr, 16);
    memcpy(raw + 16, b, n);
    handle_t a, c, hs[JWL_BATCH_HANDLES];
    CHECK_ST(jam_channel_create(&a, &c), OK);
    struct jwl_conn *sv;
    struct jwl_conn_config cfg = { .ch = c, .side = JWL_SERVER, .display = &jt_display,
                                   .known = jt_known, .nknown = JT_NKNOWN };
    CHECK_ST(jwl_conn_create(&cfg, &sv), OK);
    for (unsigned i = 0; i < nh; i++)
        CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &hs[i]), OK);
    CHECK_ST(jam_channel_write(a, raw, (uint32_t)(n + 16), hs, nh), OK);
    struct jwl_msg m;
    status_t st;
    while ((st = jwl_conn_next(sv, &m)) == OK) {
        jwl_msg_close_handles(&m);
        t->msgs++;
    }
    t->batches++;
    if (st != ERR_SHOULD_WAIT) {
        CHECK_ST(st, ERR_INVALID_ARGS);
        CHECK(sv->error.code <= JWL_ERROR_IMPLEMENTATION);
        t->refused++;
        t->codes[sv->error.code]++;
        uint32_t back[8], nb, nhb;
        struct channel_read_args r = { .h = a, .bytes_cap = sizeof(back),
                                       .bytes = (uint64_t)(uintptr_t)back,
                                       .actual_bytes = (uint64_t)(uintptr_t)&nb,
                                       .actual_handles = (uint64_t)(uintptr_t)&nhb };
        status_t rs = jam_channel_read(&r);   /* the error is the only message back */
        CHECK(rs == OK || rs == ERR_BUFFER_TOO_SMALL);
        CHECK(back[0] == JWL_MAGIC && back[4] == JWL_DISPLAY_ID && back[7] == sv->error.code);
    }
    jwl_conn_destroy(sv);
    jam_handle_close(a);
    return true;
}

bool t_jwl_fuzz_conn(void)
{
    struct job_info ji;
    CHECK_ST(jam_job_get_info(own_job(), &ji), OK);
    uint64_t h0 = ji.used[JOB_LIMIT_HANDLES], b0 = ji.used[JOB_LIMIT_MSG_BYTES];
    static uint8_t b[ROOM];
    uint32_t seed = 0x434f4e4eu;
    struct tally t = { 0 };
    unsigned count = 1500 * JWL_FUZZ_SCALE;
    for (unsigned i = 0; i < count; i++) {
        unsigned nh = 0;
        size_t n = i % 3 == 0 ? random_batch(b, &seed) : good_requests(b, &seed, &nh);
        if (i % 3 && jt_rng(&seed) % 4)
            n = mutate(b, n, &nh, &seed);
        if (!feed_conn(b, n, nh, &seed, &t))
            FAIL("batch %u (%u bytes, %u handles)", i, (unsigned)n, nh);
    }
    printf("utest: jwl_fuzz_conn: %u batches, %u messages taken, %u refused "
           "(invalid_object %u, invalid_method %u, no_memory %u)\n",
           t.batches, t.msgs, t.refused, t.codes[0], t.codes[1], t.codes[2]);
    CHECK(t.refused > count / 4 && t.msgs > count && t.codes[3] == 0);
    CHECK_ST(jam_job_get_info(own_job(), &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_HANDLES], h0);
    CHECK_EQ(ji.used[JOB_LIMIT_MSG_BYTES], b0);
    return true;
}
