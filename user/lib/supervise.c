/* What init, devmgr and (after the storage split) volumes share to supervise
 * a service: the restart window, the warm spare, the keeper and state VMO
 * that outlive an instance, and the measured restart. The contract is in
 * <supervise.h>; the design is docs/M11.6-PLAN.md ("The warm spare",
 * "Supervision").
 *
 * **The warm spare** is a second process of the program started with
 * nothing but SR_STANDBY, which waits in libos before main, having opened
 * and read nothing (user/lib/start.c). Promoting it is one message with
 * the startup handles a new process would have been given, and the time
 * of the kill; it runs main as if just started. Starting a process takes
 * CPU time the promoted instance needs while it sets itself up, so the
 * next spare starts SPARE_REFILL later (init measured it for the mixer:
 * kill to first answer 3.4 ms with the new spare started at once, against
 * 2.0 ms for a plain process start).
 *
 * **The keeper and the state** (<keep.h>, <svcstate.h>): each instance
 * gets a new keep channel; what the dead one wrote on the old one is
 * still taken first (keeper_attach). The state VMO is made once and every
 * instance gets a handle with SVCSTATE_SERVICE_RIGHTS only (no resize,
 * transfer or duplicate); the supervisor never maps it. Only after the
 * instance has its end does the keeper write its restore into it: the
 * kernel won't send a channel end whose queue holds channel ends. */
#include <stdarg.h>

#include <supervise.h>

#define SPARE_REFILL (50 * NS_PER_MS)   /* the next spare, after a promotion */
#define SPARE_FIRST  (100 * NS_PER_MS)  /* the wait before a spare that ended is replaced */
#define SPARE_MAX    (5 * NS_PER_S)     /* ... doubling up to this */
#define SPARE_STEADY (10 * NS_PER_S)    /* a spare that waited this long ended by no fault of
                                         * its start: the wait starts again from SPARE_FIRST */
#define PROBE_WAIT   (5 * NS_PER_S)     /* how long the probe waits for the first answer */

/* ---- the log ----------------------------------------------------------------------- */

static const char *log_prefix = "?";
static sup_say_fn log_sink;

void sup_log(const char *prefix, sup_say_fn say)
{
    log_prefix = prefix;
    log_sink = say;
}

static void say_line(bool report, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* One line, "<prefix>: <text>", to the sink. */
static void say_line(bool report, const char *fmt, ...)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s: ", log_prefix);
    if (n < 0 || n >= (int)sizeof(line))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (log_sink)
        log_sink(report, line);
}

#define SAY(...) say_line(false, __VA_ARGS__)

/* ---- the restart window ------------------------------------------------------------ */

unsigned sup_window_recent(const struct sup_window *w, uint64_t t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SUP_RESTART_LIMIT; i++)
        n += w->restarted[i] && t - w->restarted[i] < SUP_WINDOW;
    return n;
}

bool sup_window_end(struct sup_window *w, uint64_t t, enum sup_end why, bool outlives,
                    uint64_t *at)
{
    bool on_purpose = outlives && why == SUP_END_ON_PURPOSE;
    unsigned n = sup_window_recent(w, t);
    if (n >= SUP_RESTART_LIMIT && !on_purpose)
        return false;
    uint64_t delay = 0;   /* a death on purpose, or the first in the window of a service that
                           * outlives its process: at once */
    if (!outlives || (!on_purpose && n > 0)) {
        unsigned step = outlives ? n - 1 : n;
        delay = step < 16 ? SUP_BACKOFF_FIRST << step : SUP_BACKOFF_MAX;
        if (delay > SUP_BACKOFF_MAX)
            delay = SUP_BACKOFF_MAX;
    }
    w->backoff_ms = (uint32_t)(delay / NS_PER_MS);
    *at = t + delay;
    return true;
}

void sup_window_started(struct sup_window *w, uint64_t t, bool counted)
{
    if (counted)
        w->restarted[w->restarts % SUP_RESTART_LIMIT] = t;
    w->restarts++;
}

void sup_window_reset(struct sup_window *w)
{
    for (unsigned i = 0; i < SUP_RESTART_LIMIT; i++)
        w->restarted[i] = 0;
    w->backoff_ms = 0;
}

