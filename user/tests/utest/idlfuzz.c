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
#include "disks.h"
#include "nettest.h"
#include "utest.h"

#define ROUNDS  1000u         /* messages per protocol, each way */
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
        /* A str: a path more often than not ("/", names, "." and ".."), so a
         * server that takes paths gets past its first check. */
        static const char letters[] = "/abcab/.x/..yz~1";
        for (uint32_t i = 0; i < bytes; i++)
            b[at + i] = v->str ? (uint8_t)letters[rnd(f) % (sizeof(letters) - 1)]
                               : (uint8_t)rnd(f);
        if (v->str && bytes && rnd(f) % 2)
            b[at] = '/';
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

/* ---- live servers: fat and volumes fed the same messages ---------------------------- */

static const struct idl_fuzz_proto *proto_named(const char *name)
{
    for (unsigned i = 0; i < IDL_FUZZ_PROTOS; i++)
        if (!strcmp(idl_fuzz_protos[i].name, name))
            return &idl_fuzz_protos[i];
    return NULL;
}

/* Write the n bytes at b with nh handles hs (gone either way) on ch, then
 * take every answer that has come, until the one to its txid (a request
 * with a txid must be answered within 5 s). The status of the write, or
 * ERR_PEER_CLOSED, or ERR_TIMED_OUT for a request never answered. */
static status_t send_take(struct fz *f, handle_t ch, uint8_t *b, uint32_t n, handle_t *hs,
                          uint32_t nh)
{
    static uint8_t rep[CHANNEL_MAX_BYTES];
    uint32_t txid = 0;
    if (n >= 4) {   /* a txid of ours never has the kernel's bit: its answer would be dropped */
        memcpy(&txid, b, 4);
        txid &= ~CHANNEL_TXID_KERNEL;
        memcpy(b, &txid, 4);
    }
    status_t st = jam_channel_write(ch, b, n, hs, nh);
    drop_peers(f);
    if (st != OK)
        return st;
    uint64_t until = now() + 5 * NS_PER_S;
    for (;;) {
        struct idl_msg m;
        st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if (st == OK || st == ERR_INTERNAL) {
            idl_msg_drop(&m);   /* a view or a file it made, or the like: closed */
            if (txid && m.txid == txid)
                return OK;
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (!txid)
            return OK;   /* nothing to wait for: no txid, never answered */
        if (now() >= until)
            return ERR_TIMED_OUT;
        signals_t seen;
        (void)jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, until, &seen);
    }
}

static struct ramdisk hdisk;

#define HOSTILE_ROUNDS 1600u
#define HOSTILE_DRIVER_NS (2 * NS_PER_S)   /* how long the hostile storage driver answers */

/* fat over a RAM disk, its `fs` protocol on a view and its `file` protocol
 * on an open file fed HOSTILE_ROUNDS messages made as idl_fuzz makes them
 * (paths among the strings, so requests get past the decoding): every
 * request with a txid is answered, neither channel is closed on us, fat
 * still answers properly after, and when it ends its job is empty (no
 * view, file, handle or message left behind). */
bool t_fat_hostile(void)
{
    struct fatrun r;
    struct tfile file;
    struct fz f = { .seed = 0xfa7b0057ull };
    handle_t v;
    const struct idl_fuzz_proto *pfs = proto_named("fs"), *pfile = proto_named("file");
    CHECK(pfs && pfile);
    if (!ramdisk_create(&hdisk, 16 * MIB_SECTORS) || !fat_start(&r, &hdisk, false))
        return false;
    CHECK(put_file(&r, "/a", "a file the messages may find"));
    CHECK_ST(t_mkdir(&r, "/b"), OK);
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, 0, &v), OK);
    CHECK_ST(t_open(&r, "/a", FS_READ | FS_WRITE, &file), OK);
    static uint8_t msg[AREA];
    for (unsigned k = 0; k < HOSTILE_ROUNDS; k++) {
        bool on_file = k % 2;
        const struct idl_fuzz_proto *p = on_file ? pfile : pfs;
        const struct idl_fuzz_msg *m = &p->msgs[rnd(&f) % p->nmsgs];
        uint32_t n = build(&f, p, m, msg, AREA);
        handle_t hs[IDL_HANDLES_MAX];
        uint32_t nh = handles_for(&f, m, hs);
        status_t st = send_take(&f, on_file ? file.ch : v, msg, n, hs, nh);
        if (st != OK)
            FAIL("round %u: a %s message of %u bytes (ordinal %08x, %u handles): %s", k,
                 p->name, n, m->ordinal, nh, status_str(st));
    }
    uint64_t id = 0, size = 0;
    uint32_t done = 0;
    char back[32] = { 0 };
    CHECK_ST(t_stat_id(&r, "/b", &id), OK);
    CHECK_ST(t_read(&file, 0, back, sizeof(back) - 1, &done), OK);
    CHECK_ST(t_stat(&r, "/a", &size, NULL, NULL), OK);
    t_close(&file);
    CHECK_ST(jam_handle_close(v), OK);
    return fat_stop(&r);
}

