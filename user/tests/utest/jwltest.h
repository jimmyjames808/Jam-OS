/* utest's libjwl tests (<jwl.h>): what jwl.c, jwl_bad.c, jwl_conn.c and
 * jwl_fuzz.c share. A hand-written stand-in for the generated tables:
 * wl_display, wl_registry and wl_callback as wayland.xml has them, and
 * two test interfaces, jt_all (every argument letter, a since-2 and a
 * since-3 message, a typed and an untyped new_id, handles, 20 arguments)
 * and jt_thing (what jt_all's object arguments and new ids name). */
#pragma once

#include <jwl.h>

extern const struct jwl_interface jt_display, jt_registry, jt_callback, jt_all, jt_thing;
extern const struct jwl_interface *const jt_known[];   /* what a bind may name */
#define JT_NKNOWN 3u

/* jt_all's requests and events by opcode, and their signatures */
enum {
    JT_ARGS,       /* "iuf?s?oa", the 'o' a jt_thing */
    JT_MAKE,       /* "n" jt_thing */
    JT_GIVE,       /* "hh" */
    JT_NEWER,      /* "2u" */
    JT_NEWEST,     /* "3" */
    JT_STRS,       /* "ss" */
    JT_DESTROY,    /* "" */
    JT_ANY_OBJ,    /* "o", any interface */
    JT_MAX,        /* twenty 'u' */
};
enum {
    JT_EV_ARGS,    /* "iufsoa", the 'o' a jt_thing */
    JT_EV_MAKE,    /* "n" jt_thing, made by the compositor */
    JT_EV_HANDLE,  /* "h" */
    JT_EV_NEWER,   /* "2u" */
};

/* Both ends' maps with the same objects: wl_registry at 2, jt_all at 3
 * (bound at `version`), a jt_thing at 4, each made by a request the
 * client encoded and the compositor decoded. */
struct jt_pair {
    struct jwl_map client, server;
};
bool     jt_pair_init(struct jt_pair *p, uint32_t version);
void     jt_pair_free(struct jt_pair *p);
/* Encode request (id, op, args) as the client would, decode it as the
 * compositor: jwl_decode's status, *m and *err. */
status_t jt_request(struct jt_pair *p, uint32_t id, uint16_t op, const union jwl_arg *args,
                    unsigned nargs, struct jwl_msg *m, struct jwl_error *err);

/* Raw bytes, for messages the encoder would refuse to make. */
struct jt_wr {
    uint8_t *b;
    size_t   n;        /* bytes written */
    size_t   start;    /* where the open message began */
};
void jt_w32(struct jt_wr *w, uint32_t v);
/* A string's or array's wire form: len, then bytes (n of them), zero padding. */
void jt_wbytes(struct jt_wr *w, uint32_t len, const void *bytes, uint32_t n);
void jt_wstr(struct jt_wr *w, const char *s);
void jt_begin(struct jt_wr *w, uint32_t id, uint16_t op);
/* Close the open message: its size word from what was written. */
void jt_end(struct jt_wr *w);

/* xorshift32, for the fuzz loops (a fixed seed: runs repeat). */
uint32_t jt_rng(uint32_t *s);