/* ---- the warm spare ---------------------------------------------------------------- */

void sup_spare_init(struct sup_spare *sp, const struct sup_spare_cfg *cfg, bool on)
{
    *sp = (struct sup_spare){ .cfg = *cfg, .on = on };
}

void sup_spare_arm(struct sup_spare *sp, uint64_t first_at)
{
    if (sp->armed)
        return;
    sp->armed = true;
    sp->next_try = first_at;
}

bool sup_spare_waits(const struct sup_spare *sp)
{
    return sp->proc != HANDLE_INVALID;
}

void sup_spare_dismiss(struct sup_spare *sp)
{
    if (!sp->proc)
        return;
    (void)jam_port_unbind(sp->cfg.port, sp->proc, sp->cfg.key);   /* ERR_NOT_FOUND: it fired */
    jam_handle_close(sp->standby);
    jam_job_kill(sp->job);
    jam_handle_close(sp->proc);
    jam_handle_close(sp->job);
    sp->proc = sp->job = sp->standby = HANDLE_INVALID;
}

void sup_spare_stop(struct sup_spare *sp)
{
    sp->stopped = true;
    sup_spare_dismiss(sp);
}

/* A new job for the spare, with the limits of the instance it becomes. */
static status_t spare_job(const struct sup_spare_cfg *cfg, handle_t *out)
{
    handle_t job;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    for (unsigned i = 0; st == OK && i < cfg->nlimits; i++)
        st = jam_job_set_limit(job, cfg->limits[i].kind, cfg->limits[i].value);
    if (st != OK && job)
        jam_handle_close(job);
    if (st == OK)
        *out = job;
    return st;
}

/* Start a spare: SR_STANDBY its only handle. */
static status_t spare_start(struct sup_spare *sp)
{
    const struct sup_spare_cfg *cfg = &sp->cfg;
    handle_t mine, theirs, job = HANDLE_INVALID, proc = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = spare_job(cfg, &job);
    if (st == OK) {
        const char *argv0 = cfg->name ? cfg->name : cfg->path;
        struct spawn_handle x[] = { { SR_STANDBY, theirs } };
        const char *argv[] = { argv0 };
        struct spawn_args a = { .path = cfg->path, .name = cfg->name, .argc = 1, .argv = argv,
                                .job = job, .extra = x, .nextra = 1 };
        st = spawn(&a, &proc);   /* consumes theirs either way */
        theirs = HANDLE_INVALID;
    }
    if (st == OK)
        st = jam_port_bind(cfg->port, proc, cfg->key, SIG_TERMINATED, PORT_BIND_ONCE);
    if (st != OK) {
        if (theirs)
            jam_handle_close(theirs);
        if (job) {
            jam_job_kill(job);
            jam_handle_close(job);
        }
        if (proc)
            jam_handle_close(proc);
        jam_handle_close(mine);
        return st;
    }
    sp->proc = proc;
    sp->job = job;
    sp->standby = mine;
    sp->started = now();
    struct process_info info;
    if (jam_process_get_info(proc, &info) == OK)
        SAY("a warm spare %s waits (process %lu)", cfg->path, (unsigned long)info.koid);
    return OK;
}

/* The wait before the next spare after one that ended at t, doubling if
 * spares keep ending soon after their start. */
static void spare_backoff_after_end(struct sup_spare *sp, uint64_t t)
{
    sp->backoff = t - sp->started > SPARE_STEADY || !sp->backoff ? SPARE_FIRST : sp->backoff * 2;
    if (sp->backoff > SPARE_MAX)
        sp->backoff = SPARE_MAX;
    sp->next_try = t + sp->backoff;
}

/* The spare's process has ended (by itself, or someone killed it). */
static void spare_ended(struct sup_spare *sp, uint64_t t)
{
    struct process_info info;
    bool got = jam_process_get_info(sp->proc, &info) == OK;
    spare_backoff_after_end(sp, t);
    SAY("the warm spare %s %s %ld: another in %lu ms", sp->cfg.path,
        got && info.killed ? "was killed, code" : "exited with code",
        got ? (long)info.exit_code : -1L, (unsigned long)(sp->backoff / NS_PER_MS));
    sup_spare_dismiss(sp);
}

