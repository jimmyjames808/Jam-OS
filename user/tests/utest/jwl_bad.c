/* utest: libjwl's decoder (<jwl.h>) refusing every malformed message, one
 * rule broken at a time, each with the wl_display error (object and code)
 * the compositor will send, and nothing consumed: the framing (sizes,
 * stray bytes, unknown objects and opcodes, versions, leftovers), every
 * argument letter (strings, arrays, objects, new ids, handles, the
 * untyped new_id of wl_registry.bind) and the events a client refuses. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jwl.h>
#include <os.h>
#include "jwltest.h"
#include "utest.h"

#define M_OBJ JWL_ERROR_INVALID_OBJECT
#define M_BAD JWL_ERROR_INVALID_METHOD
#define M_MEM JWL_ERROR_NO_MEMORY

static uint8_t buf[JWL_BATCH_MAX];

/* buf[0..n) with nh handles must be refused by map with (object, code),
 * leaving the cursor and the map's live count where they were. */
static bool refused(struct jwl_map *map, size_t n, unsigned nh, uint32_t object, uint32_t code,
                    const char *what)
{
    static const handle_t hs[4] = { 11, 12, 13, 14 };
    struct jwl_in in = { .buf = buf, .len = n, .h = hs, .nh = nh };
    struct jwl_msg m;
    struct jwl_error err;
    uint32_t live = map->live;
    status_t st = jwl_decode(&in, map, &m, &err);
    if (st != ERR_INVALID_ARGS)
        FAIL("%s: %s, not refused", what, status_str(st));
    if (err.object != object || err.code != code)
        FAIL("%s: code %u on @%u (\"%s\"), want %u on @%u", what, err.code, err.object, err.text,
             code, object);
    if (in.at != 0 || in.hat != 0 || map->live != live)
        FAIL("%s: refused, but something was used up", what);
    return true;
}

#define REFUSED(map, nh, object, code, what)                     \
    do {                                                          \
        if (!refused(map, w.n, nh, object, code, what))           \
            return false;                                         \
    } while (0)

/* One message: id, op, the given words, its size word right. */
static void msg(struct jt_wr *w, uint32_t id, uint16_t op, const uint32_t *words, unsigned n)
{
    w->n = 0;
    jt_begin(w, id, op);
    for (unsigned i = 0; i < n; i++)
        jt_w32(w, words[i]);
    jt_end(w);
}

bool t_jwl_bad_framing(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jt_wr w = { .b = buf };
    jt_w32(&w, 3);
    REFUSED(&p.server, 0, 1, M_BAD, "4 stray bytes");
    uint32_t sizes[] = { 0, 4, 10, 12 };   /* under 8, not words, past the batch */
    for (unsigned i = 0; i < 4; i++) {
        w.n = 0;
        jt_w32(&w, 3);
        jt_w32(&w, sizes[i] << 16 | JT_NEWEST);
        REFUSED(&p.server, 0, 1, M_BAD, "a bad size");
    }
    w.n = 0;   /* over JWL_MSG_MAX, with every byte there */
    jt_begin(&w, 3, JT_STRS);
    memset(buf + w.n, 0, 4100 - w.n);
    w.n = 4100;
    jt_end(&w);
    REFUSED(&p.server, 0, 1, M_BAD, "4100 bytes");
    msg(&w, 99, 0, NULL, 0);
    REFUSED(&p.server, 0, 1, M_OBJ, "an object never made");
    msg(&w, 0, 0, NULL, 0);
    REFUSED(&p.server, 0, 1, M_OBJ, "object 0");
    msg(&w, JWL_SERVER_ID_BASE + 5, 0, NULL, 0);
    REFUSED(&p.server, 0, 1, M_OBJ, "a compositor id never made");
    msg(&w, 3, 10, NULL, 0);
    REFUSED(&p.server, 0, 3, M_BAD, "opcode 10 of 10");
    msg(&w, 3, 0xffff, NULL, 0);
    REFUSED(&p.server, 0, 3, M_BAD, "opcode 0xffff");
    uint32_t one = 7;
    msg(&w, 4, 0, &one, 0);
    REFUSED(&p.server, 0, 4, M_BAD, "poke cut short");
    uint32_t two[2] = { 1, 2 };
    msg(&w, 4, 0, two, 2);
    REFUSED(&p.server, 0, 4, M_BAD, "a word past the arguments");
    CHECK_ST(jwl_map_remove(&p.server, 4), OK);
    msg(&w, 4, 0, &one, 1);
    REFUSED(&p.server, 0, 1, M_OBJ, "an object destroyed");
    jt_pair_free(&p);
    /* messages newer than the version bound */
    if (!jt_pair_init(&p, 1))
        return false;
    msg(&w, 3, JT_NEWER, &one, 1);
    REFUSED(&p.server, 0, 3, M_BAD, "a since-2 request on version 1");
    msg(&w, 3, JT_NEWEST, NULL, 0);
    REFUSED(&p.server, 0, 3, M_BAD, "a since-3 request on version 1");
    struct jwl_msg m;
    struct jwl_error err;
    union jwl_arg a = { .u = 1 };
    CHECK_ST(jt_request(&p, 3, JT_DESTROY, &a, 0, &m, &err), OK);   /* since 1 still goes */
    jt_pair_free(&p);
    return true;
}

