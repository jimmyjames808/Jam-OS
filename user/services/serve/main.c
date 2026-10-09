/* serve: the background file server (bin/serve). init starts it in shell
 * mode, like the music player, with
 *   SERVE_SR_CTL   the server end of the `serve` channel (abi/idl/serve.idl);
 *                 init keeps both ends, so a restarted server serves the
 *                 same channel, which init publishes as /svc/serve
 *   SR_NS         /svc/net-listen only: the network, with the permission
 *                 to listen (its list below says `svc net listen`; init
 *                 grants the same, shell.c's serve_grants)
 * It holds no mount: the shell opens each file and hands it over (serve.idl's
 * share), so the server can read that file and nothing else, and serves
 * it to every path asked (no directories, no path from a request is ever
 * looked up). It runs in a job of its own under init's, so it serves on
 * while the shell runs other commands and across a restart of the shell;
 * `serve stop` (or `kill serve`) stops it.
 *
 * serve.h has the model: one thread and one wait set for everything, and
 * the listen worker. This file: the loop, the control channels (`share`,
 * `info`, `stop`) and the log. Each request is one line in the log
 * ("serve: 10.2.21.174:41000 GET /big.bin 200, 52428800 bytes"), at most
 * LOG_BURST lines in LOG_WINDOW: the rest are counted and said once the
 * window ends. */
#include <stdarg.h>
#include <idl/serve.h>
#include <wants.h>
#include "serve.h"

/* What it wants (<wants.h>): the network, with the listen permission on
 * every port, the system's below 1024 too (`serve <file> 80`). init starts
 * it with exactly this (its grants in init's shell.c). */
JAM_WANTS("svc net listen low\n");

#define BATCH      32u
#define BUDGET     16u                   /* requests from one control channel a turn */
#define LOG_BURST  20u
#define LOG_WINDOW (10 * NS_PER_S)

struct netwait *serve_w;
struct share shares[SERVE_SHARES];
static handle_t ctl;                     /* init's shared channel (SERVE_SR_CTL) */
static uint32_t ctl_id;                  /* ... its entry in the wait set */
static bool     ctl_held;                /* ... held back by the unread bound */
static handle_t ctls[CTL_MAX];           /* our ends of the channels openers got (0: free) */
static uint32_t ctl_ids[CTL_MAX];
static bool     ctls_held[CTL_MAX];      /* held back by the unread bound (behind) */
static struct idl_held held;             /* how many are held, and when to look again */

void *serve_key(enum tag t, unsigned slot)
{
    return (void *)(uintptr_t)((uintptr_t)slot << 8 | (uintptr_t)t);
}

static enum tag key_tag(void *u)
{
    return (enum tag)((uintptr_t)u & 0xff);
}

static unsigned key_slot(void *u)
{
    return (unsigned)((uintptr_t)u >> 8);
}

