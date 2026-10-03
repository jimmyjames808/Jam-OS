/* init's side of a service that outlives its process (docs/M11.6-PLAN.md):
 * for the mixer, a keeper, a state VMO and a warm spare.
 *
 * **The keeper** (<keep.h>) holds a duplicate of each kernel object the
 * mixer hands it on its keep channel (SR_KEEP), so the object outlives the
 * process, and hands them all to the next instance. Each instance gets a
 * new keep channel; what the dead one wrote on the old one is still taken
 * first (keeper_attach). **The state VMO** (<svcstate.h>) is made once,
 * at the mixer's first start, and every instance gets a handle with
 * SVCSTATE_SERVICE_RIGHTS only (no resize, transfer or duplicate); init
 * never maps it. Both live until init gives up on the mixer (kept_given_up):
 * then the kept objects close, and their clients see ERR_PEER_CLOSED.
 *
 * **The warm spare** is a second bin/mixer started with nothing but
 * SR_STANDBY, which waits in libos before main, having opened and read
 * nothing (start.c). When shell.c starts the mixer again after its end,
 * the spare is promoted: one message with the startup handles a new
 * process would have been given, and the time of the kill; it runs main
 * as if just started, and a new spare is started at once in the
 * background (spare_due, after the loop's starts). Without a spare (the
 * boot word `nospare`, or none ready yet) a new process is started as
 * before. Either way the instance gets the state VMO and a new keep
 * channel, and only after it has its end does the keeper write its
 * restore into it: the kernel won't send a channel end whose queue holds
 * channel ends.
 *
 * A mixer that reads neither SR_STATE nor SR_KEEP (one that doesn't
 * adopt state yet) starts fresh as before and costs nothing more: the
 * state's pages are committed only by svcstate_open, and the keeper's
 * restore is one KEEP_DONE (it holds nothing) on an end that closes with
 * the process.
 *
 * **Measured, every restart:** when the new instance runs (promoted, or a
 * process started), after the kill (initctl.kill) or after its end was
 * seen (a crash); and, on a thread of its own that serves nothing (so it
 * may block), one cheap call on the shared audioctl channel init keeps
 * (audioctl.streams): kill to first answer, as a client feels it. The
 * spare's memory is its job's (the shell's `ps`).
 *
 * Everything here runs on init's loop thread but the probe, which touches
 * only `probe` (its busy flag released when it is done). */
#include <idl/audioctl.h>
#include <keep.h>
#include <os.h>
#include <svcstate.h>
#include "init.h"

/* The mixer's state VMO. The mixer's own layout (tens of KiB: two request
 * slots and its numbers) is the mixer's to set; svcstate_open refuses a
 * VMO smaller than the layout, so this is made with room to spare, which
 * costs nothing: pages are committed only as they are used. */
#define MIXER_STATE_SIZE (1ull << 20)

#define SPARE_FIRST  (100 * NS_PER_MS)   /* the wait before a spare that ended is replaced */
#define SPARE_MAX    (5 * NS_PER_S)      /* ... doubling up to this */
#define SPARE_STEADY (10 * NS_PER_S)     /* a spare that waited this long ended by no fault of
                                          * its start: the wait starts again from SPARE_FIRST */
#define PROBE_WAIT   (5 * NS_PER_S)      /* how long the probe waits for the first answer */
/* After a promotion the next spare starts this much later, not at once:
 * starting a process takes CPU time the promoted instance needs while it
 * sets itself up (measured in QEMU: kill to first answer 3.4 ms with the
 * new spare started at once, against 2.0 ms for a plain process start). */
#define SPARE_REFILL (50 * NS_PER_MS)

/* What init keeps of a service that outlives its process. */
struct kept {
    unsigned      svc;          /* which service (shell.c's index) */
    uint64_t      state_size;   /* bytes of its state VMO */
    handle_t      state;        /* the state VMO, every right, never mapped; 0: not made */
    struct keeper keeper;       /* its keep channel and what it kept */
    bool          bound;        /* keeper.ch is bound on the loop's port (KEY_KEEP) */
};

/* The warm spare (one, for the mixer). */
struct spare {
    handle_t proc, job;   /* while one waits; 0: none */
    handle_t standby;     /* our end of its SR_STANDBY */
    uint64_t started;     /* uptime ns */
    uint64_t next_try;    /* the next may start then (uptime ns) */
    uint64_t backoff;     /* the last wait after a spare ended by itself, ns */
};

