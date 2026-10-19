/* volumes: a filesystem service's process, and its supervision (until
 * M12's storage split, devmgr's: a filesystem service was a binding like
 * a driver's, started by bind.c and supervised by supervise.c).
 *
 * A start makes everything from scratch: a job of its own (a child of
 * volumes' job, with a driver's limits), a new `block` channel on its
 * partition and a new `fsctl` channel (fs_handles), a duplicate of the
 * `fs` server end volumes keeps (fs_serve_end), and its state and keep
 * channel (spare.c). Its process is named after its mount ("fat-data"):
 * in the log, in `ps`, for the shell's `kill`.
 *
 * When its process terminates:
 *   - exit 0 by itself (not killed): the service is finished (its fs
 *     channel's clients all left). Not restarted; its job must be empty.
 *   - FAT_EXIT_NO_VOLUME (EXTFS_EXIT_NO_VOLUME for an ext volume) from
 *     another stick's partition: no volume there, left alone (fsvc.c's
 *     fs_check_ended); the ESP's service ending before it answered the
 *     boot-disk check: not the boot disk.
 *   - anything else -- a crash, a kill by anyone (volumes.kill included),
 *     an exit with an error: restarted, by the rule of a service that
 *     outlives its process (spare.c, docs/HISTORY.md, M11.6 Q5; the restart
 *     window is the supervision module's, <supervise.h>): its `fs` channel
 *     stays volumes', so its mount never goes away while it restarts and
 *     calls on it wait for the next instance; a deliberate kill
 *     (volumes.kill) is not counted and restarts at once; a crash or an
 *     error exit is counted, the first in the window restarts at once and
 *     later ones back off (100 ms, doubling, at most 5 s); the death after
 *     5 counted restarts within 60 s gives up: no more restarts, its mount
 *     goes, a log line and a RESULTS line. Its restart promotes the warm
 *     spare when one waits. A crash, an error exit or a give-up of the
 *     boot disk's services is a problem (volumes exits 1 at the end); one
 *     of another stick's (excused: a stick that fails its reads is that
 *     stick's trouble) or a test disk's is not.
 *
 * On the desktop (notices, <notice.h>) a boot-disk service's first crash
 * in its window and its give-up are notices in plain words ("File system
 * crashed / Jam OS started it again"); a deliberate kill, another stick's
 * service and anything while volumes stops are not. */
#include <extfssvc.h>
#include <fatsvc.h>
#include <idl/fsctl.h>
#include "disk.h"

/* b ending is never a problem of ours: a test disk's, or the filesystem
 * service of a stick that isn't the boot disk (but /jam, the Jam OS drive's
 * data, is as much ours as /data). */
static bool excused(const struct vol *b)
{
    return b->test || (b->other && !b->jam);
}

/* A notice of b's crash (first: the first in its window) or give-up, if
 * it is news to a user. */
static void tell_desktop(const struct vol *b, bool gave_up, bool first)
{
    if (excused(b) || stopping || (!gave_up && !first))
        return;
    notice_post(&notices, gave_up ? "File system stopped" : "File system crashed",
                gave_up ? "It kept crashing; Jam OS gave up."
                        : "Jam OS started it again.",
                '!', NOTICE_RASPBERRY);
}

/* ---- the process --------------------------------------------------------------------- */

void fs_watch_answers(struct vol *b)
{
    if (!b->client || b->client_key)
        return;
    uint64_t key = KEY_EV_OF(b - vols, b->gen);
    if (jam_port_bind(port, b->client, key, SIG_READABLE, PORT_BIND_PERSISTENT) != OK ||
        (b->own && jam_port_bind(port, b->own, key, SIG_READABLE, PORT_BIND_PERSISTENT) != OK)) {
        (void)jam_port_unbind(port, b->client, key);
        say(false, "volumes: %s: can't watch its channel for answers", b->name);
        return;
    }
    b->client_key = key;
}

