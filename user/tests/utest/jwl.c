/* utest: libjwl's codec and object map (<jwl.h>) against a hand-written
 * stand-in for the generated tables (jwltest.h): the tables' and
 * signatures' checks, every argument letter round-tripped both ways, the
 * size limits, and the id rules of both ends' maps (next or free, the
 * delete_id handshake, zombies, the caps). The malformed messages are in
 * jwl_bad.c, the transport in jwl_conn.c, the fuzzing in jwl_fuzz.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwltest.h"
#include "utest.h"

/* ---- the stand-in tables ---------------------------------------------------------- */

static const struct jwl_interface *const t_cb[] = { &jt_callback };
static const struct jwl_interface *const t_reg[] = { &jt_registry };
static const struct jwl_interface *const t_thing[] = { &jt_thing };
static const struct jwl_interface *const t_bind[] = { NULL, NULL };
static const struct jwl_interface *const t_args[] = { NULL, NULL, NULL, NULL, &jt_thing, NULL };
static const struct jwl_interface *const t_ev_args[] = { NULL, NULL, NULL, NULL, &jt_thing, NULL };

static const struct jwl_message display_requests[] = {
    { "sync", "n", t_cb },
    { "get_registry", "n", t_reg },
};
static const struct jwl_message display_events[] = {
    { "error", "ous", NULL },
    { "delete_id", "u", NULL },
};
const struct jwl_interface jt_display = { "wl_display", 1, 2, 2, display_requests,
                                          display_events };

static const struct jwl_message registry_requests[] = { { "bind", "un", t_bind } };
static const struct jwl_message registry_events[] = {
    { "global", "usu", NULL },
    { "global_remove", "u", NULL },
};
const struct jwl_interface jt_registry = { "wl_registry", 1, 1, 2, registry_requests,
                                           registry_events };

static const struct jwl_message callback_events[] = { { "done", "u", NULL } };
const struct jwl_interface jt_callback = { "wl_callback", 1, 0, 1, NULL, callback_events };

static const struct jwl_message all_requests[] = {
    { "args", "iuf?s?oa", t_args },
    { "make", "n", t_thing },
    { "give", "hh", NULL },
    { "newer", "2u", NULL },
    { "newest", "3", NULL },
    { "strs", "ss", NULL },
    { "destroy", "", NULL },
    { "any_obj", "o", NULL },
    { "max", "uuuuuuuuuuuuuuuuuuuu", NULL },
};
static const struct jwl_message all_events[] = {
    { "ev_args", "iufsoa", t_ev_args },
    { "ev_make", "n", t_thing },
    { "ev_handle", "h", NULL },
    { "ev_newer", "2u", NULL },
};
const struct jwl_interface jt_all = { "jt_all", 3, 9, 4, all_requests, all_events };

static const struct jwl_message thing_requests[] = { { "poke", "u", NULL }, { "destroy", "", NULL } };
static const struct jwl_message thing_events[] = { { "poked", "u", NULL } };
/* version 3 as jt_all's: an object a request makes has its maker's version */
const struct jwl_interface jt_thing = { "jt_thing", 3, 2, 1, thing_requests, thing_events };

const struct jwl_interface *const jt_known[] = { &jt_all, &jt_thing, &jt_callback };

/* ---- helpers ------------------------------------------------------------------------ */

uint32_t jt_rng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

void jt_w32(struct jt_wr *w, uint32_t v)
{
    memcpy(w->b + w->n, &v, 4);
    w->n += 4;
}

void jt_wbytes(struct jt_wr *w, uint32_t len, const void *bytes, uint32_t n)
{
    jt_w32(w, len);
    if (n)
        memcpy(w->b + w->n, bytes, n);
    uint32_t pad = (n + 3u) & ~3u;
    memset(w->b + w->n + n, 0, pad - n);
    w->n += pad;
}

void jt_wstr(struct jt_wr *w, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    jt_wbytes(w, n, s, n);
}

void jt_begin(struct jt_wr *w, uint32_t id, uint16_t op)
{
    w->start = w->n;
    jt_w32(w, id);
    jt_w32(w, op);
}

