/* init's calls to devmgr that don't wait (M12, track D). init's loop must
 * never stand inside a call: devmgr's first binding pass takes as long as
 * usb-bus needs, a kill waits for a driver to die, and an answer that is
 * late holds up every other request init has. So `status` (once devmgr is
 * started), `set_input`, `set_volumes` and the kill of a driver
 * (devmgrctl.kill_process, for initctl.kill) are sent with
 * devmgr_<method>_send / devmgrctl_<method>_send on init's own control
 * channel to devmgr (shell_devmgr: no other program reads it, the
 * namespace's copies are only for svc.connect), and answered later: the
 * channel is bound to the loop's port (KEY_DEVMGR, one ONCE binding while a
 * call waits), dm_event reads the answers, and a call that is not answered
 * in time ends with ERR_TIMED_OUT (dm_due). The caller's `done` runs then,
 * with ERR_PEER_CLOSED if devmgr ended meanwhile (dm_gone).
 *
 * The test mode of init (main.c) has no loop and waits, as it always did:
 * dm_set_volumes calls and waits when dm_init was never called. */
#include <idl/devmgr.h>
#include <os.h>
#include "init.h"

#define MAX_CALLS 8
/* The biggest answer of the two protocols. */
#define REP_MAX (DEVMGR_REP_MAX > DEVMGRCTL_REP_MAX ? DEVMGR_REP_MAX : DEVMGRCTL_REP_MAX)

enum kind {
    K_STATUS,
    K_SET_INPUT,
    K_SET_VOLUMES,
    K_KILL_PROCESS,
};

struct call {
    bool       used;
    enum kind  kind;
    uint32_t   txid;
    uint64_t   deadline;
    handle_t   ch;       /* the channel it went on (init's end of devmgr's control channel) */
    dm_done_fn done;     /* NULL: only logged */
    void      *arg;
};

static struct call calls[MAX_CALLS];
static uint32_t    last_txid;
static handle_t    loop_port;

void dm_init(handle_t port)
{
    loop_port = port;
}

/* Watch ch for the answers: one ONCE binding (an older one replaced), which
 * fires at once if an answer is already there. */
static status_t watch(handle_t ch)
{
    (void)jam_port_unbind(loop_port, ch, KEY_DEVMGR);   /* none: nothing to undo */
    return jam_port_bind(loop_port, ch, KEY_DEVMGR, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_ONCE);
}

/* What a call that nobody asked about says when it fails. */
static const char *what(enum kind k)
{
    return k == K_STATUS      ? "the first binding pass"
           : k == K_SET_INPUT ? "the new input channel"
           : k == K_SET_VOLUMES ? "the disks channel" : "the kill";
}

/* c is over with st (and the status counts, for K_STATUS's OK): its done
 * runs, or the failure is said. */
static void finish(struct call *c, status_t st, const struct dm_counts *counts)
{
    struct call over = *c;
    *c = (struct call){ 0 };
    if (over.done)
        over.done(over.arg, st, counts);
    else if (st != OK)
        printf("init: devmgr didn't take %s (%s)\n", what(over.kind), status_str(st));
}

static struct call *call_new(enum kind kind, handle_t ch, uint64_t wait, dm_done_fn done,
                             void *arg)
{
    for (unsigned i = 0; i < MAX_CALLS; i++) {
        struct call *c = &calls[i];
        if (c->used)
            continue;
        *c = (struct call){ .used = true, .kind = kind, .txid = idl_txid_next(&last_txid),
                            .deadline = now() + wait, .ch = ch, .done = done, .arg = arg };
        return c;
    }
    return NULL;
}

/* The call went (st) or not: pending (watched), or freed. */
static status_t sent(struct call *c, status_t st)
{
    if (st == OK)
        st = watch(c->ch);
    if (st != OK)
        *c = (struct call){ 0 };
    return st;
}

status_t dm_status(handle_t dm, dm_done_fn done, void *arg)
{
    struct call *c = call_new(K_STATUS, dm, 30 * NS_PER_S, done, arg);
    if (!c)
        return ERR_NO_RESOURCES;
    return sent(c, devmgr_status_send(dm, c->txid));
}

status_t dm_kill_process(handle_t dm, uint64_t koid, uint64_t wait, dm_done_fn done, void *arg)
{
    struct call *c = call_new(K_KILL_PROCESS, dm, wait, done, arg);
    if (!c)
        return ERR_NO_RESOURCES;
    return sent(c, devmgrctl_kill_process_send(dm, c->txid, koid));
}

/* set_input and set_volumes: the handle goes with the request whatever
 * happens (a send's handles are always moved). */
