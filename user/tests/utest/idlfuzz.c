/* utest: the generated servers fed hostile messages. genidl writes
 * <idl/fuzz.h>: every protocol's generated dispatch (and its client's
 * event dispatch) over stub handlers that close what they are given and
 * answer an error or every result at its maximum, and the shape of every
 * message. Each protocol gets messages made from those shapes (valid ones,
 * and ones cut short, lengthened, with counts past their maxima, bad UTF-8,
 * bad padding, a byte flipped, another protocol's ordinal, no txid), with
 * handles of every kind (a typed one mostly of its type), and:
 *   - the request ends at an unmapped page, and the reply buffer at one
 *     (with a canary up to it): a read past the message or a write past
 *     <PROTO>_REP_MAX faults;
 *   - every handle given is gone after the dispatch (taken by a stub, or
 *     closed by the generated code);
 *   - every request with a txid is answered, with that txid, and an answer
 *     is never longer than REP_MAX nor carries more than REP_HANDLES.
 * A protocol added to abi/idl is in the table by `make idl`: nothing here
 * names one. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <idl/fuzz.h>
#include "nettest.h"
#include "utest.h"

#define ROUNDS  240u          /* messages per protocol, each way */
#define AREA    (20u << 10)   /* room before each guard page: a message's 16 KiB and more */
#define CANARY  0xc5u

/* A buffer of AREA bytes ending at a page with no access. */
struct fz_guard {
    uint8_t *base;
};

static bool fz_guard_open(struct fz_guard *g)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), vmo;
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(AREA + PAGE_SIZE, 0, &vmo), OK);
    status_t st = jam_vmar_map(vmar, vmo, 0, AREA + PAGE_SIZE, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    CHECK_ST(st, OK);
    CHECK_ST(jam_vmar_protect(vmar, addr + AREA, PAGE_SIZE, 0), OK);
    g->base = (uint8_t *)(uintptr_t)addr;
    return true;
}

static void fz_guard_close(struct fz_guard *g)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)g->base,
                         AREA + PAGE_SIZE);   /* a test's own mapping */
}

/* The run's state: the generator, and the other ends of channels given. */
struct fz {
    uint64_t seed;
    handle_t peers[IDL_HANDLES_MAX];
    unsigned npeers;
    uint64_t dispatches, answered, full;
};

static uint32_t rnd(struct fz *f)
{
    return fuzz_rand(&f->seed);
}

/* A new handle of `type` (IDL_OBJ_*; 0 or another: an event), a channel's
 * other end kept in f->peers. */
static handle_t make(struct fz *f, uint32_t type)
{
    handle_t h = HANDLE_INVALID, peer = HANDLE_INVALID;
    status_t st;
    if (type == IDL_OBJ_VMO)
        st = jam_vmo_create(PAGE_SIZE, 0, &h);
    else if (type == IDL_OBJ_CHANNEL && f->npeers < IDL_HANDLES_MAX)
        st = jam_channel_create(&h, &peer);
    else if (type == IDL_OBJ_PORT)
        st = jam_port_create(&h);
    else
        st = jam_event_create(&h);
    if (st != OK)
        return HANDLE_INVALID;
    if (peer)
        f->peers[f->npeers++] = peer;
    return h;
}

/* A handle for a message: of `want` mostly (0: any kind), else any. */
static handle_t pick(struct fz *f, uint32_t want)
{
    static const uint32_t kinds[] = { IDL_OBJ_EVENT, IDL_OBJ_VMO, IDL_OBJ_CHANNEL, IDL_OBJ_PORT };
    if (want && rnd(f) % 8)
        return make(f, want);
    return make(f, kinds[rnd(f) % 4]);
}

static void drop_peers(struct fz *f)
{
    for (unsigned i = 0; i < f->npeers; i++)
        jam_handle_close(f->peers[i]);
    f->npeers = 0;
}