void jt_end(struct jt_wr *w)
{
    uint32_t word = (uint32_t)(w->n - w->start) << 16;
    uint32_t op;
    memcpy(&op, w->b + w->start + 4, 4);
    word |= op & 0xffff;
    memcpy(w->b + w->start + 4, &word, 4);
}

status_t jt_request(struct jt_pair *p, uint32_t id, uint16_t op, const union jwl_arg *args,
                    unsigned nargs, struct jwl_msg *m, struct jwl_error *err)
{
    static uint8_t buf[JWL_MSG_MAX];
    struct jwl_object *o = jwl_map_entry(&p->client, id);
    if (!o || !o->iface || op >= o->iface->nrequests)
        return ERR_NOT_FOUND;
    struct jwl_out out = { .buf = buf, .cap = sizeof(buf) };
    handle_t hs[4];
    out.h = hs;
    out.hcap = 4;
    status_t st = jwl_encode(&out, &o->iface->requests[op], id, op, args, nargs);
    if (st != OK)
        return st;
    struct jwl_in in = { .buf = buf, .len = out.len, .h = hs, .nh = out.nh };
    return jwl_decode(&in, &p->server, m, err);
}

bool jt_pair_init(struct jt_pair *p, uint32_t version)
{
    CHECK_ST(jwl_map_init(&p->client, JWL_CLIENT, &jt_display, jt_known, JT_NKNOWN), OK);
    CHECK_ST(jwl_map_init(&p->server, JWL_SERVER, &jt_display, jt_known, JT_NKNOWN), OK);
    struct jwl_msg m;
    struct jwl_error err;
    union jwl_arg a[2];
    CHECK_ST(jwl_map_new(&p->client, &jt_registry, 1, NULL, &a[0].n), OK);
    CHECK_EQ(a[0].n, 2);
    CHECK_ST(jt_request(p, JWL_DISPLAY_ID, 1, a, 1, &m, &err), OK);
    a[0].u = 7;   /* the global's name */
    a[1].any = (struct jwl_new_any){ .iface = &jt_all, .version = version };
    CHECK_ST(jwl_map_new(&p->client, &jt_all, version, NULL, &a[1].any.id), OK);
    CHECK_ST(jt_request(p, 2, 0, a, 2, &m, &err), OK);
    CHECK(m.args[1].any.iface == &jt_all && m.args[1].any.version == version);
    CHECK_ST(jwl_map_new(&p->client, &jt_thing, version, NULL, &a[0].n), OK);
    CHECK_ST(jt_request(p, 3, JT_MAKE, a, 1, &m, &err), OK);
    CHECK_EQ(a[0].n, 4);
    return true;
}

void jt_pair_free(struct jt_pair *p)
{
    jwl_map_free(&p->client);
    jwl_map_free(&p->server);
}

/* ---- tables and signatures ------------------------------------------------------------ */