status_t fs_spawn(struct vol *b)
{
    struct spawn_handle x[STARTUP_MAX_HANDLES];
    rights_t xr[STARTUP_MAX_HANDLES];
    unsigned n = 0;
    handle_t job = HANDLE_INVALID, proc = HANDLE_INVALID;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    const struct sup_limit *lim = b->ext ? ext_limits : fat_limits;
    unsigned nlim = b->ext ? ext_nlimits : fat_nlimits;
    for (unsigned i = 0; st == OK && i < nlim; i++)
        st = jam_job_set_limit(job, lim[i].kind, lim[i].value);
    if (st == OK)
        st = fs_handles(b, x, xr, &n);
    if (st == OK)
        st = fs_serve_end(b, x, xr, &n);
    if (st == OK) {
        /* Named after its mount, and told it; the one that may format a
         * blank partition is told so, and one that replaces a dead
         * instance how that one ended. */
        char mount[16];
        snprintf(mount, sizeof(mount), "%s", fs_mount_path(b));
        const char *argv[6] = { b->name, mount };
        int argc = 2;
        if (fs_format_arg(b))
            argv[argc++] = fs_format_arg(b);
        if (fs_end_arg(b))   /* "killed", "crashed" or none */
            argv[argc++] = fs_end_arg(b);
        if (fs_esp_arg(b))   /* another computer's boot files: views read-only */
            argv[argc++] = fs_esp_arg(b);
        /* drv/nvme's writes are FUA; without the word extfs flushes the
         * disk's cache where its journal needs it (a USB stick's) */
        const struct disk *d = disk_of(b);
        if (b->ext && d && d->bus == DISKS_BUS_NVME)
            argv[argc++] = EXTFS_ARG_FUA;
        struct spawn_args a = {
            .path = b->path, .name = b->name, .argc = argc, .argv = argv, .job = job,
            .extra = x, .nextra = n, .extra_rights = xr,
        };
        st = spawn(&a, &proc);   /* consumes the extras either way */
        n = 0;
    }
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(x[i].h);   /* made, never sent */
    if (st == OK) {
        b->gen++;
        st = jam_port_bind(port, proc, KEY_OF(b - vols, b->gen), SIG_TERMINATED, PORT_BIND_ONCE);
        if (st != OK)
            jam_handle_close(proc);   /* the job kill below takes the process */
    }
    if (st != OK) {
        if (job) {
            jam_job_kill(job);   /* whatever did start */
            jam_handle_close(job);
        }
        /* Its end of the `fs` channel went with the extras: the next start
         * makes a new one. */
        fs_close_client(b);
        return st;
    }
    b->job = job;
    b->proc = proc;
    b->killed = false;
    b->spare_now = false;
    b->state = VOLUMES_SUP_RUNNING;
    fs_watch_answers(b);
    disk_started(b);
    return OK;
}

void fs_close_client(struct vol *b)
{
    call_fail(fs_asker(b));   /* an fs.sync a task waits for: no answer comes now */
    if (b->client && b->client_key)
        jam_port_unbind(port, b->client, b->client_key);
    if (b->own && b->client_key)
        jam_port_unbind(port, b->own, b->client_key);
    b->client_key = 0;
    if (b->client)
        jam_handle_close(b->client);
    b->client = HANDLE_INVALID;
    if (b->own)
        jam_handle_close(b->own);
    if (b->own_serve)
        jam_handle_close(b->own_serve);
    b->own = b->own_serve = HANDLE_INVALID;
}

void fs_kill(struct vol *b)
{
    if (b->job)
        jam_job_kill(b->job);   /* returns once everything in it is dead */
}

void fs_forget(struct vol *b)
{
    if (b->proc) {
        jam_port_unbind(port, b->proc, KEY_OF(b - vols, b->gen));
        jam_handle_close(b->proc);
    }
    if (b->job)
        jam_handle_close(b->job);
    b->proc = b->job = HANDLE_INVALID;
    disk_stopped(b);   /* no mount until it is back, unless its kept channel is one */
}

/* Wait until b's process has ended, or deadline (ERR_TIMED_OUT). In a
 * task the loop goes on meanwhile (sup_died leaves a process being
 * stopped alone, and every round wakes the task to look again);
 * ERR_PEER_CLOSED: b was retired meanwhile. Outside one, a plain wait
 * (only after a kill: the kernel has ended it already). */
static status_t wait_ended(struct vol *b, uint64_t deadline)
{
    signals_t seen;
    if (!in_task())
        return jam_object_wait_one(b->proc, SIG_TERMINATED, deadline, &seen);
    uint32_t serial = b->serial;
    for (;;) {
        if (b->serial != serial || !b->proc)
            return ERR_PEER_CLOSED;
        if (jam_object_wait_one(b->proc, SIG_TERMINATED, 0, &seen) == OK)
            return OK;
        if (now() >= deadline)
            return ERR_TIMED_OUT;
        task_wait(tasks, deadline);
    }
}

