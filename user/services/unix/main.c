/* unix: the named AF_UNIX socket broker (bin/unix), /svc/unix
 * (abi/idl/unix.idl has the protocol and the model; docs/CR-PLAN.md
 * section 4, track CU2). init starts it with
 *   UNIX_SR_CTL   the server end of the shared channel init publishes as
 *                 /svc/unix (a channel per opener, svc.connect); in shell
 *                 mode init keeps both ends, so a restarted broker serves
 *                 the same channel
 * and nothing else: no namespace, no root. Openers are made on the shared
 * channel only; a program given /svc/unix by name holds one of its own,
 * which its starter opened for it (unix.idl, "Counted per program"). It
 * never reads a file: a socket file is libc's (user/posix/sys/unixsock.c),
 * which writes the key bind answers into it and reads it back to connect,
 * so the caller's own namespace and views decide which socket files it may
 * reach.
 *
 * One thread, one wait set (<netwait.h>): the shared channel, each opener's
 * channel and each listener's. A listener's waiting connections are the
 * `conn` events it hasn't read (handle_info's peer_queued), so the broker
 * holds nothing per connection: each is a channel pair whose ends are the
 * connector's and the listener's from the start (both CHANNEL_BYTE_BOUND,
 * as socketpair's), and what waits at the listener is charged to the
 * broker's job only until accept reads it: at most BACKLOG_MAX a listener.
 *
 * Exits when init's end of the shared channel is gone (the init run's end;
 * in shell mode init holds it for the boot). */
#include <idl/svc.h>
#include <idl/unix.h>
#include <netwait.h>
#include <os.h>
#include <roles.h>

#define BATCH  32u
#define BUDGET 16u   /* requests from one channel a turn */

enum tag { T_SHARED, T_OPENER, T_LISTENER };

/* An opener's slot is taken while its channel is open or any listener
 * bound through it lives: a program can't free its listeners' count by
 * closing its channel (it couldn't open another: what its starter gave it
 * is its only one, unix.idl). */
struct opener {
    handle_t ch;          /* our end (0: closed) */
    uint32_t id;          /* its wait set entry */
    bool     held;        /* held back by the unread bound */
    uint32_t listeners;   /* listeners bound through it, still alive */
};

struct listener {
    handle_t ch;          /* our end of its channel (0: free) */
    uint32_t id;
    bool     held;
    bool     listening;   /* `listen` called: connects reach it */
    uint16_t backlog;
    int      opener;      /* the opener slot it counts against, until it closes */
    uint32_t name_len;    /* an abstract name's bytes; 0: a socket file's (key) */
    uint8_t  name[UNIX_NAME_MAX];
    uint8_t  key[UNIX_KEY_BYTES];
};

static struct netwait  *w;
static handle_t         shared;
static uint32_t         shared_id;
static bool             shared_held;
static struct opener    openers[UNIX_OPENERS_MAX];
static struct listener  listeners[UNIX_LISTENERS_MAX];
static struct idl_held  held;

static void *key_of(enum tag t, unsigned slot)
{
    return (void *)(uintptr_t)((uintptr_t)slot << 8 | (uintptr_t)t);
}

static enum tag tag_of(void *u)
{
    return (enum tag)((uintptr_t)u & 0xff);
}

static unsigned slot_of(void *u)
{
    return (unsigned)((uintptr_t)u >> 8);
}

/* A channel's wait set entry, ch in it with key. */
static status_t watch(handle_t ch, void *key, uint32_t *id)
{
    struct netwait_handle h = { ch, SIG_READABLE, 0, SIG_PEER_CLOSED };
    return netwait_add_handle(w, &h, NETWAIT_READ, key, id);
}

/* ---- the listeners ------------------------------------------------------------------- */

static void listener_free(unsigned i)
{
    struct listener *l = &listeners[i];
    (void)netwait_remove(w, l->id);
    jam_handle_close(l->ch);   /* its waiting connections went with its client's end */
    if (openers[l->opener].listeners)
        openers[l->opener].listeners--;   /* the opener's slot is free once both are gone */
    memset(l, 0, sizeof(*l));
}

/* Two keys alike, in a time that doesn't depend on where they differ. */
static bool key_eq(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (unsigned i = 0; i < UNIX_KEY_BYTES; i++)
        d |= a[i] ^ b[i];
    return !d;
}

static bool key_zero(const uint8_t *k)
{
    static const uint8_t zero[UNIX_KEY_BYTES];
    return key_eq(k, zero);
}