bool t_jwl_bad_strings(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jt_wr w = { .b = buf };
    struct { uint32_t len; const char *bytes; uint32_t n; const char *what; } cases[] = {
        { 0, "", 0, "a null string where none may be" },
        { 3, "abc", 3, "no NUL" },
        { 4, "a\0b", 4, "a NUL inside" },
        { 1, "x", 1, "a one-byte string that isn't the NUL" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        w.n = 0;
        jt_begin(&w, 3, JT_STRS);
        jt_wbytes(&w, cases[i].len, cases[i].bytes, cases[i].n);
        jt_wstr(&w, "ok");
        jt_end(&w);
        REFUSED(&p.server, 0, 3, M_BAD, cases[i].what);
    }
    w.n = 0;   /* the length runs past the message */
    jt_begin(&w, 3, JT_STRS);
    jt_wstr(&w, "ok");
    jt_w32(&w, 9);
    jt_w32(&w, 0);
    jt_end(&w);
    REFUSED(&p.server, 0, 3, M_BAD, "a string past the end");
    w.n = 0;
    jt_begin(&w, 3, JT_STRS);
    jt_w32(&w, JWL_STRING_MAX + 1);
    jt_end(&w);
    REFUSED(&p.server, 0, 3, M_BAD, "a string over 4096");
    w.n = 0;
    jt_begin(&w, 3, JT_STRS);
    jt_w32(&w, 0xfffffffdu);   /* would wrap when padded */
    jt_end(&w);
    REFUSED(&p.server, 0, 3, M_BAD, "a string length near 2^32");
    /* arrays in JT_ARGS: i u f ?s ?o a */
    uint32_t lens[] = { 5, JWL_STRING_MAX + 1, 0xffffffffu };
    for (unsigned i = 0; i < 3; i++) {
        w.n = 0;
        jt_begin(&w, 3, JT_ARGS);
        jt_w32(&w, 1);
        jt_w32(&w, 2);
        jt_w32(&w, 3);
        jt_w32(&w, 0);
        jt_w32(&w, 0);
        jt_w32(&w, lens[i]);
        jt_w32(&w, 0);
        jt_end(&w);
        REFUSED(&p.server, 0, 3, M_BAD, "an array past the end or over 4096");
    }
    jt_pair_free(&p);
    return true;
}

bool t_jwl_bad_objects(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jt_wr w = { .b = buf };
    uint32_t v = 0;
    msg(&w, 3, JT_ANY_OBJ, &v, 1);
    REFUSED(&p.server, 0, 3, M_BAD, "a null object where none may be");
    v = 77;
    msg(&w, 3, JT_ANY_OBJ, &v, 1);
    REFUSED(&p.server, 0, 3, M_OBJ, "an object never made");
    uint32_t args[6] = { 1, 2, 3, 0, 2, 0 };   /* the 'o' is the registry, not a jt_thing */
    msg(&w, 3, JT_ARGS, args, 6);
    REFUSED(&p.server, 0, 3, M_OBJ, "an object of the wrong interface");
    args[4] = 4;
    msg(&w, 3, JT_ARGS, args, 6);
    struct jwl_in in = { .buf = buf, .len = w.n };
    struct jwl_msg m;
    struct jwl_error err;
    CHECK_ST(jwl_decode(&in, &p.server, &m, &err), OK);   /* and right */
    /* new ids: the next is 5 */
    uint32_t bad_ids[] = { 0, 1, 4, 6, 4097, JWL_SERVER_ID_BASE, 0xffffffffu };
    for (unsigned i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        msg(&w, 3, JT_MAKE, &bad_ids[i], 1);
        REFUSED(&p.server, 0, 3, M_OBJ, "a new id not the next or a free one");
    }
    for (uint32_t id = 5; id <= JWL_OBJECTS_MAX; id++)
        CHECK_ST(jwl_map_insert(&p.server, id, &jt_thing, 3, JWL_LIVE), OK);
    v = JWL_OBJECTS_MAX + 1;
    msg(&w, 3, JT_MAKE, &v, 1);
    REFUSED(&p.server, 0, 1, M_MEM, "a new id past the cap");
    CHECK_ST(jwl_map_remove(&p.server, 100), OK);
    v = 100;
    msg(&w, 3, JT_MAKE, &v, 1);
    in = (struct jwl_in){ .buf = buf, .len = w.n };
    CHECK_ST(jwl_decode(&in, &p.server, &m, &err), OK);   /* a free one again */
    /* handles */
    msg(&w, 3, JT_GIVE, NULL, 0);
    REFUSED(&p.server, 1, 3, M_BAD, "two fds, one handle");
    REFUSED(&p.server, 0, 3, M_BAD, "two fds, no handle");
    jt_pair_free(&p);
    return true;
}

/* wl_registry.bind's untyped new_id: name, version, id. */
static void bind_msg(struct jt_wr *w, const char *name, uint32_t version, uint32_t id)
{
    w->n = 0;
    jt_begin(w, 2, 0);
    jt_w32(w, 7);
    if (name)
        jt_wstr(w, name);
    else
        jt_w32(w, 0);
    jt_w32(w, version);
    jt_w32(w, id);
    jt_end(w);
}

bool t_jwl_bad_bind(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jt_wr w = { .b = buf };
    bind_msg(&w, "jt_nothing", 1, 5);
    REFUSED(&p.server, 0, 2, M_OBJ, "an interface not offered");
    bind_msg(&w, "wl_display", 1, 5);
    REFUSED(&p.server, 0, 2, M_OBJ, "wl_display isn't offered");
    bind_msg(&w, "jt_all", 0, 5);
    REFUSED(&p.server, 0, 2, M_OBJ, "version 0");
    bind_msg(&w, "jt_all", 4, 5);
    REFUSED(&p.server, 0, 2, M_OBJ, "a version past ours");
    bind_msg(&w, NULL, 1, 5);
    REFUSED(&p.server, 0, 2, M_BAD, "a null name");
    bind_msg(&w, "jt_all", 1, 6);
    REFUSED(&p.server, 0, 2, M_OBJ, "a new id that skips one");
    bind_msg(&w, "jt_all", 2, 5);
    struct jwl_in in = { .buf = buf, .len = w.n };
    struct jwl_msg m;
    struct jwl_error err;
    CHECK_ST(jwl_decode(&in, &p.server, &m, &err), OK);
    CHECK(m.nargs == 4 && !strcmp(m.args[1].s, "jt_all") && m.args[2].u == 2 && m.args[3].n == 5);
    CHECK(jwl_map_get(&p.server, 5)->iface == &jt_all && jwl_map_get(&p.server, 5)->version == 2);
    jt_pair_free(&p);
    return true;
}

/* What a client refuses of the compositor, and what it drops quietly:
 * events to an object it destroyed (the destroy and the event crossed). */
bool t_jwl_bad_events(void)
{
    struct jt_pair p;
    if (!jt_pair_init(&p, 3))
        return false;
    struct jt_wr w = { .b = buf };
    msg(&w, 50, 0, NULL, 0);
    REFUSED(&p.client, 0, 1, M_OBJ, "an event to an object never made");
    uint32_t v = 5;
    msg(&w, 3, JT_EV_MAKE, &v, 1);
    REFUSED(&p.client, 0, 3, M_OBJ, "a compositor making a client id");
    v = JWL_SERVER_ID_BASE + 1;
    msg(&w, 3, JT_EV_MAKE, &v, 1);
    REFUSED(&p.client, 0, 3, M_OBJ, "a compositor id that skips one");
    msg(&w, 3, JT_EV_HANDLE, NULL, 0);
    REFUSED(&p.client, 0, 3, M_BAD, "an fd with no handle");
    uint32_t args[6] = { 1, 2, 3, 0, 4, 0 };   /* ev_args: s isn't nullable */
    msg(&w, 3, JT_EV_ARGS, args, 6);
    REFUSED(&p.client, 0, 3, M_BAD, "a null string in an event");
    /* the client destroys thing 4; an event naming it, one sent to it */
    CHECK_ST(jwl_map_remove(&p.client, 4), OK);
    w.n = 0;
    jt_begin(&w, 3, JT_EV_ARGS);
    jt_w32(&w, 1);
    jt_w32(&w, 2);
    jt_w32(&w, 3);
    jt_wstr(&w, "s");
    jt_w32(&w, 4);
    jt_w32(&w, 0);
    jt_end(&w);
    size_t first = w.n;
    jt_begin(&w, 4, 0);
    jt_w32(&w, 9);
    jt_end(&w);
    struct jwl_in in = { .buf = buf, .len = w.n };
    struct jwl_msg m;
    struct jwl_error err;
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.args[4].o == 0 && !m.dead_target);   /* the destroyed object reads as null */
    CHECK_EQ(in.at, first);
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.dead_target && m.id == 4 && m.args[0].u == 9);
    /* a compositor id made in an event to a zombie is a zombie too */
    CHECK_ST(jwl_map_remove(&p.client, 3), OK);
    v = JWL_SERVER_ID_BASE;
    msg(&w, 3, JT_EV_MAKE, &v, 1);
    in = (struct jwl_in){ .buf = buf, .len = w.n };
    CHECK_ST(jwl_decode(&in, &p.client, &m, &err), OK);
    CHECK(m.dead_target && jwl_map_entry(&p.client, v)->state == JWL_ZOMBIE);
    jt_pair_free(&p);
    /* since on events */
    if (!jt_pair_init(&p, 1))
        return false;
    msg(&w, 3, JT_EV_NEWER, &v, 1);
    REFUSED(&p.client, 0, 3, M_BAD, "a since-2 event on version 1");
    jt_pair_free(&p);
    return true;
}