bool fs_stop(struct vol *b, bool kill, bool excused_end)
{
    if (!b->proc)
        return true;
    fs_close_client(b);   /* a serving instance sees its client gone */
    if (kill)
        fs_kill(b);
    status_t st = wait_ended(b, now() + STOP_WAIT);
    if (st == ERR_PEER_CLOSED)
        return false;   /* retired meanwhile, its process with it */
    bool ok = st == OK;
    if (st != OK) {
        say(true, "volumes: %s %s did not stop in %lu s: killing its job", b->name, b->path,
            (unsigned long)(STOP_WAIT / NS_PER_S));
        fs_kill(b);
    }
    struct process_info info;
    if (jam_process_get_info(b->proc, &info) != OK || info.state != PROCESS_DEAD)
        ok = false;
    else if (!excused_end && (info.killed || info.exit_code))
        ok = false;
    if (ok && !kill && !info.killed)
        ok = sup_job_empty(b->job, b->path);
    fs_forget(b);
    return ok;
}

status_t fs_stop_in_order(struct vol *b)
{
    uint32_t serial = b->serial;
    status_t st = ERR_NOT_SUPPORTED;
    b->stopping = true;   /* its end is ours: no restart */
    if (b->ctl) {
        struct call c;
        call_begin(&c, b->ctl);
        st = fsctl_stop_send(b->ctl, c.txid);
        if (st == OK)
            st = call_wait(&c, now() + SYNC_WAIT);
        else
            call_end(&c);
        if (st == OK)
            st = fsctl_stop_result(c.rep, &c.m);
    }
    if (b->serial != serial)
        return ERR_PEER_CLOSED;   /* retired meanwhile: its disk went, or volumes stops */
    if (st != OK)
        say(false, "volumes: %s: its filesystem service did not stop in order (%s): killed%s",
            fs_mount_path(b), status_str(st), b->rw ? "; what it had not synced is lost" : "");
    if (!fs_stop(b, st != OK, true) && b->serial != serial)
        return ERR_PEER_CLOSED;
    b->stopping = false;
    return st;
}

/* ---- supervision ------------------------------------------------------------------------ */

/* The dead service's channel goes (the kept one is dropped only when it
 * is given up on). */
static void drop_channel(struct vol *b)
{
    fs_close_client(b);
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
}

/* b's service is gone (or its restart failed): schedule the next start, or
 * give up. `why` says what happened; `expected`: not a problem. A
 * deliberate kill is never counted, so never gives up. */
static void schedule(struct vol *b, const char *why, bool expected)
{
    uint64_t t = now();
    unsigned n = sup_window_recent(&b->win, t);
    bool deliberate = b->killed;
    uint64_t at;   /* when the restart is due */
    if (!fs_channel_kept(b))
        drop_channel(b);
    if (!sup_window_end(&b->win, t, b->killed ? SUP_END_ON_PURPOSE : SUP_END_COUNTED, true,
                        &at)) {
        b->state = VOLUMES_SUP_GAVE_UP;
        say(!excused(b) || b->test, "volumes: %s %s %s after %u restarts in %lu s: giving up%s",
            b->name, b->path, why, n, (unsigned long)(SUP_WINDOW / NS_PER_S),
            b->test ? " (a test disk's: expected)"
            : excused(b) ? " (another stick's filesystem: left alone)" : "");
        if (!excused(b))
            problems++;
        if (!deliberate)
            tell_desktop(b, true, false);
        drop_channel(b);   /* its clients see ERR_PEER_CLOSED: the mount is gone */
        fs_kept_release(b);
        mounts_update();
        return;
    }
    b->restart_at = at;
    b->deliberate = deliberate;
    b->state = VOLUMES_SUP_RESTARTING;
    /* The channel the restart will serve: the one it had. (One that went
     * is made again here, so calls wait for the restart; if that fails,
     * the start makes one.) */
    if (!b->client && fs_new_channel(b, &b->client, &b->serve) != OK)
        b->client = b->serve = HANDLE_INVALID;
    if (deliberate)
        say(false, "volumes: %s %s %s: not counted, restarted at once", b->name, b->path, why);
    else
        say(!expected, "volumes: %s %s %s: restart %u in %u ms", b->name, b->path, why, n + 1,
            b->win.backoff_ms);
    if (!expected && !deliberate)
        tell_desktop(b, false, n == 0);
}