/* The live listener with that abstract name (name_len > 0) or key; -1. */
static int find(const uint8_t *name, uint32_t name_len, const uint8_t *key)
{
    for (unsigned i = 0; i < UNIX_LISTENERS_MAX; i++) {
        const struct listener *l = &listeners[i];
        if (!l->ch || l->name_len != name_len)
            continue;
        if (name_len ? !memcmp(l->name, name, name_len) : key_eq(l->key, key))
            return (int)i;
    }
    return -1;
}

static status_t on_bind(void *ctx, const uint8_t *name, uint32_t name_len,
                        handle_t *out_listener, uint8_t out_key[16])
{
    if (tag_of(ctx) != T_OPENER)
        return ERR_NOT_SUPPORTED;
    unsigned o = slot_of(ctx);
    if (openers[o].listeners >= UNIX_LISTENERS_PER_OPENER)
        return ERR_NO_RESOURCES;
    if (name_len && find(name, name_len, NULL) >= 0)
        return ERR_ALREADY_EXISTS;
    int free_slot = -1;
    for (unsigned i = 0; i < UNIX_LISTENERS_MAX && free_slot < 0; i++)
        if (!listeners[i].ch)
            free_slot = (int)i;
    if (free_slot < 0)
        return ERR_NO_RESOURCES;
    struct listener *l = &listeners[free_slot];
    uint8_t key[UNIX_KEY_BYTES] = { 0 };
    while (!name_len && (key_zero(key) || find(NULL, 0, key) >= 0))
        os_random(key, sizeof(key));   /* 128 random bits: once, all but surely */
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = watch(mine, key_of(T_LISTENER, (unsigned)free_slot), &l->id);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    l->ch = mine;
    l->opener = (int)o;
    l->name_len = name_len;
    memcpy(l->name, name, name_len);
    memcpy(l->key, key, sizeof(key));
    openers[o].listeners++;
    *out_listener = theirs;
    memcpy(out_key, key, sizeof(key));
    return OK;
}

static status_t on_connect(void *ctx, const uint8_t *name, uint32_t name_len,
                           const uint8_t key[16], handle_t *out_conn)
{
    if (tag_of(ctx) != T_OPENER)
        return ERR_NOT_SUPPORTED;
    if (!name_len && key_zero(key))
        return ERR_NOT_FOUND;   /* no socket file's key is zero */
    int i = find(name, name_len, key);
    if (i < 0 || !listeners[i].listening)
        return ERR_NOT_FOUND;
    struct listener *l = &listeners[i];
    struct handle_info hi;
    status_t st = jam_handle_info(l->ch, &hi);
    if (st != OK)
        return st;
    if (hi.peer_queued >= l->backlog)
        return ERR_SHOULD_WAIT;   /* its backlog waits for accept */
    handle_t a, b;
    if ((st = jam_channel_create(&a, &b)) != OK)
        return st;
    /* Both ends bounded, as a socketpair's (user/posix/sys/pipe.c), while
     * each is still its only handle. */
    if ((st = jam_channel_set_flags(a, CHANNEL_BYTE_BOUND)) != OK ||
        (st = jam_channel_set_flags(b, CHANNEL_BYTE_BOUND)) != OK) {
        jam_handle_close(a);
        jam_handle_close(b);
        return st;
    }
    st = unix_conn(l->ch, b);   /* b is gone whatever happens */
    if (st != OK) {
        jam_handle_close(a);
        if (st == ERR_PEER_CLOSED) {
            listener_free((unsigned)i);   /* its client went: its hang-up is on the way */
            return ERR_NOT_FOUND;
        }
        return st == ERR_SHOULD_WAIT || st == ERR_NO_MEMORY ? ERR_SHOULD_WAIT : st;
    }
    *out_conn = a;
    return OK;
}

static status_t on_listen(void *ctx, uint16_t backlog)
{
    if (tag_of(ctx) != T_LISTENER)
        return ERR_NOT_SUPPORTED;
    struct listener *l = &listeners[slot_of(ctx)];
    l->listening = true;
    l->backlog = backlog < 1 ? 1 : backlog > UNIX_BACKLOG_MAX ? UNIX_BACKLOG_MAX : backlog;
    return OK;
}

static const struct unix_ops ops = { .bind = on_bind, .connect = on_connect, .listen = on_listen };
static const struct unix_ops no_ops;   /* the shared channel: svc.connect only */

/* ---- the openers --------------------------------------------------------------------- */

static uint32_t shared_dispatch(void *ctx, const void *req, uint32_t n, void *rep,
                                handle_t *rhs, uint32_t *rhn)
{
    return unix_dispatch(&no_ops, ctx, req, n, rep, rhs, rhn);
}

