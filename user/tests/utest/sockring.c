/* utest: a socket's rings (<sockring.h>, user/lib/sockring.c), which
 * netstack and programs share: the counts (empty, full, far into the
 * stream), datagrams of every length through rings that wrap, a byte
 * stream with its end, the wake flags, a hostile peer (counts out of
 * range, bad record headers, bytes after the end, a whole header page of
 * garbage: the side that reads it stays inside its ring, which ends at a
 * page nothing may touch, and never trusts its own count back), and a
 * whole exchange between a fake netstack thread and the test as the
 * program, over one VMO and two events with the rights netstack hands
 * out. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <sockring.h>
#include "nettest.h"
#include "utest.h"

/* ---- helpers -------------------------------------------------------------------- */

/* One socket's rings: netstack's VMO (all rights) mapped as netstack, the
 * program's handle (SOCKRING_VMO_RIGHTS) mapped as the program. Each
 * mapping ends in a page with no access, so a ring copy that runs past the
 * rx ring's end faults at once. */
struct rings {
    handle_t vmo, prog_vmo;
    uint8_t *stack_map, *prog_map;
    uint64_t bytes;              /* sockring_bytes: the guard page follows */
    struct sockring s, p;        /* netstack's side, the program's */
};

static bool map_guarded(handle_t vmo, uint64_t bytes, uint8_t **out)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR);
    uint64_t va = 0;
    CHECK_ST(jam_vmar_map(vmar, vmo, 0, bytes + PAGE_SIZE, VMAR_READ | VMAR_WRITE, &va), OK);
    CHECK_ST(jam_vmar_protect(vmar, va + bytes, PAGE_SIZE, 0), OK);
    *out = (uint8_t *)(uintptr_t)va;
    return true;
}

static void unmap(uint8_t *map, uint64_t bytes)
{
    if (map)   /* a test's own mapping: nothing to do if it fails */
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map,
                             bytes + PAGE_SIZE);
}

static void rings_close(struct rings *r)
{
    unmap(r->stack_map, r->bytes);
    unmap(r->prog_map, r->bytes);
    if (r->vmo)
        jam_handle_close(r->vmo);
    if (r->prog_vmo)
        jam_handle_close(r->prog_vmo);
    *r = (struct rings){ 0 };
}

static bool rings_open(struct rings *r, uint32_t framing, uint32_t tx, uint32_t rx)
{
    *r = (struct rings){ .bytes = sockring_bytes(tx, rx) };
    CHECK_ST(jam_vmo_create(r->bytes + PAGE_SIZE, 0, HANDLE_INVALID, &r->vmo), OK);
    CHECK_ST(jam_handle_duplicate(r->vmo, SOCKRING_VMO_RIGHTS, &r->prog_vmo), OK);
    CHECK(map_guarded(r->vmo, r->bytes, &r->stack_map));
    CHECK(map_guarded(r->prog_vmo, r->bytes, &r->prog_map));
    CHECK_ST(sockring_make(&r->s, r->stack_map, framing, tx, rx), OK);
    CHECK_ST(sockring_attach(&r->p, r->prog_map, r->bytes, framing, tx, rx), OK);
    return true;
}

/* Datagram i of a test run: its length (every one from 0 up over a run)
 * and bytes. */
static uint32_t dg_len(uint64_t i)
{
    return (uint32_t)(i * 37 % (SOCKRING_DGRAM_MAX + 1));
}

static void dg_fill(uint64_t i, uint8_t *d)
{
    for (uint32_t k = 0; k < dg_len(i); k++)
        d[k] = (uint8_t)(i * 7 + k);
}

static bool dg_ok(uint64_t i, const struct sockring_dgram *h, const uint8_t *d)
{
    if (h->len != dg_len(i) || h->addr != 0x0a021500u + (uint32_t)i || h->port != (uint16_t)i)
        return false;
    for (uint32_t k = 0; k < h->len; k++)
        if (d[k] != (uint8_t)(i * 7 + k))
            return false;
    return true;
}

static struct sockring_dgram dg_hdr(uint64_t i)
{
    return (struct sockring_dgram){ .addr = 0x0a021500u + (uint32_t)i, .port = (uint16_t)i,
                                    .len = (uint16_t)dg_len(i) };
}

/* Byte b of a test stream. */
static uint8_t st_byte(uint64_t b)
{
    return (uint8_t)(b * 13 + (b >> 9));
}

