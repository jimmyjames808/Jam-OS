/* utest: the update fetcher's window (<updfetch.h>) against a fake server
 * on a scripted clock: every request is answered (or lost, repeated,
 * delayed past later ones) as the case says, and replies nobody asked for
 * are thrown in. The fetch must end with exactly the server's bytes, or
 * fail for the right reason. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <updfetch.h>
#include "utest.h"

#define KSIZE   20000u   /* the fake kernel: 15 pieces, the last short */
#define BSIZE   9000u    /* the fake boot image: 7 pieces */
#define PIECES  (15u + 7u)
#define INFLIGHT 256u    /* replies on their way, at most */

/* A reply on its way. */
struct wire {
    uint64_t at;                     /* when it arrives */
    size_t   len;
    uint8_t  d[UPDWIRE_REP_MAX];
};

/* The fake server and the fetcher's side of the case. */
struct fake {
    uint64_t now;
    uint32_t snap;                   /* the snapshot it serves (0: none yet) */
    char     manifest[2048];         /* room for one too big for a datagram */
    size_t   manifest_len;
    uint8_t  file[UPDATE_FILES][KSIZE];
    uint32_t size[UPDATE_FILES];
    /* the case */
    unsigned drop_req, drop_rep, dup_rep;   /* every Nth lost / repeated (0: never) */
    bool     reorder;                       /* latencies vary: replies overtake */
    bool     silent;
    unsigned gone_after;                    /* GONE from this piece request on, once (0: never) */
    bool     always_gone;
    uint32_t lie_size;                      /* the boot image's file_size in replies, if set */
    /* what happened */
    unsigned requests, replies, begins, stores, piece_reqs;
    uint8_t  got[UPDATE_FILES][KSIZE];
    uint64_t room[UPDATE_FILES];            /* begin's sizes */
    struct wire q[INFLIGHT];
    unsigned nq;
};

static struct fake fk;

static void make_files(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->size[UPDATE_KERNEL] = KSIZE;
    f->size[UPDATE_BOOTFS] = BSIZE;
    uint32_t x = 1;
    for (unsigned w = 0; w < UPDATE_FILES; w++)
        for (unsigned i = 0; i < f->size[w]; i++) {
            x = x * 1664525u + 1013904223u;
            f->file[w][i] = (uint8_t)(x >> 24);
        }
    char hex[UPDATE_FILES][2 * SHA256_BYTES + 1];
    for (unsigned w = 0; w < UPDATE_FILES; w++) {
        uint8_t d[SHA256_BYTES];
        sha256(f->file[w], f->size[w], d);
        sha256_hex(d, hex[w]);
    }
    f->manifest_len = (size_t)snprintf(f->manifest, sizeof(f->manifest),
                                       "jamos-update 2\nversion 1.0\ngit 1234567\nnet vlan21\n"
                                       "kernel %u %s\n"
                                       "bootfs %u %s\nsignature\n", KSIZE, hex[0], BSIZE, hex[1]);
}

static void queue(struct fake *f, const struct updwire_rep *r, unsigned copies)
{
    for (unsigned c = 0; c < copies && f->nq < INFLIGHT; c++) {
        struct wire *w = &f->q[f->nq];
        if (updwire_rep_encode(r, w->d, sizeof(w->d), &w->len) != OK)
            return;
        /* 1 ms, or 1..37 ms when reordering: later replies overtake earlier ones */
        w->at = f->now + (f->reorder ? (1 + (f->replies * 7 + c * 13) % 37) : 1) * NS_PER_MS;
        f->nq++;
    }
}