/* A volumes request the hostile test must not make: it would stop
 * volumes, take a real disk's mounts away or start a disk of its own. */
static bool harmful(uint32_t ordinal)
{
    switch (ordinal) {
    case VOLUMES_REMOUNT:
    case VOLUMES_KILL:
    case VOLUMES_TEST_DISK:
    case VOLUMES_RELEASE_DISK:
    case VOLUMES_SYNC:
    case VOLUMES_STOP:
    case VOLUMES_SET_DISKS:
        return true;
    default:
        return false;
    }
}

/* One request of volumes' on the hostile driver's channel ch, answered
 * badly (junk, the right txid with the wrong answer, handles, extreme
 * values) and then, mostly, as a driver would. */
static void answer_badly(struct fz *f, handle_t ch, const uint8_t *q, uint32_t n)
{
    uint8_t junk[300];
    const struct idl_req_hdr *h = (const void *)q;
    struct idl_txn txn = { ch, n >= 4 ? h->txid : 0 };
    uint32_t ordinal = n >= 8 ? h->ordinal : 0;
    for (unsigned k = rnd(f) % 4; k--;) {   /* noise first */
        uint32_t len = rnd(f) % sizeof(junk);
        for (uint32_t i = 0; i < len; i++)
            junk[i] = (uint8_t)rnd(f);
        if (len >= 4 && rnd(f) % 2)
            memcpy(junk, &txn.txid, 4);
        handle_t ev = HANDLE_INVALID;
        if (rnd(f) % 3 == 0)
            (void)jam_event_create(&ev);
        (void)jam_channel_write(ch, junk, len, &ev, ev ? 1 : 0);   /* ev goes either way */
    }
    if (ordinal != STORAGE_INFO && rnd(f) % 5 == 0)
        return;   /* never answered (but info: volumes would wait 30 s for it) */
    static const uint8_t guid[16];
    static const uint8_t types[] = { 0xef, 0x0c, 0x00, 0xee, 0x0b, 0x83 };
    if (ordinal == STORAGE_INFO) {
        uint32_t parts = rnd(f) % 3 == 0 ? 0xffffffffu : rnd(f) % 6;
        (void)storage_reply_info(txn, OK, "hostile", 7, "disk", 4, rnd(f) % 4 ? 512 : 0,
                                 rnd(f) % 2 ? UINT64_MAX : 8192, (uint8_t)rnd(f), parts,
                                 rnd(f), guid);
    } else if (ordinal == STORAGE_PARTITION) {
        (void)storage_reply_partition(txn, rnd(f) % 6 ? OK : ERR_IO,
                                      types[rnd(f) % sizeof(types)], guid, guid, rnd(f),
                                      rnd(f) % 2 ? UINT64_MAX : 4096, 0, "p", 1);
    } else if (ordinal == STORAGE_OPEN_PARTITION) {
        handle_t ev = HANDLE_INVALID;   /* not a `block` channel at all */
        (void)jam_event_create(&ev);
        (void)storage_reply_open_partition(txn, rnd(f) % 3 ? OK : ERR_NO_RESOURCES, ev);
    } else {
        (void)jam_channel_write(ch, q, n < 8 ? n : 8, NULL, 0);
    }
}