/* svc.connect: an opener's own channel, in the wait set. */
static status_t on_svc_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    for (unsigned i = 0; i < UNIX_OPENERS_MAX; i++) {
        struct opener *o = &openers[i];
        if (o->ch || o->listeners)
            continue;   /* open, or its listeners still count against it */
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st == OK && (st = watch(mine, key_of(T_OPENER, i), &o->id)) != OK) {
            jam_handle_close(mine);
            jam_handle_close(theirs);
        }
        if (st != OK)
            return st;
        o->ch = mine;
        o->held = false;
        o->listeners = 0;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

/* Its channel closed; its listeners live on, counted against it. */
static void opener_free(unsigned i)
{
    struct opener *o = &openers[i];
    (void)netwait_remove(w, o->id);
    idl_give_up(o->ch);   /* gone, or broke the protocol: what it left unread taken back */
    o->ch = HANDLE_INVALID;
    o->id = 0;
    o->held = false;
}

/* ---- the unread bound (tools/genidl.py) ------------------------------------------------ */

/* Has ch's client left our bound of answers unread? Then its entry (id)
 * waits for its hang-up only and *flag is set until a look finds it read. */
static bool behind(handle_t ch, uint32_t id, bool *flag)
{
    if (!unix_unread_full(ch) || netwait_modify(w, id, 0) != OK)
        return false;
    *flag = true;
    idl_hold(&held);
    return true;
}

static void look_at(handle_t ch, uint32_t id, bool *flag)
{
    if (!*flag)
        return;
    if (ch && unix_unread_full(ch))
        idl_hold(&held);
    else if (!ch || netwait_modify(w, id, NETWAIT_READ) == OK)
        *flag = false;
    else
        idl_hold(&held);
}

/* Up to BUDGET requests from ch (ctx: whose): ERR_SHOULD_WAIT once it is
 * empty, its budget spent or its client behind, else the read's status. */
static status_t serve_some(handle_t ch, uint32_t id, bool *flag, void *ctx)
{
    status_t st = OK;
    for (unsigned n = 0; n < BUDGET && st == OK && !behind(ch, id, flag); n++)
        st = ctx ? unix_serve_one(ch, &ops, ctx)
                 : svc_serve_request(ch, shared_dispatch, on_svc_connect, NULL);
    return st == OK ? ERR_SHOULD_WAIT : st;
}

/* ---- the loop ------------------------------------------------------------------------ */

/* One ready entry. False: the shared channel is gone. */
static bool ready(const struct netwait_ready *r)
{
    unsigned i = slot_of(r->user);
    if ((tag_of(r->user) == T_OPENER && !openers[i].ch) ||
        (tag_of(r->user) == T_LISTENER && !listeners[i].ch))
        return true;   /* freed earlier in this batch */
    switch (tag_of(r->user)) {
    case T_SHARED:
        return serve_some(shared, shared_id, &shared_held, NULL) == ERR_SHOULD_WAIT;
    case T_OPENER:
        if (serve_some(openers[i].ch, openers[i].id, &openers[i].held, r->user) != ERR_SHOULD_WAIT)
            opener_free(i);
        break;
    case T_LISTENER:
        if (serve_some(listeners[i].ch, listeners[i].id, &listeners[i].held, r->user) !=
            ERR_SHOULD_WAIT)
            listener_free(i);
        break;
    }
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shared = startup_handle(UNIX_SR_CTL);
    status_t st = shared ? netwait_create(NETWAIT_MAX, &w) : ERR_NOT_FOUND;
    if (st == OK)
        st = watch(shared, key_of(T_SHARED, 0), &shared_id);
    if (st != OK) {
        printf("unix: can't start (%s): no named sockets\n", status_str(st));
        return 1;
    }
    for (;;) {
        struct netwait_ready r[BATCH];
        uint32_t n = 0;
        if (idl_held_due(&held)) {
            look_at(shared, shared_id, &shared_held);
            for (unsigned i = 0; i < UNIX_OPENERS_MAX; i++)
                look_at(openers[i].ch, openers[i].id, &openers[i].held);
            for (unsigned i = 0; i < UNIX_LISTENERS_MAX; i++)
                look_at(listeners[i].ch, listeners[i].id, &listeners[i].held);
        }
        st = netwait_wait(w, idl_held_deadline(&held, DEADLINE_NEVER), r, BATCH, &n);
        if (st != OK && st != ERR_TIMED_OUT && st != ERR_CANCELED) {
            printf("unix: waiting: %s: ending\n", status_str(st));
            return 1;
        }
        for (uint32_t k = 0; st == OK && k < n; k++)
            if (!ready(&r[k]))
                return 0;   /* init's end is gone: the run is over */
    }
}