void sup_died(struct vol *b, uint32_t gen)
{
    struct process_info info;
    if (b->stopping)
        return;   /* a task stops it in order: its end is that task's (it looks every round) */
    if (b->state != VOLUMES_SUP_RUNNING || !b->proc || gen != (b->gen & 0xffffu) ||
        jam_process_get_info(b->proc, &info) != OK || info.state != PROCESS_DEAD)
        return;   /* a stale packet, or it was handled already */
    fs_kill(b);   /* whatever else its job started */
    b->ended_at = now();
    bool no_volume = !info.killed && !b->killed &&
                     info.exit_code == (b->ext ? EXTFS_EXIT_NO_VOLUME : FAT_EXIT_NO_VOLUME);
    if (fs_check_ended(b, no_volume))
        return;   /* no FAT volume on someone else's partition: no restart, no problem */
    if (!info.killed && info.exit_code == 0 && !b->killed) {
        bool clean = sup_job_empty(b->job, b->path);
        fs_forget(b);
        fs_close_client(b);
        b->state = VOLUMES_SUP_FINISHED;
        say(!clean, "volumes: %s %s exited%s", b->name, b->path,
            clean ? "" : ", but did not end cleanly");
        problems += !clean;
        return;
    }
    fs_forget(b);
    bool expected = b->killed || excused(b);
    if (!expected)
        problems++;
    char why[48];
    if (b->killed)
        snprintf(why, sizeof(why), "was killed (KILL)");
    else if (info.killed)
        snprintf(why, sizeof(why), "crashed");
    else
        snprintf(why, sizeof(why), "exited with code %ld", (long)info.exit_code);
    schedule(b, why, expected);
}

status_t sup_kill(struct vol *b, uint64_t *koid)
{
    struct process_info info;
    if (b->state != VOLUMES_SUP_RUNNING || !b->proc ||
        jam_process_get_info(b->proc, &info) != OK)
        return ERR_NOT_FOUND;   /* nothing runs */
    *koid = info.koid;
    b->kill_at = now();   /* its restart is timed from here (spare.c) */
    fs_kill(b);
    signals_t seen;
    status_t st = jam_object_wait_one(b->proc, SIG_TERMINATED, now() + STOP_WAIT, &seen);
    b->killed = true;
    sup_died(b, b->gen);   /* a death like any other: the restart is scheduled now */
    return st;
}

/* Is b's restart due to be started (not under way already, not held
 * back by its disk's release)? */
static bool restart_waits(const struct vol *b)
{
    const struct disk *d = disk_of(b);
    return b->state == VOLUMES_SUP_RESTARTING && !b->starting && !(d && d->releasing);
}

uint64_t sup_next_deadline(void)
{
    uint64_t next = spare_next_deadline();
    for (unsigned i = 0; i < nvols; i++)
        if (restart_waits(&vols[i]) && vols[i].restart_at < next)
            next = vols[i].restart_at;
    return next;
}

/* A restart's task: the start (a new `block` channel: a call), then what
 * came of it. */
static void restart_main(void *arg)
{
    struct vol *b = arg;
    uint32_t serial = b->serial;
    status_t st = fs_run(b, true);
    if (b->serial != serial || stopping)
        return;   /* retired meanwhile (its disk went), or volumes stops */
    b->starting = false;
    task_kick(tasks);   /* a kill may wait for this restart (request.c) */
    b->last = st;
    if (st == OK) {
        say(false, "volumes: %s %s restarted (restart %u since boot)", b->name, b->path,
            b->win.restarts);
        return;
    }
    if (st == ERR_PEER_CLOSED) {
        say(false, "volumes: %s %s: its disk went before its restart", b->name, b->path);
        fs_retire(b);
        return;
    }
    char why[48];
    snprintf(why, sizeof(why), "could not be restarted (%s)", status_str(st));
    if (!excused(b))
        problems++;
    schedule(b, why, excused(b));
}

void sup_run_due(void)
{
    if (stopping)
        return;   /* no more restarts */
    for (unsigned i = 0; i < nvols; i++) {
        struct vol *b = &vols[i];
        uint64_t t = now();
        if (!restart_waits(b) || b->restart_at > t)
            continue;
        /* a deliberate kill's isn't counted in the window */
        sup_window_started(&b->win, t, !b->deliberate);
        b->deliberate = false;
        b->killed = false;   /* if this start fails, that is no deliberate kill */
        b->starting = true;
        if (!task_start(tasks, restart_main, b)) {
            b->starting = false;   /* every task is busy: again in a moment */
            b->restart_at = t + 100 * NS_PER_MS;
        }
    }
    spare_due();   /* after the restarts: a promotion is what makes it due */
}

void sup_reset(struct vol *b)
{
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
    sup_window_reset(&b->win);
    b->restart_at = 0;
    if (b->state == VOLUMES_SUP_RESTARTING)
        b->state = VOLUMES_SUP_NONE;
}
