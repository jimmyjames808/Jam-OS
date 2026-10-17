/* utest: the keep channel (<keep.h>; its messages are the IDL protocol
 * keepch, abi/idl/keepch.idl), both sides in this one process: a
 * fake service puts and drops slots and "dies" (closes its handles as the
 * kernel would), the keeper (libos's) keeps their objects alive and
 * restores them to a fake successor, which takes the slots its state
 * knows. Also: restores come in batches of whole slots of at most 64
 * handles, the keeper refuses what it must not hold (kinds, counts, caps,
 * malformed messages), holds and restores what extfs needs (past 1,024
 * handles), and a successor refuses a malformed restore. Every
 * test ends with this job holding exactly the handles it started with. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <keep.h>
#include <os.h>
#include "utest.h"

#define SOON (now() + 2 * NS_PER_S)   /* restore deadline: everything is queued already */

static struct keeper k;   /* the keeper of the test running (too big for a stack) */

static uint64_t handles_used(void)
{
    struct job_info ji;
    return info_of(own_job(), &ji) == OK ? ji.used[JOB_LIMIT_HANDLES] : 0;
}

/* The peer of client end c is closed (no deadline: now). */
static bool peer_gone(handle_t c)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(c, SIG_PEER_CLOSED, 0, &seen);
    return st == OK && (seen & SIG_PEER_CLOSED);
}

/* keeper_take until the queue is empty. */
static void take_all(void)
{
    while (keeper_take(&k) == OK)
        ;
}

/* A fake successor's state: the slots it knows, and what it was handed. */
struct state {
    uint32_t known[8];          /* slot numbers the state has */
    unsigned nknown;
    handle_t got[8][KEEP_SLOT_HANDLES];   /* handles of known[i], once taken */
    unsigned ngot[8];
};

static bool state_take(void *ctx, uint32_t slot, const handle_t *hs, unsigned n)
{
    struct state *s = ctx;
    for (unsigned i = 0; i < s->nknown; i++)
        if (s->known[i] == slot && !s->ngot[i]) {
            for (unsigned j = 0; j < n; j++)
                s->got[i][j] = hs[j];
            s->ngot[i] = n;
            return true;
        }
    return false;
}

static void state_close(struct state *s)
{
    for (unsigned i = 0; i < s->nknown; i++)
        for (unsigned j = 0; j < s->ngot[i]; j++)
            jam_handle_close(s->got[i][j]);
}

static bool keep_all(void *ctx, uint32_t slot, const handle_t *hs, unsigned n)
{
    (void)slot;
    unsigned *count = ctx;
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(hs[i]);
    ++*count;
    return true;
}

/* Put, drop, the service dies, its successor gets the slots back: the
 * same objects (a client's request queued meanwhile is there), the same
 * rights; a slot dropped before the death is gone (its client sees
 * PEER_CLOSED); the keeper keeps them across the successor's death too. */