/* Message m (or junk with its ordinal) into b (cap bytes): its length. */
static uint32_t build(struct fz *f, const struct idl_fuzz_proto *p, const struct idl_fuzz_msg *m,
                      uint8_t *b, uint32_t cap)
{
    static const uint8_t bad[] = { 0x00, 0xff, 0xc0, 0x80, 0xed, 0xf5, 0xe0, '/' };
    uint32_t r = rnd(f);
    bool mutate = r % 3 == 0;
    uint32_t n = m->fixed < cap ? m->fixed : cap;
    for (uint32_t i = 8; i < n; i++)
        b[i] = (uint8_t)rnd(f);
    uint32_t txid = m->way == 0 ? (rnd(f) & ~CHANNEL_TXID_KERNEL) | 1u : 0;
    if (r % 13 == 0)
        txid = m->way == 0 ? 0 : rnd(f);   /* a call without a txid, an event with one */
    uint32_t ordinal = m->ordinal;
    if (r % 11 == 0)   /* another method of the protocol, another protocol's, or anything */
        ordinal = r % 2 ? (p->id << 16) | (rnd(f) % 24) : rnd(f);
    memcpy(b, &txid, 4);
    memcpy(b + 4, &ordinal, 4);
    for (uint32_t k = 0; k < m->nvar && n + 2 <= cap; k++) {
        const struct idl_fuzz_var *v = &m->var[k];
        uint32_t count = rnd(f) % (v->max + 1u);
        if (rnd(f) % 4 == 0)
            count = v->max;   /* the edge, often */
        if (mutate && rnd(f) % 6 == 0)
            count = v->max + 1u + rnd(f) % 3;   /* past it */
        uint32_t at = n + 2;
        while (at % v->size)
            at++;
        if (at > cap)
            break;
        if (count > (cap - at) / v->size)
            count = (cap - at) / v->size;
        uint16_t c16 = (uint16_t)(count > 0xffffu ? 0xffffu : count);
        memcpy(b + n, &c16, 2);
        for (uint32_t i = n + 2; i < at; i++)
            b[i] = mutate && rnd(f) % 5 == 0 ? 0xaa : 0;   /* the padding: zeros, mostly */
        uint32_t bytes = count * v->size;
        for (uint32_t i = 0; i < bytes; i++)
            b[at + i] = v->str ? (uint8_t)('a' + rnd(f) % 26) : (uint8_t)rnd(f);
        if (v->str && bytes && mutate && rnd(f) % 3 == 0)
            b[at + rnd(f) % bytes] = bad[rnd(f) % sizeof(bad)];
        n = at + bytes;
    }
    if (mutate) {
        uint32_t how = rnd(f) % 3;
        if (how == 0 && n)
            n -= 1 + rnd(f) % (n < 16 ? n : 16);   /* cut short */
        else if (how == 1)
            for (uint32_t k = rnd(f) % 16 + 1; k-- && n < cap;)
                b[n++] = (uint8_t)rnd(f);          /* junk after it */
        else if (n > 8)
            b[8 + rnd(f) % (n - 8)] ^= (uint8_t)(1u << (rnd(f) % 8));
    }
    if (r % 97 == 0)
        n = rnd(f) % 8;   /* not even a header */
    return n;
}

/* The handles for m: its single ones (typed as it says, mostly), then its
 * list's; a wrong count now and then. */
static uint32_t handles_for(struct fz *f, const struct idl_fuzz_msg *m, handle_t *hs)
{
    uint32_t nh = m->singles;
    if (m->list)
        nh += rnd(f) % (m->list + 1u);
    if (rnd(f) % 7 == 0)
        nh = nh ? nh - 1 : 1;
    else if (rnd(f) % 7 == 0)
        nh++;
    if (nh > IDL_HANDLES_MAX)
        nh = IDL_HANDLES_MAX;
    for (uint32_t i = 0; i < nh; i++) {
        uint32_t want = i < m->singles ? (i < sizeof(m->types) ? m->types[i] : 0) : m->list_type;
        hs[i] = pick(f, want);
    }
    return nh;
}

/* Each of the nh handles given must be gone. */
static bool all_gone(const handle_t *hs, uint32_t nh, const char *proto)
{
    for (uint32_t i = 0; i < nh; i++) {
        struct handle_info info;
        if (hs[i] && jam_handle_info(hs[i], &info) == OK)
            FAIL("%s: handle %u of %u given was left open", proto, i, nh);
    }
    return true;
}