/* ---- the counts, as pure functions ------------------------------------------------------- */

bool t_sockring_counts(void)
{
    const uint32_t z = SOCKRING_MIN;
    bool bad = false;
    CHECK_EQ(sockring_ready_n(0, 0, z, &bad), 0);
    CHECK_EQ(sockring_ready_n(5, 2, z, &bad), 3);
    CHECK_EQ(sockring_ready_n(z, 0, z, &bad), z);
    CHECK_EQ(sockring_ready_n(UINT64_MAX, UINT64_MAX - 3, z, &bad), 3);   /* far along */
    CHECK_EQ(sockring_room_n(0, 0, z, &bad), z);
    CHECK_EQ(sockring_room_n(z, 0, z, &bad), 0);                         /* honestly full */
    CHECK_EQ(sockring_room_n(UINT64_MAX, UINT64_MAX - 10, z, &bad), z - 10);
    CHECK(!bad);
    CHECK_EQ(sockring_ready_n(9, 10, z, &bad), 0);                       /* went backwards */
    CHECK(bad);
    bad = false;
    CHECK_EQ(sockring_ready_n(10 + z + 1, 10, z, &bad), z);              /* over a ring ahead */
    CHECK(bad);
    bad = false;
    CHECK_EQ(sockring_ready_n(UINT64_MAX, 0, z, &bad), z);
    CHECK(bad);
    bad = false;
    CHECK_EQ(sockring_room_n(10, 11, z, &bad), 0);                       /* consumer ahead */
    CHECK(bad);
    bad = false;
    CHECK_EQ(sockring_room_n(10, UINT64_MAX, z, &bad), 0);
    CHECK(bad);
    bad = false;
    CHECK_EQ(sockring_room_n(100000, 100000 - z - 1, z, &bad), 0);       /* over a ring behind */
    CHECK(bad);
    /* sizes and records */
    CHECK(sockring_size_ok(SOCKRING_MIN) && sockring_size_ok(SOCKRING_MAX));
    CHECK(!sockring_size_ok(0) && !sockring_size_ok(SOCKRING_MIN / 2));
    CHECK(!sockring_size_ok(SOCKRING_MAX * 2) && !sockring_size_ok(SOCKRING_MIN * 3));
    CHECK_EQ(sockring_dgram_bytes(0), SOCKRING_DGRAM_HDR);
    CHECK_EQ(sockring_dgram_bytes(1), SOCKRING_DGRAM_HDR + SOCKRING_ALIGN);
    CHECK_EQ(sockring_dgram_bytes(SOCKRING_DGRAM_MAX), SOCKRING_DGRAM_HDR + SOCKRING_DGRAM_MAX);
    CHECK_EQ(sockring_bytes(SOCKRING_UDP_TX, SOCKRING_UDP_RX), 4096 + 16384 + 32768);
    return true;
}

/* ---- datagrams, both ends in this thread ---------------------------------------------------- */

#define DG_RUN 700u   /* datagrams a direction: over 100 times round a 4 KiB ring */

/* DG_RUN datagrams from producer p to consumer c, filling the ring each
 * time: the ring refuses what doesn't fit, gives back every datagram as it
 * went in, and records often straddle the ring's end. */
static bool dgram_run(struct sockring_end *p, struct sockring_end *c)
{
    static uint8_t d[SOCKRING_DGRAM_MAX];
    struct sockring_dgram h;
    uint64_t put = 0, got = 0, straddled = 0;
    CHECK_ST(sockring_dgram_take(c, &h, d), ERR_SHOULD_WAIT);   /* empty */
    while (got < DG_RUN) {
        for (; put < DG_RUN; put++) {
            struct sockring_dgram ph = dg_hdr(put);
            dg_fill(put, d);
            uint64_t before = p->count;
            status_t st = sockring_dgram_put(p, &ph, d);
            if (st == ERR_SHOULD_WAIT) {   /* full for this one: nothing written */
                CHECK(sockring_room(p) < sockring_dgram_bytes(ph.len));
                CHECK_EQ(p->count, before);
                break;
            }
            CHECK_ST(st, OK);
            straddled += (before & (p->size - 1)) + sockring_dgram_bytes(ph.len) > p->size;
        }
        CHECK_ST(sockring_dgram_take(c, &h, d), ERR_SHOULD_WAIT);   /* not published yet */
        (void)sockring_publish(p);
        for (; got < put; got++) {
            CHECK_ST(sockring_dgram_take(c, &h, d), OK);
            CHECK(dg_ok(got, &h, d));
        }
        CHECK_ST(sockring_dgram_take(c, &h, d), ERR_SHOULD_WAIT);
        (void)sockring_publish(c);
    }
    CHECK_EQ(sockring_room(p), p->size);
    CHECK(straddled > 10);
    CHECK(!p->errors && !c->errors);
    return true;
}