/* The server's answer to one request (as tools/update-server.py's). */
static status_t fake_send(void *ctx, const uint8_t *d, size_t len)
{
    struct fake *f = ctx;
    struct updwire_req q;
    if (updwire_req_decode(d, len, &q) != OK)
        return ERR_INVALID_ARGS;
    f->requests++;
    if (f->silent || (f->drop_req && f->requests % f->drop_req == 0))
        return OK;   /* lost on the way */
    if (q.file == UPDWIRE_MANIFEST && !q.snapshot)
        f->snap = f->snap * 7 + 11;   /* a new snapshot */
    struct updwire_rep r = { .file = q.file, .snapshot = q.snapshot ? q.snapshot : f->snap,
                             .offset = q.offset };
    bool piece = q.file != UPDWIRE_MANIFEST;
    bool drop = piece && (f->always_gone || (f->gone_after && ++f->piece_reqs >= f->gone_after));
    if (drop && q.snapshot == f->snap) {
        f->gone_after = 0;   /* once */
        f->snap += 1000;     /* the snapshot is dropped */
    }
    if (piece && q.snapshot != f->snap) {
        r.status = UPDWIRE_GONE;
    } else {
        const uint8_t *data = q.file == UPDWIRE_MANIFEST ? (const uint8_t *)f->manifest
                                                         : f->file[q.file - 1];
        uint32_t size = q.file == UPDWIRE_MANIFEST ? (uint32_t)f->manifest_len
                                                   : f->size[q.file - 1];
        uint32_t n = size - q.offset < q.length ? size - q.offset : q.length;
        r.file_size = q.file == UPDWIRE_BOOTFS && f->lie_size ? f->lie_size : size;
        r.length = (uint16_t)n;
        r.data = data + q.offset;
    }
    f->replies++;
    queue(f, &r, f->dup_rep && f->replies % f->dup_rep == 0 ? 2 : 1);
    return OK;
}

static status_t fake_begin(void *ctx, const struct update_manifest *m, const uint8_t *text,
                           size_t len)
{
    struct fake *f = ctx;
    (void)text;
    (void)len;
    f->begins++;
    memset(f->got, 0, sizeof(f->got));
    for (unsigned w = 0; w < UPDATE_FILES; w++)
        f->room[w] = m->file[w].size;
    return OK;
}

static status_t fake_store(void *ctx, unsigned file, uint64_t off, const uint8_t *d, size_t len)
{
    struct fake *f = ctx;
    if (file >= UPDATE_FILES || off > f->room[file] || len > f->room[file] - off)
        return ERR_OUT_OF_RANGE;   /* the test checks this never happens */
    memcpy(f->got[file] + off, d, len);
    f->stores++;
    return OK;
}

static const struct updfetch_io fake_io = {
    .ctx = &fk, .send = fake_send, .begin = fake_begin, .store = fake_store,
};

/* Deliver what has arrived by fk.now, earliest first; one forged reply
 * after every `forge`th (0: none). */
static void deliver(struct updfetch *u, unsigned forge)
{
    static unsigned delivered;
    for (;;) {
        unsigned best = INFLIGHT;
        for (unsigned i = 0; i < fk.nq; i++)
            if (fk.q[i].at <= fk.now && (best == INFLIGHT || fk.q[i].at < fk.q[best].at))
                best = i;
        if (best == INFLIGHT)
            return;
        struct wire w = fk.q[best];
        fk.q[best] = fk.q[--fk.nq];
        updfetch_reply(u, w.d, w.len, fk.now);
        if (!forge || ++delivered % forge)
            continue;
        /* the same reply moved: another offset, another snapshot, a byte more */
        struct updwire_rep r;
        if (updwire_rep_decode(w.d, w.len, &r) != OK || r.status != UPDWIRE_OK || !r.length)
            continue;
        static const uint8_t junk[UPDWIRE_CHUNK_MAX];
        struct updwire_rep forged[] = {
            { r.file, UPDWIRE_OK, r.snapshot, r.offset + 1, r.file_size, r.length - 1, junk },
            { r.file, UPDWIRE_OK, r.snapshot + 1, r.offset, r.file_size, r.length, junk },
            { r.file, UPDWIRE_OK, r.snapshot, r.offset, r.file_size + 1, r.length, junk },
        };
        uint8_t d[UPDWIRE_REP_MAX];
        size_t n;
        for (unsigned i = 0; i < 3; i++)
            if (updwire_rep_encode(&forged[i], d, sizeof(d), &n) == OK)
                updfetch_reply(u, d, n, fk.now);
        updfetch_reply(u, "garbage", 7, fk.now);
    }
}