bool t_jwl_tables(void)
{
    const struct jwl_interface *all[] = { &jt_display, &jt_registry, &jt_callback, &jt_all,
                                          &jt_thing };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        CHECK_ST(jwl_interface_check(all[i]), OK);
    struct jwl_sig s;
    CHECK_ST(jwl_sig_parse("", &s), OK);
    CHECK(s.n == 0 && s.since == 1);
    CHECK_ST(jwl_sig_parse("12?s?oh", &s), OK);
    CHECK(s.since == 12 && s.n == 3 && s.type[0] == 's' && s.nullable[0] && s.nullable[1] &&
          !s.nullable[2] && s.type[2] == 'h');
    CHECK_ST(jwl_sig_parse(all_requests[JT_MAX].signature, &s), OK);
    CHECK_EQ(s.n, JWL_ARGS_MAX);
    const char *bad[] = { "?", "u?", "??s", "?u", "?n", "?a", "?h", "x", "0u", "00", "u2",
                          "uuuuuuuuuuuuuuuuuuuuu", "99999999999u", "s?" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (jwl_sig_parse(bad[i], &s) != ERR_INVALID_ARGS)
            FAIL("signature \"%s\" taken", bad[i]);
    CHECK_ST(jwl_sig_parse(NULL, &s), ERR_INVALID_ARGS);
    /* broken tables */
    static const struct jwl_message newer[] = { { "x", "4u", NULL } };
    static const struct jwl_interface *const typed_u[] = { &jt_thing };
    static const struct jwl_message typed_wrong[] = { { "x", "u", typed_u } };
    static const struct jwl_message noname[] = { { NULL, "u", NULL } };
    struct jwl_interface t = { "t", 3, 1, 0, newer, NULL };
    CHECK_ST(jwl_interface_check(&t), ERR_INVALID_ARGS);   /* since 4 > version 3 */
    t.requests = typed_wrong;
    CHECK_ST(jwl_interface_check(&t), ERR_INVALID_ARGS);   /* a type on a 'u' */
    t.requests = noname;
    CHECK_ST(jwl_interface_check(&t), ERR_INVALID_ARGS);
    t.requests = NULL;
    CHECK_ST(jwl_interface_check(&t), ERR_INVALID_ARGS);   /* a count with no array */
    t.version = 0;
    t.nrequests = 0;
    CHECK_ST(jwl_interface_check(&t), ERR_INVALID_ARGS);
    CHECK(jwl_interface_same(&jt_all, &jt_all) && !jwl_interface_same(&jt_all, &jt_thing));
    struct jwl_interface copy = jt_thing;   /* another library's table of the same name */
    CHECK(jwl_interface_same(&copy, &jt_thing));
    return true;
}

/* ---- round trips ---------------------------------------------------------------------- */

bool t_jwl_roundtrip(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jwl_msg m;
    struct jwl_error err;
    static const uint8_t blob[7] = { 1, 2, 3, 4, 5, 6, 7 };
    union jwl_arg a[JWL_ARGS_MAX] = {
        { .i = -5 }, { .u = 0xdeadbeef }, { .f = -256 }, { .s = "hello" }, { .o = 4 },
        { .a = { sizeof(blob), blob } },
    };
    CHECK_ST(jt_request(&p, 3, JT_ARGS, a, 6, &m, &err), OK);
    CHECK(m.id == 3 && m.opcode == JT_ARGS && m.nargs == 6 && m.iface == &jt_all);
    CHECK(m.args[0].i == -5 && m.args[1].u == 0xdeadbeef && m.args[2].f == -256);
    CHECK(!strcmp(m.args[3].s, "hello") && m.args[4].o == 4);
    CHECK(m.args[5].a.size == 7 && !memcmp(m.args[5].a.data, blob, 7));
    a[3].s = NULL;   /* the nullable ones null */
    a[4].o = 0;
    a[5].a = (struct jwl_array){ 0, NULL };
    CHECK_ST(jt_request(&p, 3, JT_ARGS, a, 6, &m, &err), OK);
    CHECK(m.args[3].s == NULL && m.args[4].o == 0 && m.args[5].a.size == 0 && !m.args[5].a.data);
    /* the bytes: padding written as zeros, sizes exact */
    uint8_t buf[64];
    struct jwl_out out = { .buf = buf, .cap = sizeof(buf) };
    a[3].s = "hello";
    a[5].a = (struct jwl_array){ sizeof(blob), blob };
    CHECK_ST(jwl_encode(&out, &all_requests[JT_ARGS], 3, JT_ARGS, a, 6), OK);
    CHECK_EQ(out.len, 8 + 12 + 4 + 8 + 4 + 4 + 8);
    CHECK(buf[8 + 12 + 4 + 6] == 0 && buf[8 + 12 + 4 + 7] == 0);   /* "hello\0" + 2 */
    CHECK(buf[out.len - 1] == 0);                                     /* 7 bytes + 1 */
    uint32_t w;
    memcpy(&w, buf + 4, 4);
    CHECK_EQ(w, (uint32_t)out.len << 16 | JT_ARGS);
    /* twenty arguments; a since-3 message on a version-3 object */
    for (unsigned i = 0; i < JWL_ARGS_MAX; i++)
        a[i].u = i * 3;
    CHECK_ST(jt_request(&p, 3, JT_MAX, a, JWL_ARGS_MAX, &m, &err), OK);
    CHECK(m.nargs == JWL_ARGS_MAX && m.args[19].u == 57);
    CHECK_ST(jt_request(&p, 3, JT_NEWEST, a, 0, &m, &err), OK);
    jt_pair_free(&p);
    return true;
}

/* The compositor's events to the client: its own new ids, a handle. */
bool t_jwl_roundtrip_events(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    uint8_t buf[256];
    handle_t hs[2];
    struct jwl_out out = { .buf = buf, .cap = sizeof(buf), .h = hs, .hcap = 2 };
    static const uint8_t blob[4] = { 9, 8, 7, 6 };
    union jwl_arg a[6] = { { .i = 1 }, { .u = 2 }, { .f = 3 }, { .s = "x" }, { .o = 4 },
                           { .a = { 4, blob } } };
    CHECK_ST(jwl_encode(&out, &all_events[JT_EV_ARGS], 3, JT_EV_ARGS, a, 6), OK);
    uint32_t sid;
    CHECK_ST(jwl_map_new(&p.server, &jt_thing, 3, NULL, &sid), OK);
    CHECK_EQ(sid, JWL_SERVER_ID_BASE);
    a[0].n = sid;
    CHECK_ST(jwl_encode(&out, &all_events[JT_EV_MAKE], 3, JT_EV_MAKE, a, 1), OK);
    a[0].h = 0x1234;   /* a value only: decoding never touches the handle */
    CHECK_ST(jwl_encode(&out, &all_events[JT_EV_HANDLE], 3, JT_EV_HANDLE, a, 1), OK);
    CHECK_EQ(out.nh, 1);
    struct jwl_in in = { .buf = buf, .len = out.len, .h = hs, .nh = out.nh };
    struct jwl_msg m;
    struct jwl_error err;
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.opcode == JT_EV_ARGS && !strcmp(m.args[3].s, "x") && m.args[4].o == 4);
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.args[0].n == sid && jwl_map_get(&p.client, sid)->iface == &jt_thing);
    CHECK_EQ(jwl_map_get(&p.client, sid)->version, 3);   /* the parent's version */
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.args[0].h == 0x1234 && m.nhandles == 1 && in.hat == 1);
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), ERR_SHOULD_WAIT);
    jt_pair_free(&p);
    return true;
}