bool t_sockring_dgram(void)
{
    struct rings r;
    bool ok = rings_open(&r, SOCKRING_DGRAM, SOCKRING_MIN, SOCKRING_MIN) &&
              dgram_run(&r.p.tx, &r.s.tx) && dgram_run(&r.s.rx, &r.p.rx);
    if (ok) {
        /* What a producer refuses, before any room is looked at. */
        static uint8_t d[SOCKRING_DGRAM_MAX + 1];
        struct sockring_dgram h = { .len = SOCKRING_DGRAM_MAX + 1 };
        CHECK_ST(sockring_dgram_put(&r.p.tx, &h, d), ERR_INVALID_ARGS);
        h = (struct sockring_dgram){ .len = 4, .flags = 1 };
        CHECK_ST(sockring_dgram_put(&r.p.tx, &h, d), ERR_INVALID_ARGS);
        h = (struct sockring_dgram){ .len = 4, .reserved = 1 };
        CHECK_ST(sockring_dgram_put(&r.p.tx, &h, d), ERR_INVALID_ARGS);
        CHECK_EQ(r.p.tx.count, r.s.tx.count);
    }
    rings_close(&r);
    return ok;
}

/* ---- a byte stream and its end --------------------------------------------------------------- */

#define ST_RUN (200u * 1024)   /* bytes a direction */

/* ST_RUN bytes from p to c in writes and reads of uneven sizes, then
 * the producer's end: the consumer takes every byte, then sees the end. */
static bool stream_run(struct sockring_end *p, struct sockring_end *c)
{
    static uint8_t b[6000];
    uint64_t sent = 0, got = 0;
    for (unsigned k = 0; got < ST_RUN; k++) {
        uint32_t want = (uint32_t)(k * 131 % 5000 + 1);
        if (want > ST_RUN - sent)
            want = (uint32_t)(ST_RUN - sent);
        for (uint32_t i = 0; i < want; i++)
            b[i] = st_byte(sent + i);
        sent += sockring_stream_write(p, b, want);
        if (sent == ST_RUN && !p->ended)
            (void)sockring_finish(p);
        else
            (void)sockring_publish(p);
        uint32_t n = sockring_stream_read(c, b, (uint32_t)(k * 97 % 3000 + 1));
        for (uint32_t i = 0; i < n; i++)
            CHECK_EQ(b[i], st_byte(got + i));
        got += n;
        (void)sockring_publish(c);
        CHECK(!sockring_at_end(c) || got == ST_RUN);
    }
    CHECK(sockring_at_end(c));
    CHECK_EQ(sockring_stream_read(c, b, sizeof(b)), 0);
    CHECK(!sockring_sleep(c, 1));                 /* at the end: nothing to sleep for */
    CHECK_EQ(sockring_stream_write(p, b, 10), 0);  /* nothing after the end */
    CHECK(!p->errors && !c->errors);
    return true;
}

/* A producer that adds bytes after its end: the consumer ignores them. */
static bool stream_past_end(struct sockring *prog, struct sockring *stack)
{
    struct sockring_line *l = prog->tx.prod;
    uint64_t end = stack->tx.count;
    CHECK(sockring_at_end(&stack->tx));
    __atomic_store_n(&l->count, end + 100, __ATOMIC_RELEASE);
    CHECK_EQ(sockring_ready(&stack->tx), 0);
    CHECK(sockring_at_end(&stack->tx));
    CHECK(stack->tx.errors > 0);
    /* Taking END away again changes nothing: the end was seen. */
    __atomic_store_n(&l->flags, 0, __ATOMIC_RELEASE);
    CHECK(sockring_at_end(&stack->tx));
    return true;
}

bool t_sockring_stream(void)
{
    struct rings r;
    bool ok = rings_open(&r, SOCKRING_STREAM, SOCKRING_MIN, 2 * SOCKRING_MIN) &&
              stream_run(&r.p.tx, &r.s.tx) && stream_run(&r.s.rx, &r.p.rx) &&
              stream_past_end(&r.p, &r.s);
    rings_close(&r);
    return ok;
}

