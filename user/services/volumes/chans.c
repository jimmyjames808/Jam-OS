/* volumes: its channels. init gives it the server ends of a control
 * channel (VOLUMES_SR_CTL: the `volumes` protocol) and, in shell mode, the
 * ESP channel (VOLUMES_SR_ESP: `volesp` alone, which no other channel
 * speaks). A control channel also answers the svc protocol's connect
 * (abi/idl/svc.idl) with a new control channel of the caller's own, so
 * each opener of /svc/volumes has one, and a reply that comes after its
 * caller gave up waits on that caller's channel, not a shared one. Every
 * channel is watched on volumes' port, read here and answered by
 * request.c's handlers (the generated dispatch, <idl/volumes.h>). What a
 * channel may ask is decided by which channel it is, never by who sent the
 * request (<volumes.h> "Trust").
 *
 * The table is bounded (MAX_CHANS): a connect asked for past it is
 * ERR_NO_RESOURCES. An opener's channel whose clients are all gone is
 * dropped, and its slot is free again; volumes lives as long as the
 * clients of slot 0 do (init's control channel). */
#include <idl/svc.h>
#include "internal.h"

/* The biggest request and reply of the protocols the channels speak. */
#define REQ_MAX (VOLUMES_REQ_MAX > VOLESP_REQ_MAX ? VOLUMES_REQ_MAX : VOLESP_REQ_MAX)
#define REP_MAX (VOLUMES_REP_MAX > VOLESP_REP_MAX ? VOLUMES_REP_MAX : VOLESP_REP_MAX)
_Static_assert(SVC_REQ_MAX <= REQ_MAX && SVC_REP_MAX <= REP_MAX, "a connect fits");

#define MAX_CHANS 16   /* init's control and ESP channels, the openers' */
#define SLOT_LIFE 0    /* the channel volumes lives by */
#define SLOT_ESP  1

struct chan {
    handle_t   h;       /* our end (0: a free slot) */
    enum level lv;      /* what it may ask */
    uint32_t   gen;     /* the slot's generation, in its port key */
    bool       armed;   /* bound to the port (ONCE), not fired yet */
};

static struct chan chans[MAX_CHANS];

void discard(handle_t h, uint32_t n, uint32_t nh)
{
    uint8_t *big = malloc(n ? n : 1);
    handle_t *bh = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = h, .rd_cap = n, .rd_bytes = (uint64_t)(uintptr_t)big,
        .rd_actual = (uint64_t)(uintptr_t)&n2, .rd_handles = (uint64_t)(uintptr_t)bh,
        .rd_hcap = nh, .rd_hactual = (uint64_t)(uintptr_t)&nh2,
    };
    if (big && bh && jam_channel_read(&a) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            jam_handle_close(bh[i]);
    free(big);
    free(bh);
}

static status_t add_control(handle_t *out);

/* svc.connect on a control channel: a control channel of the caller's
 * own. */
static status_t s_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    return add_control(out);
}

static const struct svc_ops svc_ops = { .connect = s_connect };

/* One request q (n bytes, nh handles in hs, which go to the dispatch) on
 * c: the reply into r and rhs. Returns its length (0: none). */
static uint32_t dispatch(const struct chan *c, const void *q, uint32_t n, const handle_t *hs,
                         uint32_t nh, void *r, handle_t *rhs, uint32_t *rhn)
{
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    *rhn = 0;
    if (c->lv == LEVEL_ESP) {   /* volesp alone; it carries no handles */
        if (nh) {
            idl_close_all(hs, nh);
            idl_reply_status(c->h, q, n, ERR_INVALID_ARGS);
            return 0;
        }
        return volesp_dispatch_on(c->h, &volesp_ops, NULL, q, n, r, rhs, rhn);
    }
    if (n >= sizeof(*h) && h->ordinal >> 16 == SVC_PROTOCOL_ID) {
        if (nh) {
            idl_close_all(hs, nh);
            idl_reply_status(c->h, q, n, ERR_INVALID_ARGS);
            return 0;
        }
        return svc_dispatch_on(c->h, &svc_ops, NULL, q, n, r, rhs, rhn);
    }
    return volumes_dispatch_h(c->h, &volumes_ops, NULL, q, n, hs, nh, r, rhs, rhn);
}

/* Answer everything queued on c. Returns ERR_SHOULD_WAIT once the queue is
 * empty, ERR_PEER_CLOSED once every client is gone and nothing is left to
 * read. */
