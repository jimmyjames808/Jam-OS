/* volumes: a filesystem service that outlives its process
 * (docs/M11.6-PLAN.md): for each filesystem service (/data, /esp, each
 * /usbN) a keeper and a state VMO, and one warm spare fat for every mount.
 * init does the same for the mixer (user/services/init/spare.c); how a
 * spare, a keeper and a measured restart work is the supervision module's
 * (<supervise.h>, user/lib/supervise.c). What is fat's and volumes', and
 * stays here:
 *
 * **What a service keeps** (a struct sup_kept, made at its first start):
 *   - its `fs` channel, both ends (fsvc.c's fs_serve_end): each instance is
 *     handed a duplicate of the server end, so the mount stays listed while
 *     no instance runs, and calls on it wait in the channel for the next;
 *   - a state VMO (<svcstate.h>), FAT_STATE_SIZE bytes, handed to every
 *     instance as SR_STATE; volumes never maps it. Its pages are committed
 *     only as fat uses them, and charged to volumes' job;
 *   - a keeper (<keep.h>): each instance gets a new keep channel (SR_KEEP).
 * All three go when the service is retired (its disk gone, no volume),
 * given up on, or stopped in order (a remount: fsvc.c's fs_restart): then
 * the kept objects close and their clients see ERR_PEER_CLOSED.
 *
 * fat carries on from the state a dead instance left (user/services/fat/
 * adopt.c): the volume as it was, its open files and views (the keeper's
 * handles), the request in progress finished exactly once. Each instance
 * is told how the last one ended (fs_end_arg: "killed" or "crashed"), for
 * its rule against a request that crashes it again and again.
 *
 * **The warm spare** is one bin/fat (process name "fat-spare"), so it can
 * become any mount's service. When a filesystem service is restarted
 * (supervise.c: at once for a deliberate kill and the first crash in a
 * minute) the spare is promoted with the handles a new process would have
 * been given, and the time of the kill. Without a spare (the boot word
 * `nospare`, or none ready yet) a new process is started, with the same
 * handles. The first spare starts SPARE_BOOT after the first filesystem
 * service, so it doesn't compete with the boot's own mounts.
 *
 * **The `block` channel of /data is opened in advance** while a spare waits,
 * so the promotion that matters most needs no call to usb-storage; any
 * other mount's is opened at its promotion (a call, CALL_WAIT at most, as
 * every start makes, waited for in the start's task). Opening it in
 * advance is a task of its own too (prepare_main). It is a channel of its own, not the dead fat's:
 * usb-storage's fence (drivers/usb-storage/block.c) drops what a dead
 * client left queued, so nothing of the dead fat lands after its
 * successor's writes.
 *
 * **Measured, every restart:** when the new instance runs, after the kill
 * (VOLUMES_KILL) or after its end was seen (a crash); and one cheap call on
 * the kept `fs` channel (fs.statfs): kill to first answer, as a client
 * feels it.
 *
 * Everything here runs on volumes' loop thread. */
#include <fatsvc.h>
#include <fs_idl.h>
#include <idl/storage.h>
#include <supervise.h>
#include "disk.h"

#define SPARE_NAME   "fat-spare"
#define SPARE_BOOT   (1 * NS_PER_S)      /* the first spare, after the first service's start */
#define BLOCK_RETRY  (1 * NS_PER_S)      /* /data's channel in advance couldn't be opened */
#define MAX_X        8                   /* handles a fat is given */

/* The limits devmgr gives every driver's job (16 MiB, 256 handles, 16
 * threads, 1 MiB queued): a filesystem service's (supervise.c), and the
 * spare's, which becomes one. */
const struct sup_limit fat_limits[] = {
    { JOB_LIMIT_PAGES, 4096 },
    { JOB_LIMIT_HANDLES, 256 },
    { JOB_LIMIT_THREADS, 16 },
    { JOB_LIMIT_MSG_BYTES, 1u << 20 },
};
const unsigned fat_nlimits = sizeof(fat_limits) / sizeof(fat_limits[0]);

static struct sup_spare spare;   /* the warm spare fat */

/* /data's `block` channel, opened for the spare in advance. */
static struct {
    handle_t block;       /* 0: none */
    uint32_t block_for;   /* ... for this service (vols index + 1) */
    uint64_t block_try;   /* the next try to open it (uptime ns) */
    bool     opening;     /* prepare_main is at it */
} adv;