/* ---- the wake flags ------------------------------------------------------------------------- */

bool t_sockring_wake(void)
{
    struct rings r;
    if (!rings_open(&r, SOCKRING_DGRAM, SOCKRING_MIN, SOCKRING_MIN))
        return false;
    struct sockring_end *p = &r.p.tx, *c = &r.s.tx;
    uint8_t d[SOCKRING_DGRAM_MAX] = { 0 };
    struct sockring_dgram h = { .addr = 1, .port = 2, .len = SOCKRING_DGRAM_MAX };
    CHECK(sockring_sleep(c, 1));                    /* empty: netstack sleeps, flag up */
    CHECK_EQ(c->cons->waits, 1);
    CHECK_ST(sockring_dgram_put(p, &h, d), OK);
    CHECK(sockring_publish(p));                     /* it waits: signal SOCKRING_SIG_TX */
    sockring_awake(c);
    CHECK(!sockring_sleep(c, 1));                   /* a record is there: no sleep */
    CHECK_EQ(c->cons->waits, 0);
    CHECK(!sockring_publish(p));                    /* awake: no signal */
    while (sockring_dgram_put(p, &h, d) == OK)
        ;
    (void)sockring_publish(p);
    uint32_t need = sockring_dgram_bytes(h.len);
    CHECK(sockring_room(p) > 0 && sockring_room(p) < need);
    CHECK(sockring_sleep(p, need));                 /* full for its next one: it sleeps */
    CHECK_EQ(p->prod->waits, 1);
    CHECK_ST(sockring_dgram_take(c, &h, d), OK);
    CHECK(sockring_publish(c));                     /* room, and it waits: SIG_TX_ROOM */
    CHECK(!sockring_sleep(p, need));
    CHECK_EQ(p->prod->waits, 0);
    /* The end wakes a sleeping consumer too. */
    struct sockring_end *q = &r.s.rx, *e = &r.p.rx;
    CHECK(sockring_sleep(e, 1));
    CHECK(sockring_finish(q));
    CHECK(!sockring_sleep(e, 1));
    CHECK(sockring_at_end(e));
    rings_close(&r);
    return true;
}

/* ---- a hostile peer -------------------------------------------------------------------------- */

/* netstack consuming a program's tx ring, the program writing its line
 * and records by hand. */
static bool hostile_counts(struct sockring *s, struct sockring *p)
{
    struct sockring_end *c = &s->tx;
    struct sockring_line *l = c->prod;
    c->count = 4096;
    __atomic_store_n(&c->cons->count, 7, __ATOMIC_RELAXED);   /* its own line, scribbled */
    l->count = 4000;                                          /* backwards */
    CHECK_EQ(sockring_ready(c), 0);
    CHECK_EQ(c->errors, 1);
    l->count = 4096 + c->size + 99;                           /* too far ahead */
    CHECK_EQ(sockring_ready(c), c->size);
    CHECK_EQ(c->errors, 2);
    l->count = 4096 + 40;                                     /* not on a record's edge */
    CHECK_EQ(sockring_ready(c), 32);
    CHECK_EQ(c->errors, 3);
    l->flags = 0x80;                                          /* a flag that doesn't exist */
    CHECK_EQ(sockring_ready(c), 32);
    CHECK_EQ(c->errors, 4);
    l->flags = 0;
    (void)sockring_publish(c);                                /* its own count, not the 7 */
    CHECK_EQ(c->cons->count, 4096);
    /* netstack producing into a program's rx ring whose count is hostile. */
    struct sockring_end *q = &s->rx;
    q->count = 9008;
    __atomic_store_n(&q->prod->count, 3, __ATOMIC_RELAXED);
    static const uint64_t cons[] = { 9009, UINT64_MAX, 9008 - SOCKRING_MIN - 16 };
    uint8_t d[16] = { 0 };
    struct sockring_dgram h = { .len = 16 };
    for (unsigned i = 0; i < 3; i++) {
        q->cons->count = cons[i];
        CHECK_EQ(sockring_room(q), 0);
        CHECK_ST(sockring_dgram_put(q, &h, d), ERR_SHOULD_WAIT);
    }
    CHECK_EQ(q->errors, 6);
    q->cons->count = 9008 - 64;
    CHECK_EQ(sockring_room(q), q->size - 64);
    CHECK_ST(sockring_dgram_put(q, &h, d), OK);
    (void)sockring_publish(q);
    CHECK_EQ(q->prod->count, 9008 + 32);
    (void)p;
    return true;
}