/* What the encoder refuses: sizes, nulls, counts. */
bool t_jwl_encode_limits(void)
{
    static uint8_t buf[JWL_BATCH_MAX];
    static char big[JWL_STRING_MAX + 8];
    struct jwl_out out = { .buf = buf, .cap = sizeof(buf) };
    union jwl_arg a[6] = { { 0 } };
    const struct jwl_message *strs = &all_requests[JT_STRS];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    /* "ss" with "" second: 8 + (4 + 4076) + (4 + 4) is the biggest message */
    big[4075] = 0;
    a[0].s = big;
    a[1].s = "";
    CHECK_ST(jwl_encode(&out, strs, 3, JT_STRS, a, 2), OK);
    CHECK_EQ(out.len, JWL_MSG_MAX);
    big[4075] = 'x';   /* one byte more pads to 4080: 4100 bytes */
    big[4076] = 0;
    CHECK_ST(jwl_encode(&out, strs, 3, JT_STRS, a, 2), ERR_OUT_OF_RANGE);
    big[4076] = 'x';   /* 4096 bytes and the NUL: over any string */
    big[JWL_STRING_MAX] = 0;
    CHECK_ST(jwl_encode(&out, strs, 3, JT_STRS, a, 2), ERR_INVALID_ARGS);
    a[0].s = NULL;
    CHECK_ST(jwl_encode(&out, strs, 3, JT_STRS, a, 2), ERR_INVALID_ARGS);
    a[0].s = "a";
    CHECK_ST(jwl_encode(&out, strs, 3, JT_STRS, a, 1), ERR_INVALID_ARGS);   /* a count short */
    a[0].o = 0;
    CHECK_ST(jwl_encode(&out, &all_requests[JT_ANY_OBJ], 3, JT_ANY_OBJ, a, 1), ERR_INVALID_ARGS);
    a[0].n = 0;
    CHECK_ST(jwl_encode(&out, &all_requests[JT_MAKE], 3, JT_MAKE, a, 1), ERR_INVALID_ARGS);
    a[0].h = HANDLE_INVALID;
    a[1].h = 5;
    CHECK_ST(jwl_encode(&out, &all_requests[JT_GIVE], 3, JT_GIVE, a, 2), ERR_INVALID_ARGS);
    a[0].h = 4;
    CHECK_ST(jwl_encode(&out, &all_requests[JT_GIVE], 3, JT_GIVE, a, 2), ERR_BUFFER_TOO_SMALL);
    a[5].a = (struct jwl_array){ 3, NULL };
    a[3].s = NULL;
    CHECK_ST(jwl_encode(&out, &all_requests[JT_ARGS], 3, JT_ARGS, a, 6), ERR_INVALID_ARGS);
    a[5].a = (struct jwl_array){ JWL_STRING_MAX + 1, big };
    CHECK_ST(jwl_encode(&out, &all_requests[JT_ARGS], 3, JT_ARGS, a, 6), ERR_INVALID_ARGS);
    /* wl_registry.bind: no room; then a version past jt_all's */
    a[0].u = 7;
    a[1].any = (struct jwl_new_any){ &jt_all, 3, 9 };
    struct jwl_out small = { .buf = buf, .cap = 8 };
    CHECK_ST(jwl_encode(&small, &registry_requests[0], 2, 0, a, 2), ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(small.len, 0);
    a[1].any.version = 4;
    CHECK_ST(jwl_encode(&out, &registry_requests[0], 2, 0, a, 2), ERR_INVALID_ARGS);
    a[1].any.version = 3;
    a[1].any.id = 0;
    CHECK_ST(jwl_encode(&out, &registry_requests[0], 2, 0, a, 2), ERR_INVALID_ARGS);
    return true;
}

/* ---- the maps -------------------------------------------------------------------------- */

/* The client's own ids: made from 2, a destroyed one a zombie until
 * delete_id, then the first one reused. */
bool t_jwl_map_client_ids(void)
{
    struct jwl_map m;
    CHECK_ST(jwl_map_init(&m, JWL_CLIENT, &jt_display, jt_known, JT_NKNOWN), OK);
    uint32_t id[4];
    for (unsigned i = 0; i < 4; i++) {
        CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &id[i]), OK);
        CHECK_EQ(id[i], i + 2);
    }
    CHECK_EQ(m.live, 5);
    CHECK_ST(jwl_map_remove(&m, 3), OK);
    CHECK(jwl_map_entry(&m, 3)->state == JWL_ZOMBIE && !jwl_map_get(&m, 3));
    uint32_t x;
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &x), OK);
    CHECK_EQ(x, 6);   /* not 3: the compositor hasn't said delete_id */
    CHECK_ST(jwl_map_delete_id(&m, 3), OK);
    CHECK(jwl_map_entry(&m, 3)->state == JWL_FREE);
    CHECK_ST(jwl_map_delete_id(&m, 3), ERR_INVALID_ARGS);   /* twice */
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &x), OK);
    CHECK_EQ(x, 3);
    /* delete_id before the client destroys it (a wl_callback after done) */
    CHECK_ST(jwl_map_delete_id(&m, 4), OK);
    CHECK(jwl_map_get(&m, 4) != NULL);
    CHECK_ST(jwl_map_remove(&m, 4), OK);
    CHECK(jwl_map_entry(&m, 4)->state == JWL_FREE);   /* free at once */
    CHECK_ST(jwl_map_delete_id(&m, JWL_DISPLAY_ID), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_delete_id(&m, 99), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_delete_id(&m, JWL_SERVER_ID_BASE), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_remove(&m, JWL_DISPLAY_ID), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_remove(&m, 4), ERR_NOT_FOUND);
    CHECK_ST(jwl_map_set_data(&m, 5, &m), OK);
    CHECK(jwl_map_get(&m, 5)->data == &m);
    jwl_map_unmake(&m, 5);   /* never sent: free at once, no handshake */
    CHECK(jwl_map_entry(&m, 5)->state == JWL_FREE);
    /* the compositor's ids on a client: entered, a zombie once destroyed,
     * and made again in its place */
    CHECK_ST(jwl_map_check_new(&m, JWL_SERVER_ID_BASE), OK);
    CHECK_ST(jwl_map_check_new(&m, JWL_SERVER_ID_BASE + 1), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_check_new(&m, 7), ERR_INVALID_ARGS);   /* the client's own range */
    CHECK_ST(jwl_map_insert(&m, JWL_SERVER_ID_BASE, &jt_thing, 1, JWL_LIVE), OK);
    CHECK_ST(jwl_map_remove(&m, JWL_SERVER_ID_BASE), OK);
    CHECK(jwl_map_entry(&m, JWL_SERVER_ID_BASE)->state == JWL_ZOMBIE);
    CHECK_ST(jwl_map_check_new(&m, JWL_SERVER_ID_BASE), OK);
    jwl_map_free(&m);
    return true;
}