/* Run a fetch to its end on the scripted clock. */
static void run(struct updfetch *u, unsigned forge)
{
    updfetch_start(u, &fake_io);
    for (unsigned steps = 0; steps < 100000; steps++) {
        deliver(u, forge);   /* bin/update polls again after what arrived */
        uint64_t next = updfetch_poll(u, fk.now);
        if (u->state == UPDFETCH_DONE || u->state == UPDFETCH_FAILED)
            return;
        for (unsigned i = 0; i < fk.nq; i++)
            if (fk.q[i].at < next)
                next = fk.q[i].at;
        if (next == UINT64_MAX)
            return;
        fk.now = next > fk.now ? next : fk.now;
    }
}

static bool same_files(void)
{
    return !memcmp(fk.got[UPDATE_KERNEL], fk.file[UPDATE_KERNEL], KSIZE) &&
           !memcmp(fk.got[UPDATE_BOOTFS], fk.file[UPDATE_BOOTFS], BSIZE);
}

bool t_updfetch_clean(void)
{
    struct updfetch u;
    make_files(&fk);
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_DONE);
    CHECK(same_files());
    CHECK_EQ(u.sent, 1 + PIECES);   /* the manifest, then each piece once */
    CHECK_EQ(u.resent, 0);
    CHECK_EQ(fk.stores, PIECES);
    CHECK_EQ(fk.begins, 1);
    CHECK(fk.now < 100 * NS_PER_MS);   /* one round trip per window, not per piece */
    CHECK_EQ(updfetch_poll(&u, fk.now), UINT64_MAX);
    return true;
}

bool t_updfetch_lossy(void)
{
    struct updfetch u;
    make_files(&fk);
    fk.drop_req = 5;
    fk.drop_rep = 0;
    fk.dup_rep = 3;
    fk.reorder = true;
    run(&u, 4);
    CHECK_EQ(u.state, UPDFETCH_DONE);
    CHECK(same_files());
    CHECK_EQ(fk.stores, PIECES);   /* each piece stored once: repeats and forgeries ignored */
    CHECK(u.resent > 0 && u.ignored > 0);
    return true;
}

bool t_updfetch_snapshot_gone(void)
{
    struct updfetch u;
    make_files(&fk);
    fk.gone_after = 5;
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_DONE);
    CHECK(same_files());
    CHECK_EQ(fk.begins, 2);   /* the manifest again, and the files from the start */
    CHECK_EQ(u.restarts, 1);
    make_files(&fk);
    fk.always_gone = true;
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_FAILED);
    CHECK_ST(u.why, ERR_NOT_FOUND);
    CHECK_EQ(fk.begins, UPDFETCH_RESTARTS + 1);
    return true;
}

bool t_updfetch_failures(void)
{
    struct updfetch u;
    /* nobody answers: given up after UPDFETCH_TRIES sends of the manifest */
    make_files(&fk);
    fk.silent = true;
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_FAILED);
    CHECK_ST(u.why, ERR_TIMED_OUT);
    CHECK_EQ(fk.requests, UPDFETCH_TRIES);
    CHECK(fk.now >= (UPDFETCH_TRIES - 1) * UPDFETCH_RETRY);
    /* a manifest that doesn't parse */
    make_files(&fk);
    fk.manifest[0] = 'J';
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_FAILED);
    CHECK_ST(u.why, ERR_INVALID_ARGS);
    CHECK_EQ(fk.begins, 0);
    /* a server whose boot image isn't the size its manifest says: its
     * replies match nothing, and the requests time out */
    make_files(&fk);
    fk.lie_size = BSIZE + 1;
    run(&u, 0);
    CHECK_EQ(u.state, UPDFETCH_FAILED);
    CHECK_ST(u.why, ERR_TIMED_OUT);
    CHECK(!memcmp(fk.got[UPDATE_KERNEL], fk.file[UPDATE_KERNEL], KSIZE));
    /* a manifest that fits a datagram but not the format's limit */
    make_files(&fk);
    memset(fk.manifest, 'x', sizeof(fk.manifest));
    fk.manifest_len = UPDATE_MANIFEST_MAX + 1;
    run(&u, 0);
    CHECK_ST(u.why, ERR_INVALID_ARGS);
    /* one too big for a datagram: the reply can't be all of it */
    fk.manifest_len = UPDWIRE_CHUNK_MAX + 1;
    run(&u, 0);
    CHECK_ST(u.why, ERR_OUT_OF_RANGE);
    return true;
}