bool t_keep_put_drop_restore(void)
{
    uint64_t before = handles_used();
    keeper_init(&k);
    handle_t svc, srv, cli, srv2, cli2, buf, ro, ev;
    CHECK_ST(keeper_attach(&k, &svc), OK);
    CHECK_ST(jam_channel_create(&srv, &cli), OK);
    CHECK_ST(jam_channel_create(&srv2, &cli2), OK);
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &buf), OK);
    CHECK_ST(jam_vmo_write(buf, 0, "hello", 5), OK);
    CHECK_ST(jam_handle_duplicate(buf, RIGHT_READ | RIGHT_MAP | RIGHTS_BASIC, &ro), OK);
    CHECK_ST(jam_event_create(&ev), OK);

    CHECK_ST(keep_put(svc, 7, (handle_t[]){ srv, buf }, 2), OK);
    CHECK_ST(keep_put(svc, 9, (handle_t[]){ srv2 }, 1), OK);
    CHECK_ST(keep_put(svc, 11, (handle_t[]){ ro, ev }, 2), OK);
    CHECK_ST(keep_drop(svc, 9), OK);
    take_all();
    CHECK(k.nslots == 2 && k.nhandles == 4 && k.puts == 3 && k.drops == 1 && !k.refused);

    /* The service dies: the kernel closes everything it held. */
    jam_handle_close(svc);
    jam_handle_close(srv);
    jam_handle_close(srv2);
    jam_handle_close(buf);
    jam_handle_close(ro);
    jam_handle_close(ev);
    CHECK(!peer_gone(cli));   /* kept */
    CHECK(peer_gone(cli2));   /* dropped */
    uint32_t req[2] = { 0, 42 };
    CHECK_ST(jam_channel_write(cli, req, sizeof(req), NULL, 0), OK);

    handle_t svc2;
    CHECK_ST(keeper_attach(&k, &svc2), OK);
    CHECK_ST(keeper_restore(&k), OK);
    struct state s = { .known = { 7, 11 }, .nknown = 2 };
    struct keep_restored got;
    CHECK_ST(keep_restore(svc2, SOON, state_take, &s, &got), OK);
    CHECK(got.slots == 2 && got.taken == 2 && got.handles == 4);
    CHECK(s.ngot[0] == 2 && s.ngot[1] == 2);

    uint32_t rep[2] = { 0 }, nb = 0, nh = 0;
    struct channel_read_args a = {
        .h = s.got[0][0], .rd_cap = sizeof(rep), .rd_bytes = (uint64_t)(uintptr_t)rep,
        .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&a), OK);   /* the request the client sent while it was dead */
    CHECK(nb == sizeof(rep) && rep[1] == 42);
    char text[6] = { 0 };
    CHECK_ST(jam_vmo_read(s.got[0][1], 0, text, 5), OK);
    CHECK(!strcmp(text, "hello"));
    CHECK_ST(jam_vmo_write(s.got[1][0], 0, "x", 1), ERR_ACCESS_DENIED);   /* rights kept */
    CHECK_ST(jam_event_signal(s.got[1][1], 0, SIG_SIGNALED), OK);

    /* The successor dies too: the keeper still holds every slot. */
    state_close(&s);
    jam_handle_close(svc2);
    CHECK(!peer_gone(cli));
    keeper_release(&k);
    CHECK(peer_gone(cli));
    jam_handle_close(cli);
    jam_handle_close(cli2);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* What the fake successor of t_keep_restore_batches reads. */
struct tally {
    unsigned msgs, slots, total;   /* restore events, their slots and handles */
    bool     sums;                 /* every restore's counts add up to its handles */
    bool     done;                 /* the done event came ... */
    uint32_t done_slots, done_handles;   /* ... with these totals */
};

static void tally_restore(void *ctx, const uint32_t *slots, uint32_t ns, const uint8_t *counts,
                          uint32_t nc, const handle_t *hs, uint32_t nh)
{
    struct tally *t = ctx;
    unsigned sum = 0;
    for (unsigned i = 0; i < nc; i++)
        sum += counts[i];
    t->sums = t->sums && ns == nc && sum == nh && nh <= KEEP_BATCH && slots != NULL;
    for (unsigned i = 0; i < nh; i++)
        jam_handle_close(hs[i]);
    t->msgs++;
    t->slots += ns;
    t->total += nh;
}

static void tally_done(void *ctx, uint32_t slots, uint32_t handles)
{
    struct tally *t = ctx;
    t->done = true;
    t->done_slots = slots;
    t->done_handles = handles;
}

/* 100 slots of 2 handles come back as restore events of whole slots, none
 * over 64 handles, then a done with the totals. */
bool t_keep_restore_batches(void)
{
    uint64_t before = handles_used();
    keeper_init(&k);
    handle_t svc, svc2;
    CHECK_ST(keeper_attach(&k, &svc), OK);
    for (uint32_t i = 0; i < 100; i++) {
        handle_t pair[2];
        CHECK_ST(jam_event_create(&pair[0]), OK);
        CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &pair[1]), OK);
        CHECK_ST(keep_put(svc, 1000 + i, pair, 2), OK);
        jam_handle_close(pair[0]);
        jam_handle_close(pair[1]);
        CHECK_ST(keeper_take(&k), OK);
    }
    CHECK(k.nslots == 100 && k.nhandles == 200 && !k.refused);
    CHECK_ST(keeper_attach(&k, &svc2), OK);
    CHECK_ST(keeper_restore(&k), OK);

    static const struct keepch_events ev = { .restore = tally_restore, .done = tally_done };
    struct tally t = { .sums = true };
    while (!t.done)
        CHECK_ST(keepch_read_event(svc2, &ev, &t), OK);
    CHECK(t.sums && t.done_slots == 100 && t.done_handles == 200);
    CHECK(t.msgs == 4 && t.slots == 100 && t.total == 200);   /* 32 + 32 + 32 + 4 slots */
    CHECK_ST(keepch_read_event(svc2, &ev, &t), ERR_SHOULD_WAIT);

    /* A second restore, read by keep_restore. */
    CHECK_ST(keeper_restore(&k), OK);
    unsigned count = 0;
    struct keep_restored got;
    CHECK_ST(keep_restore(svc2, SOON, keep_all, &count, &got), OK);
    CHECK(count == 100 && got.slots == 100 && got.taken == 100 && got.handles == 200);
    keeper_release(&k);
    jam_handle_close(svc);
    jam_handle_close(svc2);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* Slots the successor's state doesn't know (the service died between its
 * put and recording the slot) are closed and dropped by the keeper too:
 * their clients see PEER_CLOSED; the known one's doesn't. */
