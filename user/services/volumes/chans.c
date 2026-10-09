/* volumes: its channels. init gives it the server ends of a control
 * channel (VOLUMES_SR_CTL) and, in shell mode, the ESP channel
 * (VOLUMES_SR_ESP: ESP_WRITE alone, which no other channel may ask). The
 * control channel also answers the svc protocol's connect
 * (abi/idl/svc.idl) with a new control channel of the caller's own, so
 * each opener of /svc/volumes has one, and a reply that comes after its
 * caller gave up (a MOUNTS reply with its handles) waits on that caller's
 * channel, not a shared one. Every channel is watched on volumes' port,
 * read here and answered by request.c. What a channel may ask is decided
 * by which channel it is, never by who sent the request (<volumes.h>
 * "Trust").
 *
 * The table is bounded (MAX_CHANS): a connect asked for past it is
 * ERR_NO_RESOURCES. An opener's channel whose clients are all gone is
 * dropped (its MOUNTS calls first: mounts_forget), and its slot is free
 * again; volumes lives as long as the clients of slot 0 do (init's control
 * channel). (devmgr's chans.c, which served these methods before the
 * storage split, is the model.) */
#include <idl/svc.h>
#include "internal.h"

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

static bool allowed(const struct chan *c, uint32_t ordinal)
{
    /* The ESP made writable: init's ESP channel only, and that is all it may ask. */
    if (ordinal == VOLUMES_ESP_WRITE || c->lv == LEVEL_ESP)
        return ordinal == VOLUMES_ESP_WRITE && c->lv == LEVEL_ESP;
    return true;
}

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

/* svc.connect on c: a control channel of the caller's own, if c is a
 * control channel (the ESP channel hands out nothing). */
static void on_connect(const struct chan *c, uint32_t txid)
{
    struct svc_connect_rep r = { .txid = txid, .status = ERR_NOT_SUPPORTED };
    handle_t h = HANDLE_INVALID;
    if (c->lv == LEVEL_CONTROL)
        r.status = add_control(&h);
    /* h goes with the reply either way (K1): if the caller is gone, so is
     * the new channel's only client, and its slot is dropped. */
    (void)jam_channel_write(c->h, &r, sizeof(r), &h, r.status == OK ? 1 : 0);
}

/* Answer everything queued on c. Returns ERR_SHOULD_WAIT once the queue is
 * empty, ERR_PEER_CLOSED once every client is gone and nothing is left to
 * read. */
static status_t serve(const struct chan *c)
{
    handle_t ch = c->h;
    for (;;) {
        _Alignas(8) uint8_t buf[64];
        handle_t in[4];
        uint32_t n = 0, nh = 0;
        struct channel_read_args a = {
            .h = ch, .rd_cap = sizeof(buf), .rd_bytes = (uint64_t)(uintptr_t)buf,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)in,
            .rd_hcap = 4, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(ch, n, nh);   /* nothing of ours is that big */
            continue;
        }
        if (st != OK)
            return st;
        /* Only TEST_DISK and SET_DISKS carry a handle (one), and only on a
         * control channel. */
        const struct volumes_req *q = (const struct volumes_req *)buf;
        bool denied = n >= 8 && !allowed(c, q->ordinal);
        bool whole = !denied && n == sizeof(*q);
        bool takes_handle = whole && nh == 1 && (q->ordinal == VOLUMES_TEST_DISK ||
                                                 q->ordinal == VOLUMES_SET_DISKS);
        if (!takes_handle)
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(in[i]);
        if (n < 4)
            continue;   /* no txid: nobody to answer */
        if (n == sizeof(struct svc_connect_req) && !nh && q->ordinal == SVC_CONNECT) {
            on_connect(c, q->txid);
            continue;
        }
        if (whole && !nh && q->ordinal == VOLUMES_MOUNTS) {
            mounts_request(ch, q->txid, q->instance);   /* answered now or later */
            continue;
        }
        struct volumes_rep r = { .txid = q->txid, .status = ERR_INVALID_ARGS };
        handle_t hs[VOLUMES_MAX_HANDLES];
        uint32_t nout = 0;
        if (denied) {
            r.status = ERR_ACCESS_DENIED;
        } else if (takes_handle && q->ordinal == VOLUMES_SET_DISKS) {
            disks_set(in[0]);
            r.status = OK;
        } else if (takes_handle) {
            uint32_t id = 0;
            r.status = disk_test(in[0], &id);
            r.a = id;
        } else if (whole && !nh) {
            request_handle(q, c->lv, &r, hs, &nout);
        }
        uint32_t rn = r.status == OK ? sizeof(r) : VOLUMES_REP_HDR;
        (void)jam_channel_write(ch, &r, rn, hs, nout);   /* the handles go either way (K1) */
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
    mounts_forget(c->h);
    jam_handle_close(c->h);
    uint32_t gen = c->gen + 1;   /* a packet already queued for it is stale */
    *c = (struct chan){ .gen = gen };
}

bool chans_init(void)
{
    chans[SLOT_LIFE] = (struct chan){ .h = startup_handle(VOLUMES_SR_CTL), .lv = LEVEL_CONTROL };
    chans[SLOT_ESP] = (struct chan){ .h = startup_handle(VOLUMES_SR_ESP), .lv = LEVEL_ESP };
    /* The ESP channel's end receives no handles (ESP_WRITE carries none):
     * no queue of ours can hold a channel end there (M12's D2). */
    if (chans[SLOT_ESP].h && jam_channel_set_flags(chans[SLOT_ESP].h, CHANNEL_NO_HANDLES) != OK)
        say(false, "volumes: the ESP channel can't be marked: it takes handles");
    return chans[SLOT_LIFE].h != HANDLE_INVALID;
}

status_t chans_serve(void)
{
    for (unsigned i = 0; i < MAX_CHANS; i++) {
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
