/* devmgr: its channels. init gives it the server ends of a control channel
 * (SR_DEVMGR_CTL), a query channel (SR_DEVMGR) and, in shell mode, the ESP
 * channel (DEVMGR_SR_ESP: ESP_WRITE alone, which no other channel may
 * ask), and asks the control
 * channel for device channels (DEVMGR_DEVICE_CHANNEL), each scoped to one
 * device and made here. A query channel also answers the svc protocol's
 * connect (abi/idl/svc.idl) with a new query channel of the caller's own,
 * so each opener of /svc/devmgr has one and a reply that comes after its
 * caller gave up waits on that caller's channel, not a shared one. Every
 * channel is watched on devmgr's port, read here and answered by
 * request.c. What a channel may ask is decided by which channel it is
 * (its level, and a device channel's device), never by who sent the
 * request (<devmgr.h> "Trust").
 *
 * The table is bounded (MAX_CHANS): a device channel or a connect asked for
 * past it is ERR_NO_RESOURCES. A query or device channel whose clients are
 * all gone is dropped, and its slot is free again; devmgr lives as long as
 * the clients of slot 0 do (the control channel, or without one the query
 * channel). */
#include <idl/svc.h>
#include "internal.h"

#define MAX_CHANS 32   /* the control and query channels, the openers', device channels */
#define SLOT_LIFE 0    /* the channel devmgr lives by */

struct chan {
    handle_t   h;       /* our end (0: a free slot) */
    enum level lv;      /* what it may ask */
    uint32_t   dev;     /* LEVEL_DEVICE: devs index of its device */
    uint32_t   gen;     /* the slot's generation, in its port key */
    bool       armed;   /* bound to the port (ONCE), not fired yet */
};

static struct chan chans[MAX_CHANS];

/* What the query channel may ask; the control channel may ask everything. */
static bool query_ok(uint32_t ordinal)
{
    return ordinal == DEVMGR_STATUS || ordinal == DEVMGR_GET_SERVICE ||
           ordinal == DEVMGR_GET_DRIVER || ordinal == DEVMGR_SUPERVISION;
}

/* What a device channel may ask (about its device alone: request.c). */
static bool device_ok(uint32_t ordinal)
{
    return ordinal == DEVMGR_GET_SERVICE || ordinal == DEVMGR_GET_DRIVER ||
           ordinal == DEVMGR_SUPERVISION;
}

static bool allowed(const struct chan *c, uint32_t ordinal)
{
    /* The ESP made writable: init's ESP channel only, and that is all it may ask. */
    if (ordinal == DEVMGR_ESP_WRITE || c->lv == LEVEL_ESP)
        return ordinal == DEVMGR_ESP_WRITE && c->lv == LEVEL_ESP;
    return c->lv == LEVEL_CONTROL || (c->lv == LEVEL_QUERY && query_ok(ordinal)) ||
           (c->lv == LEVEL_DEVICE && device_ok(ordinal));
}

static status_t add(enum level lv, uint32_t dev, handle_t *out);

/* svc.connect on c: a query channel of the caller's own, if c is a query
 * channel (no other kind hands them out). */
static void on_connect(const struct chan *c, uint32_t txid)
{
    struct svc_connect_rep r = { .txid = txid, .status = ERR_NOT_SUPPORTED };
    handle_t h = HANDLE_INVALID;
    if (c->lv == LEVEL_QUERY)
        r.status = add(LEVEL_QUERY, NO_DEVICE, &h);
    if (jam_channel_write(c->h, &r, sizeof(r), &h, r.status == OK ? 1 : 0) != OK && h)
        jam_handle_close(h);   /* the caller is gone: so is the new channel's only client */
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
            .h = ch, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)in,
            .handles_cap = 4, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_BUFFER_TOO_SMALL) {
            discard(ch, n, nh);   /* nothing of ours is that big */
            continue;
        }
        if (st != OK)
            return st;
        /* Only SET_CONSOLE and TEST_DISK carry a handle (one), and only on
         * control. */
        const struct devmgr_req *q = (const struct devmgr_req *)buf;
        bool denied = n >= 8 && !allowed(c, q->ordinal);
        bool whole = !denied && n == sizeof(*q);
        bool takes_handle = whole && nh == 1 && (q->ordinal == DEVMGR_SET_CONSOLE ||
                                                 q->ordinal == DEVMGR_TEST_DISK);
        if (!takes_handle)
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(in[i]);
        if (n < 4)
            continue;   /* no txid: nobody to answer */
        if (n == sizeof(struct svc_connect_req) && !nh && q->ordinal == SVC_CONNECT) {
            on_connect(c, q->txid);
            continue;
        }
        if (whole && !nh && q->ordinal == DEVMGR_MOUNTS) {
            mounts_request(ch, q->txid, q->instance);   /* answered now or later */
            continue;
        }
        struct devmgr_rep r = { .txid = q->txid, .status = ERR_INVALID_ARGS };
        handle_t hs[DEVMGR_MAX_HANDLES];
        rights_t rs[DEVMGR_MAX_HANDLES];
        uint32_t nout = 0;
        if (denied) {
            r.status = ERR_ACCESS_DENIED;
        } else if (takes_handle && q->ordinal == DEVMGR_SET_CONSOLE) {
            usb_new_console(in[0]);
            r.status = OK;
        } else if (takes_handle) {
            uint32_t id = 0;
            r.status = disk_test(in[0], &id);
            r.a = id;
        } else if (whole && !nh) {
            struct request_from from = { c->lv, c->lv == LEVEL_DEVICE ? c->dev : NO_DEVICE };
            request_handle(q, &from, &r, hs, rs, &nout);
        }
        uint32_t rn = r.status == OK ? sizeof(r) : DEVMGR_REP_HDR;
        if (jam_channel_write_rights(ch, &r, rn, hs, rs, nout) != OK)
            for (uint32_t i = 0; i < nout; i++)
                jam_handle_close(hs[i]);   /* the client is gone */
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
    chans[SLOT_LIFE] = (struct chan){ .h = startup_handle(SR_DEVMGR_CTL), .lv = LEVEL_CONTROL };
    chans[1] = (struct chan){ .h = startup_handle(SR_DEVMGR), .lv = LEVEL_QUERY };
    chans[2] = (struct chan){ .h = startup_handle(DEVMGR_SR_ESP), .lv = LEVEL_ESP };
    if (!chans[SLOT_LIFE].h) {   /* no control channel: it lives by the query channel */
        chans[SLOT_LIFE] = chans[1];
        chans[1] = (struct chan){ 0 };
    }
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
 * meanwhile); a driver's death arrives on the same port, and a due
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

bool chans_device_owned(uint32_t dev)
{
    for (unsigned i = 0; i < MAX_CHANS; i++)
        if (chans[i].h && chans[i].lv == LEVEL_DEVICE && chans[i].dev == dev)
            return true;
    return false;
}

/* A new channel of level lv (LEVEL_DEVICE: scoped to device dev) in a free
 * slot, watched by the next chans_arm; *out: its client end. */
static status_t add(enum level lv, uint32_t dev, handle_t *out)
{
    for (unsigned i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &chans[i];
        if (c->h || i == SLOT_LIFE)
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            return st;
        c->h = mine;
        c->lv = lv;
        c->dev = dev;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

status_t chans_new_device(uint32_t dev, handle_t *out)
{
    if (chans_device_owned(dev))
        return ERR_BAD_STATE;   /* one holder of a device's channel at a time */
    return add(LEVEL_DEVICE, dev, out);
}