void sup_spare_event(struct sup_spare *sp)
{
    signals_t seen;
    if (!sp->proc || jam_object_wait_one(sp->proc, SIG_TERMINATED, 0, &seen) != OK)
        return;   /* an earlier spare's packet: it was promoted or dismissed since */
    spare_ended(sp, now());
}

uint64_t sup_spare_next(const struct sup_spare *sp)
{
    if (!sp->on || sp->stopped || !sp->armed || sp->proc)
        return DEADLINE_NEVER;   /* off, stopping, not wanted yet, or one waits */
    return sp->next_try;
}

uint64_t sup_spare_due(struct sup_spare *sp, uint64_t t)
{
    if (sup_spare_next(sp) == DEADLINE_NEVER)
        return DEADLINE_NEVER;
    if (t < sp->next_try)
        return sp->next_try;
    status_t st = spare_start(sp);
    if (st == OK)
        return DEADLINE_NEVER;
    sp->backoff = sp->backoff ? sp->backoff * 2 : SPARE_FIRST;
    if (sp->backoff > SPARE_MAX)
        sp->backoff = SPARE_MAX;
    sp->next_try = t + sp->backoff;
    SAY("can't start a warm spare %s (%s)", sp->cfg.path, status_str(st));
    return sp->next_try;
}

bool sup_spare_ready(struct sup_spare *sp)
{
    signals_t seen;
    if (!sp->on || !sp->proc)
        return false;
    if (jam_object_wait_one(sp->proc, SIG_TERMINATED, 0, &seen) == OK) {
        spare_ended(sp, now());   /* its packet may still be on its way */
        return false;
    }
    return true;
}

status_t sup_spare_promote(struct sup_spare *sp, const struct standby_args *a, handle_t *proc,
                           handle_t *job)
{
    if (!sp->proc)
        return ERR_NOT_FOUND;
    handle_t p = sp->proc, j = sp->job;
    (void)jam_port_unbind(sp->cfg.port, p, sp->cfg.key);   /* the caller's from now on */
    status_t st = standby_promote(sp->standby, a);         /* consumes a->hs */
    jam_handle_close(sp->standby);   /* a promotion already queued is still read */
    sp->proc = sp->job = sp->standby = HANDLE_INVALID;
    sp->next_try = now() + SPARE_REFILL;
    if (st != OK) {
        SAY("the warm spare %s couldn't be promoted (%s)", sp->cfg.path, status_str(st));
        jam_job_kill(j);
        jam_handle_close(p);
        jam_handle_close(j);
        return st;
    }
    *proc = p;
    *job = j;
    return OK;
}

/* ---- what outlives an instance ----------------------------------------------------- */

void sup_kept_init(struct sup_kept *k, handle_t port, uint64_t state_size, const char *name)
{
    keeper_init(&k->keeper);
    k->state = HANDLE_INVALID;
    k->state_size = state_size;
    k->port = port;
    k->key = 0;
    k->bound = false;
    k->gen = 0;
    snprintf(k->name, sizeof(k->name), "%s", name);
}

static void kept_unbind(struct sup_kept *k)
{
    if (k->bound)   /* ours: it was bound */
        (void)jam_port_unbind(k->port, k->keeper.ch, k->key);
    k->bound = false;
}

unsigned sup_kept_handles(struct sup_kept *k, struct spawn_handle *x, rights_t *r, unsigned n)
{
    status_t st = k->state ? OK : svcstate_create(k->state_size, &k->state);
    handle_t h = HANDLE_INVALID;
    if (st == OK)
        st = svcstate_give(k->state, &h);
    if (st == OK) {
        x[n] = (struct spawn_handle){ SR_STATE, h };
        r[n++] = SVCSTATE_SERVICE_RIGHTS;   /* the transfer right comes off in transit */
    } else {
        SAY("%s starts without its state (%s)", k->name, status_str(st));
    }
    kept_unbind(k);
    st = keeper_attach(&k->keeper, &h);   /* the dead instance's last puts are taken first */
    if (st == OK) {
        k->gen++;
        x[n] = (struct spawn_handle){ SR_KEEP, h };
        r[n++] = RIGHT_SAME;
    } else {
        SAY("%s starts without a keep channel (%s)", k->name, status_str(st));
    }
    return n;
}

