/* devmgr: its channels. init gives it the server ends of a control channel
 * (SR_DEVMGR_CTL) and a query channel (SR_DEVMGR), and asks the control
 * channel for device channels (devmgrctl.device_channel), each scoped to
 * one device and made here. A query or a control channel also answers the
 * svc protocol's connect (abi/idl/svc.idl) with a new channel of its own
 * level for the caller, so each opener of /svc/devmgr and /svc/devmgr-ctl
 * has one and a reply that comes after its caller gave up waits on that
 * caller's channel, not a shared one. Every channel is watched on devmgr's
 * port, read here and answered by request.c's handlers (the generated
 * dispatch, <idl/devmgr.h>). What a channel may ask is decided by which
 * channel it is (its level, and a device channel's device), never by who
 * sent the request (abi/idl/devmgr.idl, the opening paragraphs).
 *
 * The table is bounded (MAX_CHANS): a device channel or a connect asked for
 * past it is ERR_NO_RESOURCES. A query, control or device channel whose
 * clients are all gone is dropped, and its slot is free again; devmgr lives
 * as long as the clients of slot 0 do (the control channel init started it
 * with, or without one the query channel). */
#include <idl/svc.h>
#include "internal.h"

/* The biggest request and reply of the protocols the channels speak. */
#define REQ_MAX (DEVMGR_REQ_MAX > DEVMGRCTL_REQ_MAX ? DEVMGR_REQ_MAX : DEVMGRCTL_REQ_MAX)
#define REP_MAX (DEVMGR_REP_MAX > DEVMGRCTL_REP_MAX ? DEVMGR_REP_MAX : DEVMGRCTL_REP_MAX)
_Static_assert(SVC_REQ_MAX <= REQ_MAX && SVC_REP_MAX <= REP_MAX, "a connect fits");

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

static status_t add(enum level lv, uint32_t dev, handle_t *out);

/* svc.connect on a channel: a channel of the caller's own, of the same
 * level (a query channel's opener gets a query channel, a control
 * channel's a control one). A device channel is one holder's:
 * ERR_NOT_SUPPORTED. */
static status_t s_connect(void *ctx, handle_t *out)
{
    const struct chan *c = ctx;
    if (c->lv == LEVEL_DEVICE)
        return ERR_NOT_SUPPORTED;
    return add(c->lv, NO_DEVICE, out);
}

static const struct svc_ops svc_ops = { .connect = s_connect };

/* One request q (n bytes, nh handles in hs, which go to the dispatch) on
 * c: the reply into r and rhs. Returns its length (0: none). What a
 * channel may ask is its level's: `devmgr` on every channel (a device
 * channel's own checks are request.c's), `devmgrctl` on the control channel
 * alone; on any other it is ERR_ACCESS_DENIED, with the handles closed. */
static uint32_t dispatch(const struct chan *c, const void *q, uint32_t n, const handle_t *hs,
                         uint32_t nh, void *r, handle_t *rhs, uint32_t *rhn)
{
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    struct request_from from = { c->lv, c->lv == LEVEL_DEVICE ? c->dev : NO_DEVICE };
    uint32_t id = n >= sizeof(*h) ? h->ordinal >> 16 : 0;
    *rhn = 0;
    if (id == DEVMGRCTL_PROTOCOL_ID) {
        if (c->lv != LEVEL_CONTROL) {
            idl_close_all(hs, nh);
            idl_reply_status(c->h, q, n, ERR_ACCESS_DENIED);
            return 0;
        }
        return devmgrctl_dispatch_h(c->h, &devmgrctl_ops, &from, q, n, hs, nh, r, rhs, rhn);
    }
    if (nh) {   /* the other protocols carry no handles */
        idl_close_all(hs, nh);
        idl_reply_status(c->h, q, n, ERR_INVALID_ARGS);
        return 0;
    }
    if (id == SVC_PROTOCOL_ID)
        return svc_dispatch_on(c->h, &svc_ops, (void *)c, q, n, r, rhs, rhn);
    return devmgr_dispatch_on(c->h, &devmgr_ops, &from, q, n, r, rhs, rhn);
}

/* Answer everything queued on c. Returns ERR_SHOULD_WAIT once the queue is
 * empty, ERR_PEER_CLOSED once every client is gone and nothing is left to
 * read. */
static status_t serve(const struct chan *c)
{
    handle_t ch = c->h;
    for (;;) {
        _Alignas(8) uint8_t q[REQ_MAX], r[REP_MAX];
        handle_t hs[DEVMGRCTL_REQ_HANDLES], rhs[DEVMGRCTL_REP_HANDLES];
        uint32_t n = 0, nh = 0, rhn = 0;
        status_t st = jam_channel_read(&(struct channel_read_args){
            .h = ch, .rd_cap = sizeof(q), .rd_bytes = (uint64_t)(uintptr_t)q,
            .rd_actual = (uint64_t)(uintptr_t)&n, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = DEVMGRCTL_REQ_HANDLES, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        });
        if (st == ERR_BUFFER_TOO_SMALL) {
            /* Nothing of ours is that big: taken off, its handles closed,
             * answered ERR_INVALID_ARGS (as a generated server does), so its
             * caller isn't left waiting for its deadline. */
            st = idl_drain(ch, n, nh);
            if (st == ERR_NO_MEMORY)
                return ERR_SHOULD_WAIT;   /* still queued: the channel's next turn */
            if (st != OK)
                return st;
            continue;
        }
        if (st != OK)
            return st;
        uint32_t rn = dispatch(c, q, n, hs, nh, r, rhs, &rhn);
        if (rn)
            (void)jam_channel_write(ch, r, rn, rhs, rhn);   /* the handles go either way */
        else
            idl_close_all(rhs, rhn);
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