/* volumes' control channel fed messages made as idl_fuzz makes them (none
 * that would stop it, take a mount away or add a disk), then a test disk
 * whose `storage` driver answers garbage for 2 s and goes: volumes keeps
 * answering, and the disk leaves no mount behind. */
bool t_volumes_hostile(void)
{
    handle_t vm = volumes(), mine, theirs;
    struct fz f = { .seed = 0x701b0057ull };
    struct view v = { 0 };
    uint32_t state = 0, restarts = 0, backoff = 0, promoted = 0, id = 0;
    uint8_t spare = 0;
    const struct idl_fuzz_proto *p = proto_named("volumes");
    if (!vm)
        return true;
    CHECK(p);
    static uint8_t msg[AREA];
    unsigned sent = 0;
    for (unsigned k = 0; k < 800; k++) {
        const struct idl_fuzz_msg *m = &p->msgs[rnd(&f) % p->nmsgs];
        uint32_t n = build(&f, p, m, msg, AREA), ordinal = 0;
        if (n >= 8)
            memcpy(&ordinal, msg + 4, 4);
        if (n >= 8 && harmful(ordinal))
            continue;
        handle_t hs[IDL_HANDLES_MAX];
        uint32_t nh = handles_for(&f, m, hs);
        status_t st = send_take(&f, vm, msg, n, hs, nh);
        if (st != OK)
            FAIL("message %u (ordinal %08x, %u bytes, %u handles): %s", k, ordinal, n, nh,
                 status_str(st));
        sent++;
    }
    CHECK_ST(volumes_supervision_until(vm, now() + 5 * NS_PER_S, "/nothing", 8, &state, &restarts,
                                       &backoff, &promoted, &spare), ERR_NOT_FOUND);
    /* The test mounts there are now (an earlier test's, still going):
     * after the hostile disk, no other. */
    CHECK_ST(view_next(vm, &v, now() + WAIT_MOUNT), OK);
    char before[VIEW_MAX][16];
    unsigned nbefore = 0;
    for (unsigned i = 0; i < v.n; i++)
        if (v.m[i].flags & MOUNTS_TEST)
            memcpy(before[nbefore++], v.m[i].path, sizeof(before[0]));
    /* The hostile driver. */
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    CHECK_ST(volumes_test_disk_until(vm, now() + 10 * NS_PER_S, theirs, &id), OK);
    unsigned asked = 0;
    for (uint64_t until = now() + HOSTILE_DRIVER_NS; now() < until;) {
        uint8_t q[256];
        handle_t hs[8];
        uint32_t n = 0, nh = 0;
        status_t st = jam_channel_read(&(struct channel_read_args){
            .h = mine, .rd_cap = sizeof(q), .rd_bytes = (uint64_t)(uintptr_t)q,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = 8, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        });
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            (void)jam_object_wait_one(mine, SIG_READABLE, until, &seen);
            continue;
        }
        CHECK_ST(st, OK);   /* volumes keeps a disk it was given while its driver is there */
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        answer_badly(&f, mine, q, n);
        asked++;
    }
    CHECK_ST(jam_handle_close(mine), OK);   /* the driver goes */
    printf("utest: volumes_hostile: %u messages to volumes, %u requests to the hostile driver\n",
           sent, asked);
    CHECK(asked > 0);
    /* volumes still answers, and nothing of the disk stays mounted */
    CHECK_ST(volumes_supervision_until(vm, now() + 5 * NS_PER_S, "/nothing", 8, &state, &restarts,
                                       &backoff, &promoted, &spare), ERR_NOT_FOUND);
    if (!wait_mounts(vm, &v, false, false))
        return false;
    for (unsigned i = 0; i < v.n; i++) {
        bool old = false;
        for (unsigned k = 0; k < nbefore && !old; k++)
            old = !strcmp(before[k], v.m[i].path);
        if ((v.m[i].flags & MOUNTS_TEST) && !old)
            FAIL("the hostile disk left a mount behind: %s", v.m[i].path);
    }
    view_close(&v);
    return true;
}
