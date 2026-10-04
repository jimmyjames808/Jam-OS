/* devmgr: a filesystem service that outlives its process
 * (docs/M11.6-PLAN.md): for each filesystem binding (/data, /esp, each
 * /usbN) a keeper and a state VMO, and one warm spare fat for every mount.
 * init does the same for the mixer (user/services/init/spare.c).
 *
 * **What a binding keeps** (struct fs_kept, made at its first start):
 *   - its `fs` channel, both ends (fsvc.c's fs_serve_end): each instance is
 *     handed a duplicate of the server end, so the mount stays listed while
 *     no instance runs, and calls on it wait in the channel for the next;
 *   - a state VMO (<svcstate.h>), FAT_STATE_SIZE bytes, made the first
 *     time and handed to every instance as SR_STATE with
 *     SVCSTATE_SERVICE_RIGHTS only (no resize, transfer or duplicate);
 *     devmgr never maps it. Its pages are committed only as fat uses them,
 *     and charged to devmgr's job;
 *   - a keeper (<keep.h>): each instance gets a new keep channel (SR_KEEP);
 *     what the dead one put on the old one is taken first (keeper_attach),
 *     and once the instance has its end the keeper writes its restore
 *     into it (the kernel won't send a channel end whose queue holds
 *     channel ends).
 * All three go when the binding is retired (its disk gone, no volume),
 * given up on, or stopped in order (a remount: fsvc.c's fs_restart): then
 * the kept objects close and their clients see ERR_PEER_CLOSED.
 *
 * fat carries on from the state a dead instance left (user/services/fat/
 * adopt.c): the volume as it was, its open files and views (the keeper's
 * handles), the request in progress finished exactly once. Each instance
 * is told how the last one ended (fs_end_arg: "killed" or "crashed"), for
 * its rule against a request that crashes it again and again.
 *
 * **The warm spare** is one bin/fat started with nothing but SR_STANDBY
 * (process name "fat-spare"), waiting in libos before main, having opened
 * and read nothing (user/lib/start.c), so it can become any mount's
 * service. When a filesystem service is restarted (supervise.c: at once
 * for a deliberate kill and the first crash in a minute) the spare is
 * promoted with the handles a new process would have been given, and the
 * time of the kill; it runs main as if just started. A new spare starts
 * SPARE_REFILL later, not at once: a process start takes CPU time the
 * promoted one needs while it mounts (init measured it for the mixer).
 * Without a spare (the boot word `nospare`, or none ready yet) a new
 * process is started, with the same handles. The first spare starts
 * SPARE_BOOT after the first filesystem service, so it doesn't compete
 * with the boot's own mounts.
 *
 * **The `block` channel of /data is opened in advance** while a spare waits,
 * so the promotion that matters most needs no call to usb-storage; any
 * other mount's is opened at its promotion (a call, CALL_WAIT at most, as
 * every start makes). It is a channel of its own, not the dead fat's:
 * usb-storage's fence (drivers/usb-storage/block.c) drops what a dead
 * client left queued, so nothing of the dead fat lands after its
 * successor's writes.
 *
 * **Measured, every restart:** when the new instance runs, after the kill
 * (DEVMGR_KILL) or after its end was seen (a crash); and, on a thread of
 * its own that serves nothing (so it may block), one cheap call on the
 * kept `fs` channel (fs.statfs): kill to first answer, as a client feels
 * it. One probe at a time; a restart while one waits isn't measured. The
 * spare's memory is its job's (the shell's `ps`).
 *
 * Everything here runs on devmgr's loop thread but the probe, which
 * touches only `probe` (its busy flag released when it is done). */
#include <fatsvc.h>
#include <fs_idl.h>
#include <idl/storage.h>
#include <keep.h>
#include <svcstate.h>
#include "disk.h"

#define SPARE_NAME   "fat-spare"
#define SPARE_BOOT   (1 * NS_PER_S)      /* the first spare, after the first service's start */
#define SPARE_REFILL (50 * NS_PER_MS)    /* the next spare, after a promotion */
#define SPARE_FIRST  (100 * NS_PER_MS)   /* the wait before a spare that ended is replaced */
#define SPARE_MAX    (5 * NS_PER_S)      /* ... doubling up to this */
#define SPARE_STEADY (10 * NS_PER_S)     /* a spare that waited this long ended by no fault of
                                          * its start: the wait starts again from SPARE_FIRST */