uint16_t sup_kept_gen(const struct sup_kept *k)
{
    return k->gen;
}

void sup_kept_hand_over(struct sup_kept *k, uint64_t key)
{
    if (k->keeper.ch == HANDLE_INVALID)
        return;
    status_t st = keeper_restore(&k->keeper);
    if (st != OK && st != ERR_PEER_CLOSED) {   /* PEER_CLOSED: it died already; the loop sees */
        SAY("%s can't be handed what was kept (%s): its clients' ends close", k->name,
            status_str(st));
        keeper_release(&k->keeper);
        return;
    }
    st = jam_port_bind(k->port, k->keeper.ch, key, SIG_READABLE, PORT_BIND_PERSISTENT);
    k->bound = st == OK;
    k->key = key;
    if (st != OK)   /* what it sends waits on the channel until its next start */
        SAY("%s's keeper can't listen (%s)", k->name, status_str(st));
}

bool sup_kept_listening(const struct sup_kept *k, uint16_t gen)
{
    return k->bound && k->gen == gen;
}

void sup_kept_event(struct sup_kept *k)
{
    if (k->bound)
        keeper_drain(&k->keeper);   /* a persistent binding fires again only on a new edge */
}

void sup_kept_release(struct sup_kept *k)
{
    kept_unbind(k);
    keeper_release(&k->keeper);
    if (k->state)
        jam_handle_close(k->state);
    k->state = HANDLE_INVALID;
}

/* ---- a restart, measured ----------------------------------------------------------- */

/* The first-answer probe: a thread's, from the start of sup_restarted's
 * until busy clears. */
static struct {
    bool           busy;       /* a probe runs (atomic: released by the thread when done) */
    handle_t       thread;     /* the last probe's thread (closed before the next) */
    handle_t       ch;         /* the probe's */
    sup_probe_call call;
    uint64_t       from;       /* the kill, or the end seen (uptime ns) */
    bool           killed;     /* from is a deliberate kill */
    bool           promoted;   /* the instance asked was a promoted spare */
    char           name[32];
} probe;
static _Alignas(16) uint8_t probe_stack[16 << 10];

static void probe_main(void *arg)
{
    (void)arg;
    status_t st = probe.call(probe.ch, now() + PROBE_WAIT);
    uint64_t us = (now() - probe.from) / NS_PER_US;
    const char *how = probe.promoted ? "a spare" : "a new process";
    if (st == OK)
        SAY("%s: %s to first answer %lu us (%s)", probe.name, probe.killed ? "kill" : "end",
            (unsigned long)us, how);
    else
        SAY("%s: no answer %lu us after the %s (%s; %s)", probe.name, (unsigned long)us,
            probe.killed ? "kill" : "end", how, status_str(st));
    jam_handle_close(probe.ch);
    __atomic_store_n(&probe.busy, false, __ATOMIC_RELEASE);   /* probe_start reads it */
}

/* Ask the new instance one cheap thing on a thread of its own, and say
 * when it answered (from `from`). */
static void probe_start(const char *name, uint64_t from, bool killed, bool promoted,
                        handle_t ch, sup_probe_call call)
{
    if (__atomic_load_n(&probe.busy, __ATOMIC_ACQUIRE)) {   /* probe_main releases it */
        SAY("%s: this restart isn't measured (the last one's call still waits)", name);
        if (ch)
            jam_handle_close(ch);
        return;
    }
    if (probe.thread)
        jam_handle_close(probe.thread);
    probe.thread = HANDLE_INVALID;
    if (!ch)
        return;
    probe.ch = ch;
    probe.call = call;
    probe.from = from;
    probe.killed = killed;
    probe.promoted = promoted;
    snprintf(probe.name, sizeof(probe.name), "%s", name);
    __atomic_store_n(&probe.busy, true, __ATOMIC_RELAXED);   /* no probe thread runs now */
    if (thread_spawn("restart probe", probe_main, NULL, probe_stack, sizeof(probe_stack),
                     &probe.thread) != OK) {
        __atomic_store_n(&probe.busy, false, __ATOMIC_RELAXED);
        probe.thread = HANDLE_INVALID;
        jam_handle_close(probe.ch);
    }
}