/* Write a record header by hand at the consumer's next record. */
static void raw_hdr(struct sockring_end *c, uint32_t len, uint32_t flags, uint32_t reserved)
{
    struct sockring_dgram h = { .addr = 5, .port = 6, .len = (uint16_t)len, .flags = flags,
                                .reserved = reserved };
    memcpy(c->data + (c->count & (c->size - 1)), &h, sizeof(h));
}

static bool hostile_records(struct sockring *s)
{
    static uint8_t d[SOCKRING_DGRAM_MAX + 64];
    struct sockring_end *c = &s->tx;
    struct sockring_dgram h;
    struct { uint32_t len, flags, reserved, published; } bad[] = {
        { SOCKRING_DGRAM_MAX + 1, 0, 0, 2048 },   /* too long */
        { 0xffff, 0, 0, 2048 },
        { 20, 1, 0, 64 },                         /* flags */
        { 20, 0, 9, 64 },                         /* reserved */
        { 100, 0, 0, 64 },                        /* more than was published */
        { SOCKRING_DGRAM_MAX, 0, 0, SOCKRING_DGRAM_MAX },
    };
    memset(d, 0xa5, sizeof(d));
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint64_t before = c->count, errs = c->errors;
        raw_hdr(c, bad[i].len, bad[i].flags, bad[i].reserved);
        c->prod->count = before + bad[i].published;
        CHECK_ST(sockring_dgram_take(c, &h, d), ERR_OUT_OF_RANGE);
        CHECK_EQ(c->count, before + bad[i].published);   /* all of it dropped */
        CHECK_EQ(c->errors, errs + 1);
        for (unsigned k = 0; k < sizeof(d); k++)
            CHECK_EQ(d[k], 0xa5);                         /* nothing copied */
    }
    /* A good record after them is taken as it is. */
    raw_hdr(c, 3, 0, 0);
    memcpy(c->data + ((c->count + 16) & (c->size - 1)), "abc", 3);
    c->prod->count = c->count + 32;
    CHECK_ST(sockring_dgram_take(c, &h, d), OK);
    CHECK(h.len == 3 && h.addr == 5 && h.port == 6 && memcmp(d, "abc", 3) == 0 && d[3] == 0xa5);
    return true;
}

/* What a program refuses to take on as rings. */
static bool hostile_attach(struct rings *r)
{
    struct sockring x;
    struct sockring_info *in = &((struct sockring_page *)r->stack_map)->info;
    uint32_t tx = in->tx_size, rx = in->rx_size;
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_STREAM, tx, rx), ERR_BAD_STATE);
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes - 1, SOCKRING_DGRAM, tx, rx),
             ERR_BAD_STATE);
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_DGRAM, tx, rx * 2),
             ERR_BAD_STATE);
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_DGRAM, 5000, rx),
             ERR_BAD_STATE);
    in->magic ^= 1;
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_DGRAM, tx, rx), ERR_BAD_STATE);
    in->magic ^= 1;
    in->rx_size = rx * 2;
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_DGRAM, tx, rx), ERR_BAD_STATE);
    in->rx_size = rx;
    CHECK_ST(sockring_attach(&x, r->prog_map, r->bytes, SOCKRING_DGRAM, tx, rx), OK);
    CHECK_ST(sockring_make(&x, r->stack_map, 3, tx, rx), ERR_INVALID_ARGS);
    CHECK_ST(sockring_make(&x, r->stack_map, SOCKRING_DGRAM, 2048, rx), ERR_INVALID_ARGS);
    CHECK_ST(sockring_make(&x, r->stack_map, SOCKRING_DGRAM, tx, SOCKRING_MAX * 2),
             ERR_INVALID_ARGS);
    return true;
}

#define FUZZ_TURNS 3000u

/* Garbage everywhere the program may write, between netstack's steps:
 * netstack's counts only move forward, by at most a ring a look, its
 * copies stay inside its buffers, and nothing reads past the rings (the
 * guard page). */