/* The first-answer probe: a thread's, from probe_start until busy clears. */
struct probe {
    bool        busy;       /* a probe runs (atomic: released by the thread when done) */
    handle_t    thread;     /* the last probe's thread (closed before the next) */
    handle_t    ch;         /* a duplicate of the shared audioctl channel, the probe's */
    uint64_t    from;       /* the kill, or the end seen (uptime ns) */
    bool        killed;     /* from is a deliberate kill */
    bool        promoted;   /* the instance asked was a promoted spare */
    const char *path;       /* the service's */
};

static handle_t port;
static bool spares_on;
static struct kept mixer = { .svc = MIXER, .state_size = MIXER_STATE_SIZE };
static struct spare spare;
static struct probe probe;
static _Alignas(16) uint8_t probe_stack[16 << 10];

static struct kept *kept_of(unsigned i)
{
    return i == mixer.svc ? &mixer : NULL;
}

bool spare_kept(unsigned i)
{
    return kept_of(i) != NULL;
}

void spare_init(handle_t loop_port, bool spares)
{
    port = loop_port;
    spares_on = spares;
    keeper_init(&mixer.keeper);
    if (!spares)
        printf("init: nospare: no warm spare; a restart of %s starts a process\n",
               svcs[mixer.svc].path);
}

/* ---- the spare ----------------------------------------------------------------------- */

/* The spare goes (dismissed, or found dead): its standby channel closed
 * and its job killed. */
static void spare_drop(void)
{
    if (!spare.proc)
        return;
    (void)jam_port_unbind(port, spare.proc, KEY_SPARE);   /* ERR_NOT_FOUND: it fired */
    jam_handle_close(spare.standby);
    jam_job_kill(spare.job);
    jam_handle_close(spare.proc);
    jam_handle_close(spare.job);
    spare.proc = spare.job = spare.standby = HANDLE_INVALID;
}

/* Start a spare of svc i: SR_STANDBY its only handle. */
static status_t spare_start(unsigned i)
{
    handle_t mine, theirs, job, proc = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    struct spawn_handle x[] = { { SR_STANDBY, theirs } };
    const char *argv[] = { svcs[i].path };
    struct spawn_args a = { .path = svcs[i].path, .argc = 1, .argv = argv, .job = job,
                            .extra = x, .nextra = 1 };
    st = spawn(&a, &proc);   /* consumes theirs */
    if (st == OK)
        st = jam_port_bind(port, proc, KEY_SPARE, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st != OK) {
        jam_job_kill(job);
        if (proc)
            jam_handle_close(proc);
        jam_handle_close(job);
        jam_handle_close(mine);
        return st;
    }
    spare = (struct spare){ .proc = proc, .job = job, .standby = mine, .started = now(),
                            .backoff = spare.backoff };
    struct process_info info;
    if (jam_process_get_info(proc, &info) == OK)
        printf("init: a warm spare %s waits (process %lu)\n", svcs[i].path,
               (unsigned long)info.koid);
    return OK;
}

uint64_t spare_due(uint64_t t)
{
    struct svc *s = &svcs[mixer.svc];
    if (s->given_up)
        spare_drop();   /* shell_stop_devmgr gives the mixer up without a word to us */
    if (!spares_on || spare.proc || s->given_up || !s->started)
        return DEADLINE_NEVER;   /* off, one waits, or the mixer never started */
    if (t < spare.next_try)
        return spare.next_try;
    status_t st = spare_start(mixer.svc);
    if (st == OK)
        return DEADLINE_NEVER;
    spare.backoff = spare.backoff ? spare.backoff * 2 : SPARE_FIRST;
    if (spare.backoff > SPARE_MAX)
        spare.backoff = SPARE_MAX;
    spare.next_try = t + spare.backoff;
    printf("init: can't start a warm spare %s (%s)\n", s->path, status_str(st));
    return spare.next_try;
}

/* The spare's process has ended (by itself, or someone killed it): it is
 * replaced after a wait that grows if spares keep ending soon after their
 * start. */
static void spare_ended(uint64_t t)
{
    struct process_info info;
    bool got = jam_process_get_info(spare.proc, &info) == OK;
    spare.backoff = t - spare.started > SPARE_STEADY || !spare.backoff ? SPARE_FIRST
                                                                       : spare.backoff * 2;
    if (spare.backoff > SPARE_MAX)
        spare.backoff = SPARE_MAX;
    spare.next_try = t + spare.backoff;
    printf("init: the warm spare %s %s %ld: another in %lu ms\n", svcs[mixer.svc].path,
           got && info.killed ? "was killed, code" : "exited with code",
           got ? (long)info.exit_code : -1L, (unsigned long)(spare.backoff / NS_PER_MS));
    spare_drop();
}