bool t_keep_unknown_slots(void)
{
    uint64_t before = handles_used();
    keeper_init(&k);
    handle_t svc, svc2, srv[3], cli[3];
    CHECK_ST(keeper_attach(&k, &svc), OK);
    for (unsigned i = 0; i < 3; i++) {
        CHECK_ST(jam_channel_create(&srv[i], &cli[i]), OK);
        CHECK_ST(keep_put(svc, i, &srv[i], 1), OK);
        jam_handle_close(srv[i]);
    }
    jam_handle_close(svc);
    CHECK_ST(keeper_attach(&k, &svc2), OK);
    CHECK(k.nslots == 3);
    CHECK_ST(keeper_restore(&k), OK);
    struct state s = { .known = { 1 }, .nknown = 1 };
    struct keep_restored got;
    CHECK_ST(keep_restore(svc2, SOON, state_take, &s, &got), OK);
    CHECK(got.slots == 3 && got.taken == 1 && got.handles == 1);
    take_all();   /* the successor's two drops */
    CHECK(k.nslots == 1 && k.drops == 2);
    CHECK(peer_gone(cli[0]) && !peer_gone(cli[1]) && peer_gone(cli[2]));
    state_close(&s);
    keeper_release(&k);
    jam_handle_close(svc2);
    for (unsigned i = 0; i < 3; i++)
        jam_handle_close(cli[i]);
    CHECK_EQ(handles_used(), before);
    return true;
}

static bool kept(uint32_t slot)
{
    for (unsigned i = 0; i < KEEP_MAX_SLOTS; i++)
        if (k.slot[i].n && k.slot[i].id == slot)
            return true;
    return false;
}

/* Write m (nb bytes) on ch with hs[0..nh) (moved), or with nh fresh events
 * when hs is NULL. */
static status_t raw(handle_t ch, const void *m, uint32_t nb, const handle_t *hs, unsigned nh)
{
    handle_t ev[8] = { 0 };
    if (nh > 8)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < nh; i++) {
        if (hs)
            ev[i] = hs[i];
        else if (jam_event_create(&ev[i]) != OK)
            return ERR_NO_RESOURCES;
    }
    /* ev is gone whatever the write's status (a send's handles are always
     * moved): never closed after it. */
    return jam_channel_write(ch, m, nb, ev, nh);
}

/* The keeper refuses (and counts) the message queued on its end, keeps
 * nothing as slot 1, and leaves nothing behind in the queue. */
static bool refused_one(void)
{
    uint32_t refused = k.refused, puts = k.puts;
    CHECK_ST(keeper_take(&k), OK);
    CHECK(k.refused == refused + 1 && k.puts == puts && !kept(1));
    CHECK_ST(keeper_take(&k), ERR_SHOULD_WAIT);
    return true;
}

/* A put of h (consumed) alone as slot 1 is refused. */
static bool refused_kind(handle_t svc, handle_t h)
{
    status_t st = keep_put(svc, 1, &h, 1);
    jam_handle_close(h);
    CHECK_ST(st, OK);
    return refused_one();
}