static void send_handle(enum kind kind, handle_t dm, handle_t h)
{
    struct call *c = loop_port ? call_new(kind, dm, 5 * NS_PER_S, NULL, NULL) : NULL;
    status_t st;
    if (c) {
        st = sent(c, kind == K_SET_INPUT ? devmgrctl_set_input_send(dm, c->txid, h)
                                         : devmgrctl_set_volumes_send(dm, c->txid, h));
    } else if (!loop_port) {   /* no loop (init's test mode): wait */
        st = kind == K_SET_INPUT ? devmgrctl_set_input_until(dm, now() + 5 * NS_PER_S, h)
                                 : devmgrctl_set_volumes_until(dm, now() + 5 * NS_PER_S, h);
    } else {
        jam_handle_close(h);
        st = ERR_NO_RESOURCES;
    }
    if (st != OK)
        printf("init: devmgr didn't take %s (%s)\n", what(kind), status_str(st));
}

void dm_set_input(handle_t dm, handle_t input)
{
    send_handle(K_SET_INPUT, dm, input);
}

void dm_set_volumes(handle_t dm, handle_t disks)
{
    send_handle(K_SET_VOLUMES, dm, disks);
}

/* One answer (rep, m) to c, decoded by what c asked. */
static void answer(struct call *c, const void *rep, struct idl_msg *m)
{
    struct dm_counts counts = { 0 };
    status_t st;
    switch (c->kind) {
    case K_STATUS:
        st = devmgr_status_result(rep, m, &counts.bound, &counts.failed, &counts.skipped);
        break;
    case K_SET_INPUT:
        st = devmgrctl_set_input_result(rep, m);
        break;
    case K_SET_VOLUMES:
        st = devmgrctl_set_volumes_result(rep, m);
        break;
    default:
        st = devmgrctl_kill_process_result(rep, m);
        break;
    }
    finish(c, st, st == OK && c->kind == K_STATUS ? &counts : NULL);
}

/* Everything c's channel has queued, matched to the calls waiting on it. */
static void poll(handle_t ch)
{
    for (unsigned guard = 0; guard < 64; guard++) {
        _Alignas(8) uint8_t rep[REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(ch, rep, sizeof(rep), &m);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st == ERR_PEER_CLOSED) {   /* devmgr ended */
            for (unsigned i = 0; i < MAX_CALLS; i++)
                if (calls[i].used && calls[i].ch == ch)
                    finish(&calls[i], ERR_PEER_CLOSED, NULL);
            return;
        }
        if (st != OK) {   /* a message that is not a reply of ours: gone */
            idl_msg_drop(&m);
            continue;
        }
        struct call *c = NULL;
        for (unsigned i = 0; i < MAX_CALLS && !c; i++)
            if (calls[i].used && calls[i].ch == ch && calls[i].txid == m.txid)
                c = &calls[i];
        if (c)
            answer(c, rep, &m);
        else
            idl_msg_drop(&m);   /* an answer that came after we gave up */
    }
}

/* Watch again every channel a call still waits on. */
static void rearm(void)
{
    for (unsigned i = 0; i < MAX_CALLS; i++) {
        if (!calls[i].used)
            continue;
        if (watch(calls[i].ch) != OK)
            finish(&calls[i], ERR_NO_RESOURCES, NULL);   /* nothing would see the answer */
    }
}

void dm_event(void)
{
    for (unsigned i = 0; i < MAX_CALLS; i++)
        if (calls[i].used)
            poll(calls[i].ch);   /* the same channel again finds it empty */
    rearm();
}

uint64_t dm_due(uint64_t t)
{
    uint64_t first = DEADLINE_NEVER;
    bool late = false;
    for (unsigned i = 0; i < MAX_CALLS; i++)
        if (calls[i].used && t >= calls[i].deadline)
            late = true;
    if (late) {
        /* The loop may have been busy past a deadline with the answer
         * already queued (its key not yet taken from the port): read what is
         * there before calling a call timed out. */
        dm_event();
        for (unsigned i = 0; i < MAX_CALLS; i++)
            if (calls[i].used && t >= calls[i].deadline)
                finish(&calls[i], ERR_TIMED_OUT, NULL);
    }
    for (unsigned i = 0; i < MAX_CALLS; i++)
        if (calls[i].used && calls[i].deadline < first)
            first = calls[i].deadline;
    return first;
}

void dm_gone(handle_t dm)
{
    for (unsigned i = 0; i < MAX_CALLS; i++)
        if (calls[i].used && calls[i].ch == dm)
            finish(&calls[i], ERR_PEER_CLOSED, NULL);
}