#define BLOCK_RETRY  (1 * NS_PER_S)      /* /data's channel in advance couldn't be opened */
#define PROBE_WAIT   (5 * NS_PER_S)      /* how long the probe waits for the first answer */
#define MAX_X        8                   /* handles a fat is given */

/* What a filesystem binding keeps across its instances. */
struct fs_kept {
    struct keeper keeper;    /* its keep channel and what it kept */
    handle_t      state;     /* its state VMO, every right, never mapped; 0: not made */
    bool          bound;     /* keeper.ch is bound on the port (KEY_KEEP_OF) */
    uint16_t      key_gen;   /* bumped with every new keep channel: in its port key */
};

/* The same limits bind.c gives every driver's job (16 MiB, 256 handles,
 * 16 threads, 1 MiB queued): the spare becomes one. */
static const struct { uint32_t kind; uint64_t value; } spare_limits[] = {
    { JOB_LIMIT_PAGES, 4096 },
    { JOB_LIMIT_HANDLES, 256 },
    { JOB_LIMIT_THREADS, 16 },
    { JOB_LIMIT_MSG_BYTES, 1u << 20 },
};

static struct {
    handle_t proc, job;   /* while one waits; 0: none */
    handle_t standby;     /* our end of its SR_STANDBY */
    uint64_t started;     /* uptime ns */
    uint64_t next_try;    /* the next may start then (uptime ns) */
    uint64_t backoff;     /* the last wait after a spare ended by itself, ns */
    bool     armed;       /* a filesystem service has started: spares are wanted */
    handle_t block;       /* /data's `block` channel, opened in advance; 0: none */
    uint32_t block_for;   /* ... for this binding (devs index + 1) */
    uint64_t block_try;   /* the next try to open it (uptime ns) */
} spare;

/* The first-answer probe: a thread's, from probe_start until busy clears. */
static struct {
    bool        busy;       /* a probe runs (atomic: released by the thread when done) */
    handle_t    thread;     /* the last probe's thread (closed before the next) */
    handle_t    ch;         /* a duplicate of the kept `fs` client end, the probe's */
    uint64_t    from;       /* the kill, or the end seen (uptime ns) */
    bool        killed;     /* from is a deliberate kill */
    bool        promoted;   /* the instance asked was a promoted spare */
    char        name[32];   /* the binding's ("fat-data") */
} probe;
static _Alignas(16) uint8_t probe_stack[16 << 10];

static bool spares_on, stopping;

void spare_init(bool on)
{
    spares_on = on;
    if (!on)
        say(false, "devmgr: nospare: no warm spare %s; a restart starts a process", FAT_PATH);
}

bool spare_waits(void)
{
    return spare.proc != HANDLE_INVALID;
}

static bool peer_closed(handle_t h)
{
    signals_t seen;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, 0, &seen) == OK;
}

/* ---- /data's block channel, in advance ------------------------------------------------ */

static void drop_block(void)
{
    if (spare.block)
        jam_handle_close(spare.block);
    spare.block = HANDLE_INVALID;
    spare.block_for = 0;
}

/* The boot disk's /data service (not a test disk's), if it runs. */
static struct binding *data_binding(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        const struct disk *d = &disks[i];
        if (d->state != DISK_BOOT || d->test || !d->fs[PART_DATA])
            continue;
        struct binding *b = &devs[d->fs[PART_DATA] - 1];
        return b->state == DEVMGR_SUP_RUNNING && b->proc ? b : NULL;
    }
    return NULL;
}

/* Open /data's channel for the spare, unless it has one for the /data
 * that runs now. */
static void prepare_block(uint64_t t)
{
    struct binding *b = data_binding();
    uint32_t want = b ? (uint32_t)(b - devs) + 1 : 0;
    if (spare.block && (spare.block_for != want || peer_closed(spare.block)))
        drop_block();
    if (spare.block || !b || t < spare.block_try)
        return;
    status_t st = storage_open_partition_until(disk_ch(disk_of(b)), now() + CALL_WAIT, b->part,
                                               false, &spare.block);
    if (st != OK) {
        spare.block = HANDLE_INVALID;
        spare.block_try = t + BLOCK_RETRY;
        say(false, "devmgr: can't open /data's block channel for the spare (%s)", status_str(st));
        return;
    }
    spare.block_for = want;
}

bool fs_block_prepared(const struct binding *b, handle_t *out)
{
    if (!spare.block || spare.block_for != (uint32_t)(b - devs) + 1)
        return false;
    handle_t h = spare.block;
    spare.block = HANDLE_INVALID;
    spare.block_for = 0;
    if (peer_closed(h)) {   /* its disk went: the caller opens one, or learns that */
        jam_handle_close(h);
        return false;
    }
    *out = h;
    return true;
}