/* A put of h (consumed) alone as slot 1 is kept; then dropped. */
static bool kept_kind(handle_t svc, handle_t h)
{
    status_t st = keep_put(svc, 1, &h, 1);
    jam_handle_close(h);
    CHECK_ST(st, OK);
    CHECK_ST(keeper_take(&k), OK);
    CHECK(kept(1));
    CHECK_ST(keep_drop(svc, 1), OK);
    CHECK_ST(keeper_take(&k), OK);
    CHECK(!kept(1));
    return true;
}

/* The kinds the keeper keeps and refuses, as handle_info tells them
 * apart. */
static bool keep_kinds(handle_t svc)
{
    handle_t h, x, y;
    CHECK_ST(jam_port_create(&h), OK);
    CHECK(refused_kind(svc, h));
    CHECK_ST(jam_timer_create(&h), OK);
    CHECK(refused_kind(svc, h));
    CHECK_ST(new_job(&h), OK);
    CHECK(refused_kind(svc, h));
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_SELF_PROCESS), RIGHT_SAME, &h), OK);
    CHECK(refused_kind(svc, h));
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_SELF_THREAD), RIGHT_SAME, &h), OK);
    CHECK(refused_kind(svc, h));
    CHECK_ST(jam_handle_duplicate(startup_handle(SR_SELF_VMAR), RIGHT_SAME, &h), OK);
    CHECK(refused_kind(svc, h));

    CHECK_ST(jam_channel_create(&x, &y), OK);   /* a channel end whose peer has gone */
    jam_handle_close(y);
    CHECK(kept_kind(svc, x));
    /* handle_info tells a channel end whatever its state and rights: one
     * we may not wait on, and one whose peer's queue is full (neither
     * writable nor peer-closed) were refused while the keeper probed. */
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_handle_replace(x, RIGHT_DUPLICATE | RIGHT_TRANSFER, &h), OK);
    CHECK(kept_kind(svc, h));
    jam_handle_close(y);
    CHECK_ST(jam_channel_create(&x, &y), OK);
    for (unsigned i = 0; i < CHANNEL_MAX_QUEUED; i++)
        CHECK_ST(jam_channel_write(x, "full", 4, NULL, 0), OK);
    CHECK_ST(jam_channel_write(x, "full", 4, NULL, 0), ERR_SHOULD_WAIT);
    CHECK(kept_kind(svc, x));
    jam_handle_close(y);
    CHECK_ST(jam_event_create(&x), OK);         /* an event without RIGHT_SIGNAL */
    CHECK_ST(jam_handle_replace(x, RIGHTS_BASIC, &h), OK);
    CHECK(kept_kind(svc, h));
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &x), OK);   /* a read-only VMO */
    CHECK_ST(jam_handle_replace(x, RIGHT_READ | RIGHTS_BASIC, &h), OK);
    CHECK(kept_kind(svc, h));

    /* A VMO nobody may duplicate: keep_put won't send it, and the keeper
     * refuses it when it comes anyway. */
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &x), OK);
    CHECK_ST(jam_handle_replace(x, RIGHT_READ | RIGHT_TRANSFER, &h), OK);
    CHECK_ST(keep_put(svc, 1, &h, 1), ERR_ACCESS_DENIED);
    struct keepch_put_msg m = { 0, KEEPCH_PUT, 1 };
    CHECK_ST(raw(svc, &m, sizeof(m), &h, 1), OK);
    return refused_one();
}

/* Malformed messages: each refused (by the generated server, which the
 * keeper counts), its handles closed, the queue clear. */