void spare_init(bool on)
{
    const struct sup_spare_cfg cfg = { .path = FAT_PATH, .name = SPARE_NAME,
                                       .limits = fat_limits, .nlimits = fat_nlimits,
                                       .port = port, .key = KEY_SPARE };
    sup_spare_init(&spare, &cfg, on);
    if (!on)
        say(false, "volumes: nospare: no warm spare %s; a restart starts a process", FAT_PATH);
}

bool spare_waits(void)
{
    return sup_spare_waits(&spare);
}

static bool peer_closed(handle_t h)
{
    signals_t seen;
    return jam_object_wait_one(h, SIG_PEER_CLOSED, 0, &seen) == OK;
}

/* ---- /data's block channel, in advance ------------------------------------------------ */

static void drop_block(void)
{
    if (adv.block)
        jam_handle_close(adv.block);
    adv.block = HANDLE_INVALID;
    adv.block_for = 0;
}

/* The boot disk's /data service (not a test disk's), if it runs. */
static struct vol *data_binding(void)
{
    for (unsigned i = 0; i < MAX_DISKS; i++) {
        const struct disk *d = &disks[i];
        if (d->state != DISK_BOOT || d->test || !d->fs[PART_DATA])
            continue;
        struct vol *b = &vols[d->fs[PART_DATA] - 1];
        return b->state == VOLUMES_SUP_RUNNING && b->proc ? b : NULL;
    }
    return NULL;
}

/* prepare_block's task: the call to the disk's driver, waited for. */
static void prepare_main(void *arg)
{
    struct vol *b = arg;
    uint32_t serial = b->serial;
    handle_t h = HANDLE_INVALID;
    status_t st = fs_open_block(b, false, &h);
    adv.opening = false;
    if (st == OK && (b->serial != serial || data_binding() != b || adv.block)) {
        jam_handle_close(h);   /* /data moved on meanwhile: the next round opens another */
        return;
    }
    if (st != OK) {
        if (st != ERR_PEER_CLOSED || !stopping) {
            adv.block_try = now() + BLOCK_RETRY;
            say(false, "volumes: can't open /data's block channel for the spare (%s)",
                status_str(st));
        }
        return;
    }
    adv.block = h;
    adv.block_for = (uint32_t)(b - vols) + 1;
}

/* Open /data's channel for the spare (in a task), unless it has one for
 * the /data that runs now. */
static void prepare_block(uint64_t t)
{
    struct vol *b = data_binding();
    uint32_t want = b ? (uint32_t)(b - vols) + 1 : 0;
    if (adv.block && (adv.block_for != want || peer_closed(adv.block)))
        drop_block();
    if (adv.block || adv.opening || !b || t < adv.block_try || stopping)
        return;
    adv.opening = true;
    if (!task_start(tasks, prepare_main, b)) {
        adv.opening = false;
        adv.block_try = t + BLOCK_RETRY;   /* every task is busy */
    }
}

bool fs_block_prepared(const struct vol *b, handle_t *out)
{
    if (!adv.block || adv.block_for != (uint32_t)(b - vols) + 1)
        return false;
    handle_t h = adv.block;
    adv.block = HANDLE_INVALID;
    adv.block_for = 0;
    if (peer_closed(h)) {   /* its disk went: the caller opens one, or learns that */
        jam_handle_close(h);
        return false;
    }
    *out = h;
    return true;
}

/* ---- the spare -------------------------------------------------------------------------- */

void spare_event(void)
{
    sup_spare_event(&spare);
}

void spare_due(void)
{
    uint64_t t = now();
    (void)sup_spare_due(&spare, t);
    if (sup_spare_waits(&spare))
        prepare_block(t);
}

uint64_t spare_next_deadline(void)
{
    if (!sup_spare_waits(&spare))
        return sup_spare_next(&spare);
    return !adv.block && !adv.opening && adv.block_try > now() && data_binding()
               ? adv.block_try : DEADLINE_NEVER;
}

void spare_stop(void)
{
    sup_spare_stop(&spare);
    drop_block();
}

/* ---- the keeper and the state ---------------------------------------------------------- */