/* The compositor's map: the client's ids next-or-free, its own from the
 * base and reused at once; both ranges capped. */
bool t_jwl_map_server_ids(void)
{
    struct jwl_map m;
    CHECK_ST(jwl_map_init(&m, JWL_SERVER, &jt_display, jt_known, JT_NKNOWN), OK);
    CHECK_ST(jwl_map_check_new(&m, 0), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_check_new(&m, 1), ERR_INVALID_ARGS);   /* wl_display, live */
    CHECK_ST(jwl_map_check_new(&m, 3), ERR_INVALID_ARGS);   /* past the next */
    CHECK_ST(jwl_map_check_new(&m, JWL_SERVER_ID_BASE), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_check_new(&m, 2), OK);
    CHECK_ST(jwl_map_insert(&m, 2, &jt_thing, 1, JWL_LIVE), OK);
    CHECK_ST(jwl_map_check_new(&m, 2), ERR_INVALID_ARGS);
    CHECK_ST(jwl_map_remove(&m, 2), OK);
    CHECK(jwl_map_entry(&m, 2)->state == JWL_FREE);   /* free at once: delete_id follows */
    CHECK_ST(jwl_map_check_new(&m, 2), OK);
    uint32_t a, b;
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &a), OK);
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &b), OK);
    CHECK(a == JWL_SERVER_ID_BASE && b == JWL_SERVER_ID_BASE + 1);
    CHECK_ST(jwl_map_remove(&m, a), OK);
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &a), OK);
    CHECK_EQ(a, JWL_SERVER_ID_BASE);
    /* the client's range to its cap: the next id past it is no_memory, one
     * that skips ahead before then is a lie */
    for (uint32_t id = 2; id <= JWL_OBJECTS_MAX; id++) {
        if (jwl_map_check_new(&m, id) != OK || jwl_map_insert(&m, id, &jt_thing, 1, JWL_LIVE))
            FAIL("client id %u refused", id);
    }
    CHECK_ST(jwl_map_check_new(&m, JWL_OBJECTS_MAX + 1), ERR_NO_RESOURCES);
    CHECK_ST(jwl_map_check_new(&m, JWL_CLIENT_ID_MAX), ERR_NO_RESOURCES);
    CHECK_ST(jwl_map_check_new(&m, JWL_CLIENT_ID_MAX + 1), ERR_INVALID_ARGS);
    CHECK_EQ(m.live, JWL_OBJECTS_MAX + 2);
    /* our own range to its cap */
    for (unsigned i = 2; i < JWL_OBJECTS_MAX; i++)
        CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &a), OK);
    CHECK_ST(jwl_map_new(&m, &jt_thing, 1, NULL, &a), ERR_NO_RESOURCES);
    jwl_map_free(&m);
    CHECK_ST(jwl_map_init(&m, JWL_SERVER, &jt_display, jt_known, JT_NKNOWN), OK);
    CHECK_ST(jwl_map_check_new(&m, 3000), ERR_INVALID_ARGS);   /* not full: a skip */
    jwl_map_free(&m);
    return true;
}