static bool fuzz(struct rings *r, uint64_t seed)
{
    static uint8_t d[SOCKRING_DGRAM_MAX + 16];
    struct sockring_page *pg = (struct sockring_page *)r->prog_map;
    struct sockring_end *c = &r->s.tx, *q = &r->s.rx;
    for (unsigned t = 0; t < FUZZ_TURNS; t++) {
        uint32_t k = fuzz_rand(&seed);
        uint64_t near = c->count + (fuzz_rand(&seed) % (2 * c->size)) - c->size / 2;
        pg->tx_prod.count = k & 1 ? near : (uint64_t)fuzz_rand(&seed) << 32 | fuzz_rand(&seed);
        pg->tx_prod.flags = k & 2 ? fuzz_rand(&seed) & 3 : 0;
        pg->rx_cons.count = k & 4 ? q->count - (fuzz_rand(&seed) % (2 * q->size)) : near;
        uint32_t at = fuzz_rand(&seed) % (uint32_t)(r->bytes - 8);
        uint64_t junk = (uint64_t)fuzz_rand(&seed) << 32 | fuzz_rand(&seed);
        memcpy(r->prog_map + at, &junk, 8);   /* anywhere: lines, records, bytes */
        memset(d + SOCKRING_DGRAM_MAX, 0x5c, 16);
        uint64_t before = c->count;
        struct sockring_dgram h;
        if (c->framing == SOCKRING_DGRAM) {
            for (unsigned n = 0; n < 8 && sockring_dgram_take(c, &h, d) != ERR_SHOULD_WAIT; n++)
                CHECK(h.len <= SOCKRING_DGRAM_MAX);
            h = (struct sockring_dgram){ .len = (uint16_t)(k % 200) };
            (void)sockring_dgram_put(q, &h, d);
        } else {
            (void)sockring_stream_read(c, d, SOCKRING_DGRAM_MAX);
            (void)sockring_stream_write(q, d, k % SOCKRING_DGRAM_MAX);
        }
        (void)sockring_publish(c);
        (void)sockring_publish(q);
        CHECK(c->count >= before && c->count - before <= 8 * (uint64_t)c->size);
        CHECK(c->framing == SOCKRING_STREAM || (c->count & (SOCKRING_ALIGN - 1)) == 0);
        for (unsigned i = 0; i < 16; i++)
            CHECK_EQ(d[SOCKRING_DGRAM_MAX + i], 0x5c);
        /* The program's side reading netstack's garbage-free side survives too. */
        (void)sockring_ready(&r->p.rx);
    }
    CHECK(c->errors > 0);
    return true;
}

bool t_sockring_hostile(void)
{
    struct rings r;
    bool ok = rings_open(&r, SOCKRING_DGRAM, SOCKRING_MIN, SOCKRING_MIN) &&
              hostile_counts(&r.s, &r.p) && hostile_attach(&r);
    rings_close(&r);
    ok = ok && rings_open(&r, SOCKRING_DGRAM, SOCKRING_MIN, SOCKRING_MIN) &&
         hostile_records(&r.s) && fuzz(&r, 0x50c41e6u);
    rings_close(&r);
    ok = ok && rings_open(&r, SOCKRING_STREAM, SOCKRING_MIN, SOCKRING_MIN) &&
         fuzz(&r, 0x5743a11u);
    rings_close(&r);
    return ok;
}

/* ---- a fake netstack thread and the test as the program ------------------------------------- */

#define XCHG_DGRAMS 3000u
#define XCHG_BYTES  (1024u * 1024)
#define XCHG_WAIT   (5 * NS_PER_S)

/* The fake netstack: echoes the tx ring into the rx ring, as much as fits
 * (a real netstack drops a datagram the rx ring can't take; this one leaves
 * it in the tx ring, so every byte can be checked). At the tx ring's end it
 * ends the rx ring. */
struct fake_stack {
    struct sockring s;
    handle_t to_stack, to_prog;   /* all rights: its own */
    bool stop;                    /* set by the test */
    uint32_t need;                /* rx room the next record needs (1 for bytes) */
    uint32_t signals, waits;
    status_t st;                  /* how its loop ended */
};