void fs_kept_handles(struct vol *b, struct spawn_handle *x, rights_t *xr, unsigned *n)
{
    if (!b->kept && (b->kept = calloc(1, sizeof(*b->kept))) != NULL)
        sup_kept_init(b->kept, port, FAT_STATE_SIZE, b->name);
    if (!b->kept) {
        say(false, "volumes: %s starts without its state and keeper (no memory)", b->name);
        return;
    }
    *n = sup_kept_handles(b->kept, x, xr, *n);
}

/* b's instance has its keep channel's end: hand it what was kept, then
 * listen to it. */
static void hand_over(struct vol *b)
{
    if (b->kept)
        sup_kept_hand_over(b->kept, KEY_KEEP_OF(b - vols, sup_kept_gen(b->kept)));
}

void spare_keep_event(uint64_t key)
{
    uint32_t i = KEY_INDEX(key);
    if (i >= nvols || !vols[i].kept || !sup_kept_listening(vols[i].kept, KEY_GEN(key)))
        return;   /* stale */
    sup_kept_event(vols[i].kept);
}

void fs_kept_release(struct vol *b)
{
    if (adv.block_for == (uint32_t)(b - vols) + 1)
        drop_block();
    if (!b->kept)
        return;
    sup_kept_release(b->kept);
    free(b->kept);
    b->kept = NULL;
}

/* ---- promotion ------------------------------------------------------------------------- */

/* b runs as proc in job from now on: what fs_spawn does after its
 * spawn. */
static status_t adopt(struct vol *b, handle_t proc, handle_t job)
{
    b->gen++;
    status_t st = jam_port_bind(port, proc, KEY_OF(b - vols, b->gen), SIG_TERMINATED,
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
    b->state = VOLUMES_SUP_RUNNING;
    b->promoted++;
    b->spare_now = true;   /* its process is called "fat-spare" */
    if (b->client && !b->client_key) {   /* a new `fs` channel: its answers are watched */
        uint64_t key = KEY_EV_OF(b - vols, b->gen);
        if (jam_port_bind(port, fs_asker(b), key, SIG_READABLE, PORT_BIND_PERSISTENT) == OK)
            b->client_key = key;
    }
    disk_started(b);
    return OK;
}

/* Promote the spare to be b's service. ERR_NOT_FOUND: no spare waits, or
 * it couldn't be promoted (start a process instead); else OK or why b's
 * handles couldn't be made (ERR_PEER_CLOSED: its disk is gone). */
static status_t promote(struct vol *b)
{
    if (!sup_spare_ready(&spare))
        return ERR_NOT_FOUND;
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
    handle_t proc, job;
    if (sup_spare_promote(&spare, &a, &proc, &job) != OK)   /* consumes x */
        return ERR_NOT_FOUND;
    return adopt(b, proc, job);
}

/* ---- the restart, measured ---------------------------------------------------------------- */

/* One cheap call on the kept `fs` channel: fs.statfs. */
static status_t probe_call(handle_t ch, uint64_t deadline)
{
    uint64_t total = 0, free_bytes = 0;
    uint8_t ro = 0, label[16];
    return fs_statfs_until(ch, deadline, &total, &free_bytes, &ro, label);
}

/* Say how long b's restart took to get it running (at `up`), after the
 * deliberate kill or the end seen, and measure its first answer. */
static void restarted(struct vol *b, bool promoted, uint64_t up)
{
    handle_t ch = HANDLE_INVALID;
    if (!b->client || jam_handle_duplicate(fs_asker(b), RIGHT_SAME, &ch) != OK)
        ch = HANDLE_INVALID;
    sup_restarted(b->name, promoted, b->kill_at, b->ended_at, up, ch, probe_call);
}

status_t fs_run(struct vol *b, bool restart)
{
    status_t st = restart ? promote(b) : ERR_NOT_FOUND;
    bool promoted = st == OK;
    if (st == ERR_NOT_FOUND)
        st = fs_spawn(b);
    if (st != OK)
        return st;
    uint64_t up = now();
    sup_spare_arm(&spare, up + SPARE_BOOT);   /* the first filesystem service: spares from now */
    hand_over(b);
    if (restart && b->ended_at)
        restarted(b, promoted, up);
    b->kill_at = 0;
    return OK;
}