/* ---- the spare -------------------------------------------------------------------------- */

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

/* A new job for the spare, with a driver's limits. */
static status_t spare_job(handle_t *out)
{
    handle_t job;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    for (unsigned i = 0; st == OK && i < sizeof(spare_limits) / sizeof(spare_limits[0]); i++)
        st = jam_job_set_limit(job, spare_limits[i].kind, spare_limits[i].value);
    if (st != OK && job)
        jam_handle_close(job);
    if (st == OK)
        *out = job;
    return st;
}

/* Start a spare: SR_STANDBY its only handle. */
static status_t spare_start(void)
{
    handle_t mine, theirs, job = HANDLE_INVALID, proc = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = spare_job(&job);
    if (st == OK) {
        struct spawn_handle x[] = { { SR_STANDBY, theirs } };
        const char *argv[] = { SPARE_NAME };
        struct spawn_args a = { .path = FAT_PATH, .name = SPARE_NAME, .argc = 1, .argv = argv,
                                .job = job, .extra = x, .nextra = 1 };
        st = spawn(&a, &proc);   /* consumes theirs either way */
        theirs = HANDLE_INVALID;
    }
    if (st == OK)
        st = jam_port_bind(port, proc, KEY_SPARE, SIG_TERMINATED, PORT_BIND_ONCE);
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
    spare.proc = proc;
    spare.job = job;
    spare.standby = mine;
    spare.started = now();
    struct process_info info;
    if (jam_process_get_info(proc, &info) == OK)
        say(false, "devmgr: a warm spare %s waits (process %lu)", FAT_PATH,
            (unsigned long)info.koid);
    return OK;
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
    say(false, "devmgr: the warm spare %s %s %ld: another in %lu ms", FAT_PATH,
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

void spare_due(void)
{
    if (!spares_on || stopping || !spare.armed)
        return;
    uint64_t t = now();
    if (!spare.proc && t >= spare.next_try) {
        status_t st = spare_start();
        if (st != OK) {
            spare.backoff = spare.backoff ? spare.backoff * 2 : SPARE_FIRST;
            if (spare.backoff > SPARE_MAX)
                spare.backoff = SPARE_MAX;
            spare.next_try = t + spare.backoff;
            say(false, "devmgr: can't start a warm spare %s (%s)", FAT_PATH, status_str(st));
        }
    }
    if (spare.proc)
        prepare_block(t);
}

uint64_t spare_next_deadline(void)
{
    if (!spares_on || stopping || !spare.armed)
        return DEADLINE_NEVER;
    if (!spare.proc)
        return spare.next_try;
    return !spare.block && spare.block_try > now() && data_binding() ? spare.block_try
                                                                      : DEADLINE_NEVER;
}

void spare_stop(void)
{
    stopping = true;
    spare_drop();
    drop_block();
}

/* ---- the keeper and the state ---------------------------------------------------------- */

static void keep_unbind(const struct binding *b, struct fs_kept *k)
{
    if (k->bound)   /* ours: it was bound */
        (void)jam_port_unbind(port, k->keeper.ch, KEY_KEEP_OF(b - devs, k->key_gen));
    k->bound = false;
}

void fs_kept_handles(struct binding *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (!b->kept && (b->kept = calloc(1, sizeof(*b->kept))) != NULL)
        keeper_init(&b->kept->keeper);
    struct fs_kept *k = b->kept;
    if (!k) {
        say(false, "devmgr: %s starts without its state and keeper (no memory)", b->name);
        return;
    }
    status_t st = k->state ? OK : svcstate_create(FAT_STATE_SIZE, &k->state);
    handle_t h = HANDLE_INVALID;
    if (st == OK)
        st = svcstate_give(k->state, &h);
    if (st == OK) {
        x[*n] = (struct spawn_handle){ SR_STATE, h };
        xr[(*n)++] = SVCSTATE_SERVICE_RIGHTS;   /* the transfer right comes off in transit */
    } else {
        say(false, "devmgr: %s starts without its state (%s)", b->name, status_str(st));
    }
    keep_unbind(b, k);
    st = keeper_attach(&k->keeper, &h);   /* the dead instance's last puts are taken first */
    if (st == OK) {
        k->key_gen++;
        x[*n] = (struct spawn_handle){ SR_KEEP, h };
        xr[(*n)++] = RIGHT_SAME;
    } else {
        say(false, "devmgr: %s starts without a keep channel (%s)", b->name, status_str(st));
    }
}

/* b's instance has its keep channel's end: hand it what was kept, then
 * listen to it. */
static void hand_over(struct binding *b)
{
    struct fs_kept *k = b->kept;
    if (!k || k->keeper.ch == HANDLE_INVALID)
        return;
    status_t st = keeper_restore(&k->keeper);
    if (st != OK && st != ERR_PEER_CLOSED) {   /* PEER_CLOSED: it died already; the loop sees */
        say(false, "devmgr: %s can't be handed what was kept (%s): its clients' ends close",
            b->name, status_str(st));
        keeper_release(&k->keeper);
        return;
    }
    st = jam_port_bind(port, k->keeper.ch, KEY_KEEP_OF(b - devs, k->key_gen), SIG_READABLE,
                       PORT_BIND_PERSISTENT);
    k->bound = st == OK;
    if (st != OK)   /* what it sends waits on the channel until its next start */
        say(false, "devmgr: %s's keeper can't listen (%s)", b->name, status_str(st));
}

void spare_keep_event(uint64_t key)
{
    uint32_t i = KEY_INDEX(key);
    if (i >= ndevs || !devs[i].kept || !devs[i].kept->bound ||
        KEY_GEN(key) != (devs[i].kept->key_gen & 0xffffu))
        return;   /* stale */
    keeper_drain(&devs[i].kept->keeper);   /* a persistent binding fires again only on a new edge */
}

void fs_kept_release(struct binding *b)
{
    if (spare.block_for == (uint32_t)(b - devs) + 1)
        drop_block();
    struct fs_kept *k = b->kept;
    if (!k)
        return;
    keep_unbind(b, k);
    keeper_release(&k->keeper);
    if (k->state)
        jam_handle_close(k->state);
    free(k);
    b->kept = NULL;
}

/* ---- promotion ------------------------------------------------------------------------- */

/* b runs as proc in job from now on: what start_driver does after its
 * spawn. */
static status_t adopt(struct binding *b, handle_t proc, handle_t job)
{
    b->gen++;
    status_t st = jam_port_bind(port, proc, KEY_OF(b - devs, b->gen), SIG_TERMINATED,
                                PORT_BIND_ONCE);
    if (st != OK) {
        jam_job_kill(job);
        jam_handle_close(proc);
        jam_handle_close(job);
        return st;
    }
    b->job = job;
    b->proc = proc;
    b->killed = false;
    b->state = DEVMGR_SUP_RUNNING;
    b->promoted++;
    if (b->client && !b->client_key) {   /* a new `fs` channel: its answers are watched */
        uint64_t key = KEY_EV_OF(b - devs, b->gen);
        if (jam_port_bind(port, b->client, key, SIG_READABLE, PORT_BIND_PERSISTENT) == OK)
            b->client_key = key;
    }
    disk_started(b);
    return OK;
}

/* Promote the spare to be b's service. ERR_NOT_FOUND: no spare waits, or
 * it couldn't be promoted (start a process instead); else OK or why b's
 * handles couldn't be made (ERR_PEER_CLOSED: its disk is gone). */
static status_t promote(struct binding *b)
{
    signals_t seen;
    if (!spares_on || !spare.proc)
        return ERR_NOT_FOUND;
    if (jam_object_wait_one(spare.proc, SIG_TERMINATED, 0, &seen) == OK) {
        spare_ended(now());   /* its packet may still be on its way */
        return ERR_NOT_FOUND;
    }
    struct spawn_handle x[MAX_X];
    rights_t xr[MAX_X];
    unsigned n = 0;
    status_t st = fs_handles(b, x, xr, &n);
    if (st == OK)
        st = fs_serve_end(b, x, xr, &n);
    if (st != OK) {
        for (unsigned i = 0; i < n; i++)
            jam_handle_close(x[i].h);
        return st;
    }
    char mount[16];
    snprintf(mount, sizeof(mount), "%s", fs_mount_path(b));
    const char *argv[4] = { b->name, mount };
    int argc = 2;
    if (fs_format_arg(b))
        argv[argc++] = fs_format_arg(b);
    if (fs_end_arg(b))   /* how the last instance ended: fat counts only crashes */
        argv[argc++] = fs_end_arg(b);
    struct standby_args a = { .hs = x, .rights = xr, .n = n, .argc = argc,
                              .argv = argv, .kill_ns = b->kill_at ? b->kill_at : b->ended_at };
    handle_t proc = spare.proc, job = spare.job;
    (void)jam_port_unbind(port, proc, KEY_SPARE);   /* b's from now on */
    st = standby_promote(spare.standby, &a);        /* consumes x */
    jam_handle_close(spare.standby);   /* a promotion already queued is still read */
    spare.proc = spare.job = spare.standby = HANDLE_INVALID;
    spare.next_try = now() + SPARE_REFILL;
    if (st != OK) {
        say(false, "devmgr: the warm spare %s couldn't be promoted (%s)", FAT_PATH,
            status_str(st));
        jam_job_kill(job);
        jam_handle_close(proc);
        jam_handle_close(job);
        return ERR_NOT_FOUND;
    }
    return adopt(b, proc, job);
}

/* ---- the restart, measured ---------------------------------------------------------------- */

static void probe_main(void *arg)
{
    (void)arg;
    uint64_t total = 0, free_bytes = 0;
    uint8_t ro = 0, label[16];
    status_t st = fs_statfs_until(probe.ch, now() + PROBE_WAIT, &total, &free_bytes, &ro, label);
    uint64_t us = (now() - probe.from) / NS_PER_US;
    const char *how = probe.promoted ? "a spare" : "a new process";
    if (st == OK)
        say(false, "devmgr: %s: %s to first answer %lu us (%s)", probe.name,
            probe.killed ? "kill" : "end", (unsigned long)us, how);
    else
        say(false, "devmgr: %s: no answer %lu us after the %s (%s; %s)", probe.name,
            (unsigned long)us, probe.killed ? "kill" : "end", how, status_str(st));
    jam_handle_close(probe.ch);
    __atomic_store_n(&probe.busy, false, __ATOMIC_RELEASE);   /* probe_start reads it */
}

/* Ask b's new instance one cheap thing on a thread of its own, and say
 * when it answered (from `from`). */
static void probe_start(const struct binding *b, uint64_t from, bool killed, bool promoted)
{
    if (__atomic_load_n(&probe.busy, __ATOMIC_ACQUIRE)) {   /* probe_main releases it */
        say(false, "devmgr: %s: this restart isn't measured (the last one's call still waits)",
            b->name);
        return;
    }
    if (probe.thread)
        jam_handle_close(probe.thread);
    probe.thread = HANDLE_INVALID;
    if (!b->client || jam_handle_duplicate(b->client, RIGHT_SAME, &probe.ch) != OK)
        return;
    probe.from = from;
    probe.killed = killed;
    probe.promoted = promoted;
    snprintf(probe.name, sizeof(probe.name), "%s", b->name);
    __atomic_store_n(&probe.busy, true, __ATOMIC_RELAXED);   /* no probe thread runs now */
    if (thread_spawn("restart probe", probe_main, NULL, probe_stack, sizeof(probe_stack),
                     &probe.thread) != OK) {
        __atomic_store_n(&probe.busy, false, __ATOMIC_RELAXED);
        probe.thread = HANDLE_INVALID;
        jam_handle_close(probe.ch);
    }
}

/* Say how long b's restart took to get it running (at `up`), after the
 * deliberate kill or the end seen, and measure its first answer. */
static void restarted(struct binding *b, bool promoted, uint64_t up)
{
    bool killed = b->kill_at != 0;
    uint64_t from = killed ? b->kill_at : b->ended_at;
    const char *how = promoted ? "spare promoted" : "new process started (no spare waited)";
    if (killed)
        say(false, "devmgr: %s: %s %lu us after the kill (its end seen at %lu us)", b->name, how,
            (unsigned long)((up - from) / NS_PER_US),
            (unsigned long)((b->ended_at - from) / NS_PER_US));
    else
        say(false, "devmgr: %s: %s %lu us after its end was seen", b->name, how,
            (unsigned long)((up - from) / NS_PER_US));
    probe_start(b, from, killed, promoted);
}

status_t fs_run(struct binding *b, bool restart)
{
    status_t st = restart ? promote(b) : ERR_NOT_FOUND;
    bool promoted = st == OK;
    if (st == ERR_NOT_FOUND)
        st = start_driver(b);
    if (st != OK)
        return st;
    uint64_t up = now();
    if (!spare.armed) {   /* the first filesystem service: a spare from now on */
        spare.armed = true;
        spare.next_try = up + SPARE_BOOT;
    }
    hand_over(b);
    if (restart && b->ended_at)
        restarted(b, promoted, up);
    b->kill_at = 0;
    return OK;
}