static bool stack_pass(struct fake_stack *f)
{
    static uint8_t d[SOCKRING_DGRAM_MAX];
    struct sockring_end *c = &f->s.tx, *q = &f->s.rx;
    bool moved = false;
    if (c->framing == SOCKRING_DGRAM) {
        struct sockring_dgram h;
        for (;;) {   /* each record is checked for room before it is taken: peek by copy */
            struct sockring_end peek = *c;
            if (sockring_dgram_take(&peek, &h, d) != OK)
                break;
            f->need = sockring_dgram_bytes(h.len);
            if (sockring_room(q) < f->need)
                break;
            *c = peek;
            (void)sockring_dgram_put(q, &h, d);
            moved = true;
        }
    } else {
        uint32_t n = sockring_room(q);
        f->need = 1;
        n = sockring_stream_read(c, d, n < sizeof(d) ? n : (uint32_t)sizeof(d));
        moved = n && sockring_stream_write(q, d, n) == n;
        if (sockring_at_end(c) && !q->ended && sockring_finish(q)) {
            f->signals++;
            (void)jam_event_signal(f->to_prog, 0, SOCKRING_SIG_RX);
        }
    }
    if (moved && sockring_publish(q)) {
        f->signals++;
        (void)jam_event_signal(f->to_prog, 0, SOCKRING_SIG_RX);
    }
    if (moved && sockring_publish(c)) {
        f->signals++;
        (void)jam_event_signal(f->to_prog, 0, SOCKRING_SIG_TX_ROOM);
    }
    return moved;
}

static void stack_thread(void *arg)
{
    struct fake_stack *f = arg;
    (void)jam_nanosleep(now() + 20 * NS_PER_MS);   /* start late: the tx ring fills */
    while (!__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE)) {
        (void)jam_event_signal(f->to_stack, SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM, 0);
        sockring_awake(&f->s.tx);
        sockring_awake(&f->s.rx);
        if (stack_pass(f))
            continue;
        /* Nothing moved: the tx ring is empty (or done), or the rx ring has
         * no room for what is waiting in it. */
        bool idle = sockring_sleep(&f->s.tx, 1) || (sockring_at_end(&f->s.tx) && f->s.rx.ended);
        if (!idle && !sockring_sleep(&f->s.rx, f->need))
            continue;   /* room after all */
        f->waits++;
        signals_t seen = 0;
        status_t st = jam_object_wait_one(f->to_stack, SOCKRING_SIG_TX | SOCKRING_SIG_RX_ROOM,
                                          now() + XCHG_WAIT, &seen);
        if (st != OK && !__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE)) {
            f->st = st;
            return;
        }
    }
    f->st = OK;
}

/* The program's handles, with the rights netstack hands out. */
struct prog {
    struct sockring p;
    handle_t to_stack, to_prog;
    uint64_t sent, got;
    uint32_t signals, waits;
};

static bool prog_handles(struct fake_stack *f, struct prog *g, struct rings *r)
{
    CHECK_ST(jam_handle_duplicate(f->to_stack, SOCKRING_TO_STACK_RIGHTS, &g->to_stack), OK);
    CHECK_ST(jam_handle_duplicate(f->to_prog, SOCKRING_TO_PROG_RIGHTS, &g->to_prog), OK);
    /* The VMO can't be shrunk or decommitted under netstack's mapping, or copied. */
    CHECK_ST(jam_vmo_set_size(r->prog_vmo, PAGE_SIZE), ERR_ACCESS_DENIED);
    handle_t dup = HANDLE_INVALID;
    CHECK_ST(jam_handle_duplicate(r->prog_vmo, RIGHT_SAME, &dup), ERR_ACCESS_DENIED);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(g->to_stack, SOCKRING_SIG_TX, 0, &seen), ERR_ACCESS_DENIED);
    g->p = r->p;
    return true;
}

/* Send what fits (at most window ahead of what came back), take what came
 * back. True if anything moved. */