static bool keep_malformed(handle_t svc)
{
    struct keepch_put_msg m = { 0, KEEPCH_PUT, 1 };
    CHECK_ST(raw(svc, &m, 11, NULL, 1), OK);   /* short */
    CHECK(refused_one());
    m.txid = 5;
    CHECK_ST(raw(svc, &m, sizeof(m), NULL, 1), OK);   /* a txid */
    CHECK(refused_one());
    m.txid = 0;
    CHECK_ST(raw(svc, &m, sizeof(m), NULL, 0), OK);   /* a put of nothing */
    CHECK(refused_one());
    CHECK_ST(raw(svc, &m, sizeof(m), NULL, KEEP_SLOT_HANDLES + 1), OK);   /* too many for a slot */
    CHECK(refused_one());
    CHECK_ST(raw(svc, &m, sizeof(m) + 1, NULL, 1), OK);   /* a byte too many */
    CHECK(refused_one());
    struct keepch_drop_msg d = { 0, KEEPCH_DROP, 1 };
    CHECK_ST(raw(svc, &d, sizeof(d), NULL, 1), OK);   /* a drop with a handle */
    CHECK(refused_one());
    struct keepch_done_msg wrong = { 0, KEEPCH_DONE, 1, 1 };
    CHECK_ST(raw(svc, &wrong, sizeof(wrong), NULL, 0), OK);   /* an event of the other way */
    CHECK(refused_one());
    struct keepch_put_msg other = { 0, (KEEPCH_PROTOCOL_ID << 16) | 9, 1 };
    CHECK_ST(raw(svc, &other, sizeof(other), NULL, 1), OK);   /* an unknown ordinal */
    CHECK(refused_one());
    other.ordinal = KEEPCH_PUT ^ (1u << 16);                  /* another protocol's put */
    CHECK_ST(raw(svc, &other, sizeof(other), NULL, 1), OK);
    CHECK(refused_one());
    static uint8_t big[300];
    CHECK_ST(raw(svc, big, sizeof(big), NULL, 3), OK);   /* too big: thrown away whole */
    CHECK(refused_one());
    /* A drop of a slot that was never kept is no error: handled, not refused. */
    uint32_t refused = k.refused, drops = k.drops;
    CHECK_ST(keep_drop(svc, 77), OK);
    CHECK_ST(keeper_take(&k), OK);
    CHECK(k.refused == refused && k.drops == drops);
    return true;
}

/* Put one fresh event as slot id; keeper_take it. */
static bool put_event(handle_t svc, uint32_t id, unsigned n)
{
    handle_t e[KEEP_SLOT_HANDLES];
    for (unsigned j = 0; j < n; j++)
        CHECK_ST(jam_event_create(&e[j]), OK);
    status_t st = keep_put(svc, id, e, n);
    for (unsigned j = 0; j < n; j++)
        jam_handle_close(e[j]);
    CHECK_ST(st, OK);
    CHECK_ST(keeper_take(&k), OK);
    return true;
}

/* At most KEEP_MAX_SLOTS slots and KEEP_MAX_HANDLES handles; a second put
 * of a slot replaces it (the old duplicates closed). */
static bool keep_caps(void)
{
    handle_t svc;
    keeper_init(&k);
    CHECK_ST(keeper_attach(&k, &svc), OK);
    for (uint32_t i = 0; i < KEEP_MAX_SLOTS + 1; i++)
        CHECK(put_event(svc, 100 + i, 1));
    CHECK(k.nslots == KEEP_MAX_SLOTS && k.refused == 1 && !kept(100 + KEEP_MAX_SLOTS));
    keeper_release(&k);
    jam_handle_close(svc);

    keeper_init(&k);
    CHECK_ST(keeper_attach(&k, &svc), OK);
    for (uint32_t i = 0; i < KEEP_MAX_HANDLES / KEEP_SLOT_HANDLES + 1; i++)
        CHECK(put_event(svc, i, KEEP_SLOT_HANDLES));
    CHECK(k.nhandles == KEEP_MAX_HANDLES && k.refused == 1);
    keeper_release(&k);
    jam_handle_close(svc);

    keeper_init(&k);
    handle_t a, ca, b, cb;
    CHECK_ST(keeper_attach(&k, &svc), OK);
    CHECK_ST(jam_channel_create(&a, &ca), OK);
    CHECK_ST(jam_channel_create(&b, &cb), OK);
    CHECK_ST(keep_put(svc, 5, &a, 1), OK);
    CHECK_ST(keep_put(svc, 5, &b, 1), OK);
    take_all();
    jam_handle_close(a);
    jam_handle_close(b);
    CHECK(k.replaced == 1 && k.nslots == 1 && k.nhandles == 1);
    CHECK(peer_gone(ca) && !peer_gone(cb));
    keeper_release(&k);
    jam_handle_close(svc);
    jam_handle_close(ca);
    jam_handle_close(cb);
    return true;
}

/* What the keeper refuses: the kinds it doesn't keep, malformed messages
 * and too many slots or handles. */
bool t_keep_refusals(void)
{
    uint64_t before = handles_used();
    keeper_init(&k);
    handle_t svc;
    CHECK_ST(keeper_attach(&k, &svc), OK);
    bool ok = keep_kinds(svc) && keep_malformed(svc);
    keeper_release(&k);
    jam_handle_close(svc);
    CHECK(ok);
    CHECK(keep_caps());
    CHECK_EQ(handles_used(), before);
    return true;
}