static status_t serve(const struct chan *c)
{
    handle_t ch = c->h;
    for (;;) {
        _Alignas(8) uint8_t q[REQ_MAX], r[REP_MAX];
        handle_t hs[VOLUMES_REQ_HANDLES], rhs[VOLUMES_REP_HANDLES];
        uint32_t n = 0, nh = 0, rhn = 0;
        status_t st = jam_channel_read(&(struct channel_read_args){
            .h = ch, .rd_cap = sizeof(q), .rd_bytes = (uint64_t)(uintptr_t)q,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = VOLUMES_REQ_HANDLES, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        });
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(ch, n, nh);   /* nothing of ours is that big */
            continue;
        }
        if (st != OK)
            return st;
        uint32_t rn = dispatch(c, q, n, hs, nh, r, rhs, &rhn);
        if (rn)
            (void)jam_channel_write(ch, r, rn, rhs, rhn);   /* the handles go either way */
        else
            idl_close_all(rhs, rhn);
        mounts_list_new();   /* a watch's end has gone out: its list follows */
        if (stopping)
            return ERR_SHOULD_WAIT;   /* volumes.stop: nothing more is served */
        sup_run_due();   /* a long burst of requests mustn't hold up a restart */
    }
}

static uint64_t key_of(unsigned i)
{
    return KEY_CHAN_OF(i, chans[i].gen);
}

/* Slot i: its channel closed and forgotten. */
static void drop(unsigned i)
{
    struct chan *c = &chans[i];
    if (c->armed)
        (void)jam_port_unbind(port, c->h, key_of(i));   /* not fired: nothing else to undo */
    jam_handle_close(c->h);
    uint32_t gen = c->gen + 1;   /* a packet already queued for it is stale */
    *c = (struct chan){ .gen = gen };
}

bool chans_init(void)
{
    chans[SLOT_LIFE] = (struct chan){ .h = startup_handle(VOLUMES_SR_CTL), .lv = LEVEL_CONTROL };
    chans[SLOT_ESP] = (struct chan){ .h = startup_handle(VOLUMES_SR_ESP), .lv = LEVEL_ESP };
    /* The ESP channel's end receives no handles (volesp carries none):
     * no queue of ours can hold a channel end there (M12's D2). */
    if (chans[SLOT_ESP].h && jam_channel_set_flags(chans[SLOT_ESP].h, CHANNEL_NO_HANDLES) != OK)
        say(false, "volumes: the ESP channel can't be marked: it takes handles");
    return chans[SLOT_LIFE].h != HANDLE_INVALID;
}

status_t chans_serve(void)
{
    for (unsigned i = 0; i < MAX_CHANS && !stopping; i++) {
        if (!chans[i].h)
            continue;
        status_t st = serve(&chans[i]);
        if (st == ERR_PEER_CLOSED && i != SLOT_LIFE)
            drop(i);   /* nobody asks on it any more */
        else if (st != ERR_SHOULD_WAIT)
            return st;
    }
    return ERR_SHOULD_WAIT;
}

/* ONCE, re-armed after it fires (it fires at once if a message came in
 * meanwhile); a service's death arrives on the same port, and a due
 * restart ends the wait. */
status_t chans_arm(void)
{
    for (unsigned i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &chans[i];
        if (!c->h || c->armed)
            continue;
        status_t st = jam_port_bind(port, c->h, key_of(i), SIG_READABLE | SIG_PEER_CLOSED,
                                    PORT_BIND_ONCE);
        if (st != OK)
            return st;
        c->armed = true;
    }
    return ERR_SHOULD_WAIT;
}

bool chans_packet(uint64_t key)
{
    if (!(key & KEY_CHAN))
        return false;
    unsigned i = KEY_INDEX(key);
    if (i < MAX_CHANS && chans[i].h && KEY_GEN(key) == (chans[i].gen & 0xffffu))
        chans[i].armed = false;
    return true;   /* ours, stale or not */
}

/* A new control channel in a free slot, watched by the next chans_arm;
 * *out: its client end. */
static status_t add_control(handle_t *out)
{
    for (unsigned i = SLOT_ESP + 1; i < MAX_CHANS; i++) {
        struct chan *c = &chans[i];
        if (c->h)
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        c->h = mine;
        c->lv = LEVEL_CONTROL;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}