void spare_event(void)
{
    signals_t seen;
    if (!spare.proc || jam_object_wait_one(spare.proc, SIG_TERMINATED, 0, &seen) != OK)
        return;   /* an earlier spare's packet: it was promoted or dismissed since */
    spare_ended(now());
}

/* Promote the spare to be svc i, with x[0..nx) (each sent with r[k]).
 * ERR_NOT_FOUND: no spare waits (x untouched). Else x is consumed; OK:
 * svc i runs as the spare's process now. */
static status_t promote(unsigned i, struct spawn_handle *x, const rights_t *r, unsigned nx)
{
    if (!spares_on || !spare.proc)
        return ERR_NOT_FOUND;
    signals_t seen;
    if (jam_object_wait_one(spare.proc, SIG_TERMINATED, 0, &seen) == OK) {
        spare_ended(now());   /* its packet may still be on its way */
        return ERR_NOT_FOUND;
    }
    struct svc *s = &svcs[i];
    const char *argv[] = { s->path };
    struct standby_args a = { .hs = x, .rights = r, .n = nx, .argc = 1, .argv = argv,
                              .envp = NULL, .kill_ns = s->kill_at ? s->kill_at : s->ended_at };
    handle_t proc = spare.proc, job = spare.job;
    (void)jam_port_unbind(port, proc, KEY_SPARE);   /* its end is svc i's from now on */
    status_t st = standby_promote(spare.standby, &a);
    jam_handle_close(spare.standby);   /* a promotion already queued is still read */
    spare = (struct spare){ .next_try = now() + SPARE_REFILL };
    if (st != OK) {
        printf("init: the warm spare %s couldn't be promoted (%s)\n", s->path, status_str(st));
        jam_job_kill(job);
        jam_handle_close(proc);
        jam_handle_close(job);
        return st;
    }
    return svc_adopt(i, proc, job);
}

/* ---- the keeper and the state ------------------------------------------------------ */

static void keeper_unbind(struct kept *k)
{
    if (k->bound)
        (void)jam_port_unbind(port, k->keeper.ch, KEY_KEEP);   /* ours: it was bound */
    k->bound = false;
}

/* Add the state VMO (made the first time) and a new keep channel to
 * x[0..nx) (r: their rights): the new count. Either one missing is said
 * and left out: the instance then starts fresh, as one without them. */
static unsigned kept_handles(struct kept *k, struct spawn_handle *x, rights_t *r, unsigned nx)
{
    const char *path = svcs[k->svc].path;
    status_t st = k->state ? OK : svcstate_create(k->state_size, &k->state);
    handle_t h = HANDLE_INVALID;
    if (st == OK)
        st = svcstate_give(k->state, &h);
    if (st == OK) {
        x[nx] = (struct spawn_handle){ SR_STATE, h };
        r[nx++] = SVCSTATE_SERVICE_RIGHTS;   /* the transfer right comes off in transit */
    } else {
        printf("init: %s starts without its state (%s)\n", path, status_str(st));
    }
    keeper_unbind(k);
    st = keeper_attach(&k->keeper, &h);   /* the dead instance's last puts are taken first */
    if (st == OK) {
        x[nx] = (struct spawn_handle){ SR_KEEP, h };
        r[nx++] = RIGHT_SAME;
    } else {
        printf("init: %s starts without a keep channel (%s)\n", path, status_str(st));
    }
    return nx;
}

/* The instance has its keep channel's end: hand it what was kept, then
 * listen to it. */
static void hand_over(struct kept *k)
{
    if (k->keeper.ch == HANDLE_INVALID)
        return;
    status_t st = keeper_restore(&k->keeper);
    if (st != OK && st != ERR_PEER_CLOSED) {   /* PEER_CLOSED: it died already; the loop sees */
        printf("init: %s can't be handed what was kept (%s): its clients' ends close\n",
               svcs[k->svc].path, status_str(st));
        keeper_release(&k->keeper);
        return;
    }
    st = jam_port_bind(port, k->keeper.ch, KEY_KEEP, SIG_READABLE, PORT_BIND_PERSISTENT);
    k->bound = st == OK;
    if (st != OK)   /* what it sends waits on the channel until its next start */
        printf("init: %s's keeper can't listen (%s)\n", svcs[k->svc].path, status_str(st));
}

void kept_event(void)
{
    if (mixer.bound)
        keeper_drain(&mixer.keeper);   /* a persistent binding fires again only on a new edge */
}