void serve_log(const char *fmt, ...)
{
    static uint64_t window;
    static unsigned lines, dropped;
    uint64_t t = now();
    if (t - window >= LOG_WINDOW) {
        if (dropped)
            printf("serve: %u more lines not logged (at most %u in %u s)\n", dropped, LOG_BURST,
                   (unsigned)(LOG_WINDOW / NS_PER_S));
        window = t;
        lines = dropped = 0;
    }
    if (++lines > LOG_BURST) {
        dropped++;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- the control channels ------------------------------------------------------------ */

static status_t on_share(void *ctx, struct idl_txn txn, uint16_t port, const char *name,
                         uint32_t name_len, uint64_t size, handle_t file, handle_t buffer,
                         uint16_t *out_port)
{
    (void)out_port;   /* answered later, by share_listened */
    status_t st = OK;
    if (!ctx)
        st = ERR_NOT_SUPPORTED;   /* the shared channel: no call answered later there */
    else if (port < SERVE_PORT_MIN)
        st = ERR_OUT_OF_RANGE;
    else if (!name_len)
        st = ERR_INVALID_ARGS;
    int free_slot = -1;
    for (unsigned i = 0; st == OK && i < SERVE_SHARES; i++) {
        if (shares[i].state != S_FREE && shares[i].port == port)
            st = ERR_ALREADY_EXISTS;
        if (shares[i].state == S_FREE && free_slot < 0)
            free_slot = (int)i;
    }
    if (st == OK && free_slot < 0)
        st = ERR_NO_RESOURCES;
    if (st != OK) {   /* the handles are ours to close */
        jam_handle_close(file);
        jam_handle_close(buffer);
        return st;
    }
    return share_start((unsigned)free_slot, txn, port, name, name_len, size, file, buffer);
}

static status_t on_info(void *ctx, uint32_t index, uint16_t *port, char *name,
                        uint32_t *name_len, uint64_t *size, uint32_t *nclients, uint64_t *requests, uint64_t *bytes)
{
    (void)ctx;
    for (unsigned i = 0; i < SERVE_SHARES; i++) {
        const struct share *sh = &shares[i];
        if (sh->state != S_SERVING || index--)
            continue;
        *port = sh->port;
        *name_len = (uint32_t)strnlen(sh->name, SERVE_NAME_MAX - 1);
        memcpy(name, sh->name, *name_len);
        *size = sh->size;
        *nclients = sh->clients;
        *requests = sh->requests;
        *bytes = sh->bytes;
        return OK;
    }
    return ERR_NOT_FOUND;
}

static status_t on_stop(void *ctx, uint16_t port, uint32_t *stopped)
{
    (void)ctx;
    *stopped = 0;
    for (unsigned i = 0; i < SERVE_SHARES; i++) {
        if (shares[i].state != S_SERVING || (port && shares[i].port != port))
            continue;
        share_stop(i, "stopped (serve stop)");
        ++*stopped;
    }
    return OK;
}

static const struct serve_ops ops = { .share = on_share, .info = on_info, .stop = on_stop };

static uint32_t dispatch(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                         uint32_t *rhn)
{
    return serve_dispatch(&ops, ctx, req, n, rep, rhs, rhn);
}

/* svc.connect: a channel of its own for one opener, in the wait set. */
static status_t on_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    for (unsigned i = 0; i < CTL_MAX; i++) {
        if (ctls[i])
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        struct netwait_handle h = { mine, SIG_READABLE, 0, SIG_PEER_CLOSED };
        if (st == OK)
            st = netwait_add_handle(serve_w, &h, NETWAIT_READ, serve_key(T_CTL, i), &ctl_ids[i]);
        if (st != OK) {
            if (mine) {
                jam_handle_close(mine);
                jam_handle_close(theirs);
            }
            return st;
        }
        ctls[i] = mine;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

/* The unread bound (tools/genidl.py): has ch's client left serve's bound
 * of answers unread? Then its entry (id) waits for its hang-up only
 * (interest 0) and *flag is set, until a look finds them read (look_at).
 * Its requests wait in the channel meanwhile, charged to the client. */
static bool behind(handle_t ch, uint32_t id, bool *flag)
{
    if (!serve_unread_full(ch) || netwait_modify(serve_w, id, 0) != OK)
        return false;
    *flag = true;
    idl_hold(&held);
    return true;
}

/* Is a channel held back (flag) still behind? Else ready again. */
static void look_at(handle_t ch, uint32_t id, bool *flag)
{
    if (!*flag)
        return;
    if (ch && serve_unread_full(ch))
        idl_hold(&held);   /* still behind */
    else if (!ch || netwait_modify(serve_w, id, NETWAIT_READ) == OK)
        *flag = false;
    else
        idl_hold(&held);   /* try again next look */
}

/* Up to BUDGET requests from ch: ERR_SHOULD_WAIT once it is empty (or the
 * budget is spent: the entry stays ready, level-triggered; or its client
 * is behind: behind), else the read's status (ERR_PEER_CLOSED: its opener
 * is gone). The shared channel (svc.connect, and the methods without
 * handles: dispatch) through svc_serve_request; an opener's own with the
 * generated server, which takes `share`'s handles and its answer later. */
static status_t serve_some(handle_t ch)
{
    status_t st = OK;
    for (unsigned n = 0; n < BUDGET && st == OK && !behind(ch, ctl_id, &ctl_held); n++)
        st = svc_serve_request(ch, dispatch, on_connect, NULL);
    return st == OK ? ERR_SHOULD_WAIT : st;
}

static status_t serve_own(unsigned slot)
{
    status_t st = OK;
    for (unsigned n = 0; n < BUDGET && st == OK &&
                         !behind(ctls[slot], ctl_ids[slot], &ctls_held[slot]); n++)
        st = serve_serve_one(ctls[slot], &ops, (void *)1);   /* ctx non-NULL: an opener's */
    return st == OK ? ERR_SHOULD_WAIT : st;
}

static void ctl_ready(unsigned slot)
{
    if (serve_own(slot) == ERR_SHOULD_WAIT)
        return;
    (void)netwait_remove(serve_w, ctl_ids[slot]);   /* in the set since on_connect */
    idl_give_up(ctls[slot]);   /* gone, or its channel broke: what it left unread taken back */
    ctls[slot] = HANDLE_INVALID;
    ctls_held[slot] = false;
}

/* ---- the loop -------------------------------------------------------------------------- */

/* One ready entry. False: the shared channel is gone (init gave up on us). */
static bool ready(const struct netwait_ready *r)
{
    unsigned slot = key_slot(r->user);
    switch (key_tag(r->user)) {
    case T_SHARED:
        return serve_some(ctl) == ERR_SHOULD_WAIT;
    case T_CTL:
        ctl_ready(slot);
        break;
    case T_LISTENER:
        if (r->ready & NETWAIT_HUP)
            share_stop(slot, "netstack went away");
        else
            share_accept(slot);
        break;
    case T_FILE:
        share_file_ready(slot);
        break;
    case T_CLIENT:
        client_ready(&clients[slot], r);
        break;
    }
    return true;
}

static status_t setup(void)
{
    ctl = startup_handle(SERVE_SR_CTL);
    if (!ctl)
        return ERR_NOT_FOUND;
    struct netwait_handle h = { ctl, SIG_READABLE, 0, SIG_PEER_CLOSED };
    status_t st = netwait_create(NETWAIT_MAX, &serve_w);
    if (st == OK)
        st = netwait_add_handle(serve_w, &h, NETWAIT_READ, serve_key(T_SHARED, 0), &ctl_id);
    if (st == OK)
        st = listen_init();
    return st;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    status_t st = setup();
    if (st != OK) {
        printf("serve: can't start (%s): nothing to serve\n", status_str(st));
        return 1;
    }
    printf("serve: ready (`serve <file> [port]` in the shell)\n");
    for (;;) {   /* each turn: what is ready, the listen worker's answers, the deadlines */
        struct netwait_ready r[BATCH];
        uint32_t n = 0;
        if (idl_held_due(&held)) {   /* channels held back by the unread bound */
            look_at(ctl, ctl_id, &ctl_held);
            for (unsigned i = 0; i < CTL_MAX; i++)
                look_at(ctls[i], ctl_ids[i], &ctls_held[i]);
        }
        uint64_t deadline = idl_held_deadline(&held, clients_expire(now()));
        st = netwait_wait(serve_w, deadline, r, BATCH, &n);
        if (st != OK && st != ERR_TIMED_OUT && st != ERR_CANCELED) {
            printf("serve: waiting: %s: ending\n", status_str(st));
            return 1;
        }
        for (uint32_t i = 0; st == OK && i < n; i++) {
            if (!ready(&r[i])) {
                printf("serve: its channel from init is gone: ending\n");
                return 1;
            }
        }
        for (unsigned i = 0; i < SERVE_SHARES; i++)
            if (shares[i].state == S_LISTENING && __atomic_load_n(&shares[i].done, __ATOMIC_ACQUIRE))
                share_listened(i);
    }
}