static bool prog_step(struct prog *g, bool *ok, uint64_t total, uint64_t window)
{
    static uint8_t d[SOCKRING_DGRAM_MAX];
    struct sockring_end *p = &g->p.tx, *c = &g->p.rx;
    bool put = false, took = false;
    if (p->framing == SOCKRING_DGRAM) {
        for (; g->sent < total && g->sent - g->got < window; g->sent++, put = true) {
            struct sockring_dgram h = dg_hdr(g->sent);
            dg_fill(g->sent, d);
            if (sockring_dgram_put(p, &h, d) != OK)
                break;
        }
        struct sockring_dgram h;
        for (; sockring_dgram_take(c, &h, d) == OK; g->got++, took = true)
            *ok &= dg_ok(g->got, &h, d);
    } else {
        uint32_t want = (uint32_t)(total - g->sent < 1000 ? total - g->sent : 1000);
        for (uint32_t i = 0; i < want; i++)
            d[i] = st_byte(g->sent + i);
        uint32_t n = sockring_stream_write(p, d, want);
        g->sent += n;
        put = n || (g->sent == total && !p->ended);
        n = sockring_stream_read(c, d, sizeof(d));
        for (uint32_t i = 0; i < n; i++)
            *ok &= d[i] == st_byte(g->got + i);
        g->got += n;
        took = n;
    }
    bool wake = p->framing == SOCKRING_STREAM && g->sent == total && !p->ended
                    ? sockring_finish(p) : put && sockring_publish(p);
    if (wake) {
        g->signals++;
        (void)jam_event_signal(g->to_stack, 0, SOCKRING_SIG_TX);
    }
    if (took && sockring_publish(c)) {
        g->signals++;
        (void)jam_event_signal(g->to_stack, 0, SOCKRING_SIG_RX_ROOM);
    }
    return put || took;
}

/* Nothing moved: sleep until netstack signals. room: what the next send
 * needs in the tx ring (0: the program isn't held up by the tx ring). */
static status_t prog_wait(struct prog *g, uint32_t room)
{
    (void)jam_event_signal(g->to_prog, SOCKRING_SIG_RX | SOCKRING_SIG_TX_ROOM, 0);
    bool sleep_rx = sockring_sleep(&g->p.rx, 1);
    bool sleep_tx = room ? sockring_sleep(&g->p.tx, room) : true;
    status_t st = OK;
    if (sleep_rx && sleep_tx) {
        g->waits++;
        signals_t seen = 0;
        st = jam_object_wait_one(g->to_prog, SOCKRING_SIG_RX | SOCKRING_SIG_TX_ROOM,
                                 now() + XCHG_WAIT, &seen);
    }
    sockring_awake(&g->p.rx);
    sockring_awake(&g->p.tx);
    return st;
}

static bool exchange(uint32_t framing, uint64_t total, uint64_t window)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    struct rings r;
    struct fake_stack f = { 0 };
    struct prog g = { 0 };
    bool ok = rings_open(&r, framing, SOCKRING_MIN, 2 * SOCKRING_MIN);
    handle_t th = HANDLE_INVALID;
    if (ok) {
        f.s = r.s;
        ok = jam_event_create(&f.to_stack) == OK && jam_event_create(&f.to_prog) == OK &&
             prog_handles(&f, &g, &r) &&
             thread_spawn("sockring-stack", stack_thread, &f, stack, sizeof(stack), &th) == OK;
    }
    bool data_ok = true;
    status_t st = OK;
    while (ok && st == OK && data_ok &&
           (g.got < total || (framing == SOCKRING_STREAM && !sockring_at_end(&g.p.rx))))
        if (!prog_step(&g, &data_ok, total, window))
            st = prog_wait(&g, g.sent == total || g.sent - g.got >= window ? 0
                               : framing == SOCKRING_STREAM ? 1
                               : sockring_dgram_bytes(dg_len(g.sent)));
    __atomic_store_n(&f.stop, true, __ATOMIC_RELEASE);
    if (f.to_stack)
        (void)jam_event_signal(f.to_stack, 0, SOCKRING_SIG_TX);
    bool joined = th == HANDLE_INVALID || wait_threads(&th, 1);
    handle_t hs[] = { f.to_stack, f.to_prog, g.to_stack, g.to_prog };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    rings_close(&r);
    CHECK(ok && joined);
    CHECK_ST(st, OK);
    CHECK_ST(f.st, OK);
    CHECK(data_ok);
    CHECK_EQ(g.got, total);
    CHECK(!g.p.tx.errors && !g.p.rx.errors && !f.s.tx.errors && !f.s.rx.errors);
    CHECK(g.waits > 0 && f.waits > 0);   /* both sides slept and were woken */
    printf("utest: sockring: %s, %lu each way, %u + %u signals, %u + %u waits\n",
           framing == SOCKRING_DGRAM ? "datagrams" : "bytes", (unsigned long)total, g.signals,
           f.signals, g.waits, f.waits);
    return true;
}

bool t_sockring_exchange(void)
{
    return exchange(SOCKRING_DGRAM, XCHG_DGRAMS, 64) &&
           exchange(SOCKRING_STREAM, XCHG_BYTES, XCHG_BYTES);
}