void kept_given_up(unsigned i)
{
    struct kept *k = kept_of(i);
    if (!k)
        return;
    spare_drop();
    keeper_unbind(k);
    keeper_release(&k->keeper);
    if (k->state)
        jam_handle_close(k->state);
    k->state = HANDLE_INVALID;
}

/* ---- the restart, measured ---------------------------------------------------------- */

static void probe_main(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    int32_t master = 0;
    uint8_t list[640];
    status_t st = audioctl_streams_until(probe.ch, now() + PROBE_WAIT, &count, &master, list);
    uint64_t us = (now() - probe.from) / NS_PER_US;
    const char *how = probe.promoted ? "a spare" : "a new process";
    if (st == OK)
        printf("init: %s: %s to first answer %lu us (%s)\n", probe.path,
               probe.killed ? "kill" : "end", (unsigned long)us, how);
    else
        printf("init: %s: no answer %lu us after the %s (%s; %s)\n", probe.path,
               (unsigned long)us, probe.killed ? "kill" : "end", how, status_str(st));
    jam_handle_close(probe.ch);
    __atomic_store_n(&probe.busy, false, __ATOMIC_RELEASE);   /* probe_start reads it */
}

/* Ask the new instance one cheap thing on a thread of its own, and say
 * when it answered (from `from`). One probe at a time: a restart while
 * one waits isn't measured. */
static void probe_start(unsigned i, uint64_t from, bool killed, bool promoted)
{
    if (__atomic_load_n(&probe.busy, __ATOMIC_ACQUIRE)) {   /* probe_main releases it */
        printf("init: %s: this restart isn't measured (the last one's call still waits)\n",
               svcs[i].path);
        return;
    }
    if (probe.thread)
        jam_handle_close(probe.thread);
    probe.thread = HANDLE_INVALID;
    handle_t ctl = services_audioctl();
    if (!ctl || jam_handle_duplicate(ctl, RIGHT_SAME, &probe.ch) != OK)
        return;
    probe.from = from;
    probe.killed = killed;
    probe.promoted = promoted;
    probe.path = svcs[i].path;
    __atomic_store_n(&probe.busy, true, __ATOMIC_RELAXED);   /* no probe thread runs now */
    if (thread_spawn("restart probe", probe_main, NULL, probe_stack, sizeof(probe_stack),
                     &probe.thread) != OK) {
        __atomic_store_n(&probe.busy, false, __ATOMIC_RELAXED);
        probe.thread = HANDLE_INVALID;
        jam_handle_close(probe.ch);
    }
}

/* Say how long svc i's restart took to get it running (at `up`), after
 * the deliberate kill at kill_at (0: it crashed), and measure its first
 * answer. */
static void said(unsigned i, bool promoted, uint64_t kill_at, uint64_t up)
{
    struct svc *s = &svcs[i];
    bool killed = kill_at != 0;
    uint64_t from = killed ? kill_at : s->ended_at;
    const char *how = promoted ? "spare promoted" : "new process started (no spare waited)";
    if (killed)
        printf("init: %s: %s %lu us after the kill (its end seen at %lu us)\n", s->path, how,
               (unsigned long)((up - from) / NS_PER_US),
               (unsigned long)((s->ended_at - from) / NS_PER_US));
    else
        printf("init: %s: %s %lu us after its end was seen\n", s->path, how,
               (unsigned long)((up - from) / NS_PER_US));
    probe_start(i, from, killed, promoted);
}

status_t kept_start(unsigned i, struct spawn_handle *x, unsigned nx)
{
    struct kept *k = kept_of(i);
    rights_t r[STANDBY_MAX_HANDLES];
    if (!k || nx + KEPT_EXTRA > STANDBY_MAX_HANDLES) {
        for (unsigned j = 0; j < nx; j++)
            if (x[j].h)
                jam_handle_close(x[j].h);
        return ERR_INVALID_ARGS;
    }
    for (unsigned j = 0; j < nx; j++)
        r[j] = RIGHT_SAME;
    nx = kept_handles(k, x, r, nx);
    struct svc *s = &svcs[i];
    uint64_t kill_at = s->kill_at;   /* the start clears it */
    status_t st = promote(i, x, r, nx);
    bool promoted = st == OK;
    if (st == ERR_NOT_FOUND) {
        const char *argv[] = { s->path };
        struct svc_args a = { .argc = 1, .argv = argv, .x = x, .rights = r, .nx = nx };
        st = svc_start_args(i, &a);
    }
    if (st != OK)
        return st;
    uint64_t up = now();
    hand_over(k);
    if (s->ended_at)   /* a restart */
        said(i, promoted, kill_at, up);
    return OK;
}