void sup_restarted(const char *name, bool promoted, uint64_t kill_at, uint64_t ended_at,
                   uint64_t up, handle_t ch, sup_probe_call call)
{
    bool killed = kill_at != 0;
    uint64_t from = killed ? kill_at : ended_at;
    const char *how = promoted ? "spare promoted" : "new process started (no spare waited)";
    if (killed)
        SAY("%s: %s %lu us after the kill (its end seen at %lu us)", name, how,
            (unsigned long)((up - from) / NS_PER_US),
            (unsigned long)((ended_at - from) / NS_PER_US));
    else
        SAY("%s: %s %lu us after its end was seen", name, how,
            (unsigned long)((up - from) / NS_PER_US));
    probe_start(name, from, killed, promoted, ch, call);
}

/* ---- jobs that have ended ---------------------------------------------------------- */

/* Dead is not yet freed: a process that has ended can still be charged a
 * moment later for what it left on its way out (a request still queued
 * at the server it called, which the server reads when it gets to it; its
 * address space's pages, given back on another CPU). A job found not
 * empty is looked at again until JOB_RECHECK has passed, and only then
 * called not clean. */
#define JOB_RECHECK  (2 * NS_PER_S)
#define JOB_POLL     (20 * NS_PER_MS)
#define MAX_RECHECKS 16

static struct {
    handle_t job;        /* a duplicate of the job's handle; 0: a free slot */
    char     who[32];    /* the process's path, for the log */
    uint64_t until;      /* when it is called unclean if still not empty */
} rechecks[MAX_RECHECKS];
static unsigned job_unclean;      /* jobs not empty in the end, since the start */
static unsigned recheck_failures; /* ... since sup_job_settle's last call */

/* Is anything still charged to the job? With `log`, say what. False if its
 * counts can't be read (then it counts as not empty). */
static bool job_clear(handle_t job, const char *who, bool log)
{
    struct job_info ji;
    if (jam_job_get_info(job, &ji) != OK)
        return false;
    bool empty = true;
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k]) {
            if (log)
                SAY("%s left %lu units of job kind %u", who, (unsigned long)ji.used[k], k);
            empty = false;
        }
    return empty;
}

bool sup_job_empty(handle_t job, const char *who)
{
    if (job_clear(job, who, false))
        return true;
    for (unsigned i = 0; i < MAX_RECHECKS; i++) {
        if (rechecks[i].job)
            continue;
        if (jam_handle_duplicate(job, RIGHT_SAME, &rechecks[i].job) != OK)
            break;
        snprintf(rechecks[i].who, sizeof(rechecks[i].who), "%s", who ? who : "?");
        rechecks[i].until = now() + JOB_RECHECK;
        return true;   /* for now: sup_job_run_due decides */
    }
    return job_clear(job, who, true);   /* no room to look again: decided now */
}

void sup_job_run_due(void)
{
    uint64_t t = now();
    for (unsigned i = 0; i < MAX_RECHECKS; i++) {
        if (!rechecks[i].job)
            continue;
        bool empty = job_clear(rechecks[i].job, rechecks[i].who, false);
        if (!empty && t < rechecks[i].until)
            continue;
        if (!empty) {
            (void)job_clear(rechecks[i].job, rechecks[i].who, true);
            say_line(true, "%s did not end cleanly: its job was not empty %lu ms after it "
                     "ended", rechecks[i].who, (unsigned long)(JOB_RECHECK / NS_PER_MS));
            job_unclean++;
            recheck_failures++;
        }
        jam_handle_close(rechecks[i].job);
        rechecks[i].job = HANDLE_INVALID;
    }
}

uint64_t sup_job_next_deadline(void)
{
    for (unsigned i = 0; i < MAX_RECHECKS; i++)
        if (rechecks[i].job)
            return now() + JOB_POLL;
    return DEADLINE_NEVER;
}

bool sup_job_settle(void)
{
    uint64_t end = now() + JOB_RECHECK + JOB_POLL;
    while (sup_job_next_deadline() != DEADLINE_NEVER && now() < end) {
        sup_job_run_due();
        jam_sleep_until(now() + JOB_POLL);
    }
    sup_job_run_due();
    bool ok = recheck_failures == 0;
    recheck_failures = 0;
    return ok;
}

unsigned sup_job_unclean(void)
{
    return job_unclean;
}