/* The room extfs needs with its watchers and directory channels (<extfs.h>,
 * keepch.idl's MAX_HANDLES): 928 slots holding 1,216 handles, past the
 * 1,024 a keeper held before. 288 slots of 2 (an open file's channel and
 * VMO, a watcher's channel and bell) and 640 of 1 (views, maps, directory
 * channels): all kept, then restored whole to a successor. */
#define ROOM_PAIRS  288u
#define ROOM_ONES   640u
#define ROOM_SLOTS  (ROOM_PAIRS + ROOM_ONES)
#define ROOM_HANDLES (2 * ROOM_PAIRS + ROOM_ONES)

bool t_keep_room(void)
{
    uint64_t before = handles_used();
    keeper_init(&k);
    handle_t svc, svc2;
    CHECK_ST(keeper_attach(&k, &svc), OK);
    for (uint32_t i = 0; i < ROOM_SLOTS; i++)
        CHECK(put_event(svc, 7000 + i, i < ROOM_PAIRS ? 2 : 1));
    CHECK(k.nslots == ROOM_SLOTS && k.nhandles == ROOM_HANDLES && !k.refused);
    CHECK(ROOM_HANDLES > 1024 && ROOM_HANDLES <= KEEP_MAX_HANDLES);
    CHECK_ST(keeper_attach(&k, &svc2), OK);
    CHECK_ST(keeper_restore(&k), OK);
    unsigned count = 0;
    struct keep_restored got;
    CHECK_ST(keep_restore(svc2, SOON, keep_all, &count, &got), OK);
    CHECK(count == ROOM_SLOTS && got.slots == ROOM_SLOTS && got.taken == ROOM_SLOTS);
    CHECK_EQ(got.handles, ROOM_HANDLES);
    CHECK(k.nslots == ROOM_SLOTS && k.nhandles == ROOM_HANDLES);   /* still the keeper's */
    keeper_release(&k);
    jam_handle_close(svc);
    jam_handle_close(svc2);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* A fake keeper's messages are queued on kp (the keeper's end, ours to
 * close) for the successor's end sp: keep_restore's status, both ends
 * closed. deadline_ns 0: the keeper closes its end after them. */
static status_t run_restore(handle_t kp, handle_t sp, uint64_t deadline_ns)
{
    if (!deadline_ns) {
        jam_handle_close(kp);
        kp = HANDLE_INVALID;
        deadline_ns = SOON;
    }
    unsigned count = 0;
    status_t st = keep_restore(sp, deadline_ns, keep_all, &count, NULL);
    if (kp != HANDLE_INVALID)
        jam_handle_close(kp);
    jam_handle_close(sp);
    return st;
}

/* A restore event for ns slots, counts[0..nc), with nh fresh events. */
static status_t send_restore(handle_t kp, const uint32_t *slots, unsigned ns,
                             const uint8_t *counts, unsigned nc, unsigned nh)
{
    handle_t ev[KEEP_BATCH + 1];
    if (nh > KEEP_BATCH + 1)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < nh; i++) {
        status_t st = jam_event_create(&ev[i]);
        if (st != OK)
            return st;
    }
    return keepch_restore(kp, slots, ns, counts, nc, ev, nh);
}

/* A keeper that listens to a running service stops after a bound; if
 * messages are left it says so, for its loop to come back to (a binding
 * fires on an edge only, and the mixer under churn writes as fast as init
 * reads: before this, it found the queue full until its next restart). */