/* One request (or event) to p's server. */
static bool one_request(struct fz *f, const struct idl_fuzz_proto *p, struct fz_guard *qg,
                        struct fz_guard *rg)
{
    static uint8_t msg[AREA];
    const struct idl_fuzz_msg *m = &p->msgs[rnd(f) % p->nmsgs];
    if (m->way == 2)
        return true;   /* the client's: one_event */
    uint32_t n = build(f, p, m, msg, AREA);
    uint8_t *q = qg->base + AREA - n;
    memcpy(q, msg, n);
    handle_t hs[IDL_HANDLES_MAX], rhs[IDL_HANDLES_MAX];
    uint32_t nh = handles_for(f, m, hs), rhn = 0;
    uint32_t room = (p->rep_max + 7u) & ~7u;
    CHECK(room <= AREA && p->rep_handles <= IDL_HANDLES_MAX);
    uint8_t *r = rg->base + AREA - room;
    memset(r, CANARY, room);
    idl_fuzz_full = rnd(f) % 2;
    f->full += idl_fuzz_full;
    uint32_t rn = p->serve(q, n, hs, nh, r, rhs, &rhn);
    f->dispatches++;
    bool ok = all_gone(hs, nh, p->name);
    drop_peers(f);
    for (uint32_t i = 0; i < rhn && i < IDL_HANDLES_MAX; i++)
        jam_handle_close(rhs[i]);
    if (!ok)
        return false;
    if (rn > p->rep_max || rhn > p->rep_handles)
        FAIL("%s: ordinal %08x: an answer of %u bytes and %u handles (most %u and %u)", p->name,
             m->ordinal, rn, rhn, p->rep_max, p->rep_handles);
    for (uint32_t i = p->rep_max; i < room; i++)
        if (r[i] != CANARY)
            FAIL("%s: ordinal %08x: byte %u past REP_MAX written", p->name, m->ordinal, i);
    uint32_t txid = 0;
    if (n >= 4)
        memcpy(&txid, q, 4);
    if (rn) {
        f->answered++;
        uint32_t back = 0;
        memcpy(&back, r, 4);
        if (rn < sizeof(struct idl_rep_hdr) || back != txid)
            FAIL("%s: an answer of %u bytes with txid %08x to txid %08x", p->name, rn, back, txid);
        if (rhn && ((const struct idl_rep_hdr *)r)->status != OK)
            FAIL("%s: a failure answered with %u handles", p->name, rhn);
    } else if (n >= sizeof(struct idl_req_hdr) && txid && m->way == 0 &&
               ((const struct idl_req_hdr *)q)->ordinal == m->ordinal) {
        FAIL("%s: ordinal %08x with txid %08x not answered", p->name, m->ordinal, txid);
    }
    return true;
}

/* One event to p's client (its generated event dispatch). */
static bool one_event(struct fz *f, const struct idl_fuzz_proto *p, struct fz_guard *qg)
{
    static uint8_t msg[AREA];
    static struct idl_msg im;
    const struct idl_fuzz_msg *m = &p->msgs[rnd(f) % p->nmsgs];
    uint32_t n = build(f, p, m, msg, AREA);
    uint8_t *q = qg->base + AREA - n;
    memcpy(q, msg, n);
    im.n = n;
    im.txid = n >= 4 ? ((const struct idl_req_hdr *)q)->txid : 0;
    im.nh = handles_for(f, m, im.hs);
    handle_t given[IDL_HANDLES_MAX];
    uint32_t nh = im.nh;
    memcpy(given, im.hs, nh * sizeof(handle_t));
    (void)p->event(q, &im);
    f->dispatches++;
    bool ok = all_gone(given, nh, p->name);
    drop_peers(f);
    if (!ok)
        return false;
    CHECK_EQ(im.nh, 0);   /* taken, whatever happened */
    return true;
}

bool t_idl_fuzz(void)
{
    struct fz_guard qg, rg;
    struct fz f = { .seed = 0x1d1f022ull };
    if (!fz_guard_open(&qg) || !fz_guard_open(&rg))
        return false;
    bool ok = true;
    unsigned events = 0;
    for (unsigned i = 0; ok && i < IDL_FUZZ_PROTOS; i++) {
        const struct idl_fuzz_proto *p = &idl_fuzz_protos[i];
        for (unsigned k = 0; ok && k < ROUNDS; k++)
            ok = one_request(&f, p, &qg, &rg);
        for (unsigned k = 0; ok && p->event && k < ROUNDS; k++, events++)
            ok = one_event(&f, p, &qg);
    }
    idl_fuzz_full = false;
    fz_guard_close(&qg);
    fz_guard_close(&rg);
    if (ok)
        printf("utest: idl_fuzz: %u protocols, %lu messages (%u to clients), %lu answered, %lu "
               "with full stubs\n", (unsigned)IDL_FUZZ_PROTOS, (unsigned long)f.dispatches, events,
               (unsigned long)f.answered, (unsigned long)f.full);
    return ok;
}