bool t_keep_drain_bounded(void)
{
    uint64_t before = handles_used();
    handle_t svc;
    keeper_init(&k);
    CHECK_ST(keeper_attach(&k, &svc), OK);
    enum { N = 10 };
    for (uint32_t i = 0; i < N; i++) {
        handle_t e;
        CHECK_ST(jam_event_create(&e), OK);
        CHECK_ST(keep_put(svc, i, &e, 1), OK);
        jam_handle_close(e);
    }
    CHECK(keeper_drain_upto(&k, 4));    /* 6 left */
    CHECK_EQ(k.puts, 4u);
    CHECK(keeper_drain_upto(&k, 4));    /* 2 left */
    CHECK_EQ(k.puts, 8u);
    CHECK(!keeper_drain_upto(&k, 4));   /* the last two: empty */
    CHECK_EQ(k.puts, 10u);
    CHECK(!keeper_drain_upto(&k, 4));   /* nothing */
    CHECK(!keeper_drain(&k));
    for (uint32_t i = 0; i < 4; i++) {  /* exactly the bound queued: not "more" */
        handle_t e;
        CHECK_ST(jam_event_create(&e), OK);
        CHECK_ST(keep_put(svc, 20 + i, &e, 1), OK);
        jam_handle_close(e);
    }
    CHECK(!keeper_drain_upto(&k, 4));
    CHECK_EQ(k.puts, 14u);
    keeper_release(&k);
    jam_handle_close(svc);
    CHECK_EQ(handles_used(), before);
    return true;
}

/* A successor refuses a malformed restore (and closes what it carried),
 * and gives up at its deadline or when the keeper goes. */
bool t_keep_restore_refusals(void)
{
    uint64_t before = handles_used();
    static const uint32_t three[1] = { 3 }, twice[2] = { 3, 3 };
    static const uint8_t one[1] = { 1 }, ones[2] = { 1, 1 }, two[1] = { 2 }, zero[1] = { 0 },
                         five[1] = { KEEP_SLOT_HANDLES + 1 };
    handle_t kp, sp, e;

    /* A good one: a restore, then its done. */
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(keepch_done(kp, 1, 1), OK);
    CHECK_ST(run_restore(kp, sp, SOON), OK);

    /* No slots; a count of 0; one over a slot's most; counts that disagree
     * with the handles (too few and too many); counts for fewer slots than
     * there are; one slot twice. */
    static const struct {
        const uint32_t *slots; unsigned ns;
        const uint8_t *counts; unsigned nc;
        unsigned nh;
    } bad[] = {
        { three, 0, one, 0, 0 },
        { three, 1, zero, 1, 0 },
        { three, 1, five, 1, KEEP_SLOT_HANDLES + 1 },
        { three, 1, two, 1, 1 },
        { three, 1, one, 1, 2 },
        { twice, 2, one, 1, 2 },
        { twice, 2, ones, 2, 2 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_ST(jam_channel_create(&kp, &sp), OK);
        CHECK_ST(send_restore(kp, bad[i].slots, bad[i].ns, bad[i].counts, bad[i].nc, bad[i].nh),
                 OK);
        CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    }

    /* The same slot in two messages. */
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);

    /* Totals in done that don't match what came; a done before any restore
     * claiming slots. */
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(keepch_done(kp, 1, 2), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(keepch_done(kp, 1, 0), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);   /* nothing kept: done(0, 0) is right */
    CHECK_ST(keepch_done(kp, 0, 0), OK);
    CHECK_ST(run_restore(kp, sp, SOON), OK);

    /* An event a keeper doesn't send (a put), one that breaks the format (a
     * restore cut short, a done with a txid), one too big, a reply. */
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(keepch_put(kp, 1, &e, 1), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    struct keepch_restore_msg h = { 0, KEEPCH_RESTORE };
    uint8_t cutmsg[sizeof(h) + 1] = { 0 };   /* a restore's header and a stray byte */
    memcpy(cutmsg, &h, sizeof(h));
    CHECK_ST(raw(kp, cutmsg, sizeof(cutmsg), NULL, 1), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    struct keepch_done_msg dt = { 5, KEEPCH_DONE, 0, 0 };
    CHECK_ST(raw(kp, &dt, sizeof(dt), NULL, 0), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    static uint8_t big[1024];
    CHECK_ST(raw(kp, big, sizeof(big), NULL, 2), OK);
    CHECK_ST(run_restore(kp, sp, SOON), ERR_INVALID_ARGS);

    /* No done: a deadline passes, or the keeper goes. */
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(run_restore(kp, sp, now() + 20 * NS_PER_MS), ERR_TIMED_OUT);
    CHECK_ST(jam_channel_create(&kp, &sp), OK);
    CHECK_ST(send_restore(kp, three, 1, one, 1, 1), OK);
    CHECK_ST(run_restore(kp, sp, 0), ERR_PEER_CLOSED);
    CHECK_EQ(handles_used(), before);
    return true;
}
