/* devmgr: driver supervision. When a driver's process terminates:
 *
 *   - exit code 0 by itself (not killed): the driver is finished (a
 *     one-shot driver, like hid on an interface that is not a boot
 *     keyboard or mouse). Not restarted; its job must be empty.
 *   - anything else -- a crash (the kernel killed it), a kill by anyone
 *     (DEVMGR_KILL included), an exit with an error: restarted. The rest
 *     of its job is killed first. Backoff: 100 ms after the first death,
 *     doubling with every restart in the last 60 s, at most 5 s. The death
 *     after 5 restarts within 60 s gives up: no more restarts, a log line
 *     and a RESULTS line. A real driver's crash, error exit or give-up is
 *     a problem (devmgr exits 1 at the end); a DEVMGR_KILL, and anything
 *     the crash-test driver does, is expected. So is the filesystem
 *     service of someone else's stick ending (excused): a stick that
 *     fails its reads is that stick's trouble, said in the log and no
 *     more; it is restarted and given up on like any other.
 *
 * A filesystem service (BIND_FS) outlives its process (spare.c,
 * docs/M11.6-PLAN.md Q5): its `fs` channel stays devmgr's, so its mount
 * never goes away while it restarts and calls on it wait for the next
 * instance; a deliberate kill (DEVMGR_KILL) is not counted and restarts
 * at once; a crash or an error exit is counted as above, but the first in
 * the window restarts at once and only later ones back off (100 ms,
 * doubling, at most 5 s). Its restart promotes the warm spare when one
 * waits.
 *
 * The restart itself is a start from scratch (bind.c: new dma_cap, so Bus
 * Master Enable stays off until the new driver has quiesced the device;
 * new interrupt object; the function woken to D0). As soon as the driver
 * has died, devmgr makes the channel the restarted driver will serve and
 * GET_SERVICE hands it out: clients that saw ERR_PEER_CLOSED reconnect at
 * once and their calls wait in the channel until the new driver reads
 * them (the reconnect rule, <devmgr.h>). */
#include <fatsvc.h>
#include "internal.h"

/* b ending is never a problem of ours: a test's, or the filesystem service
 * of a stick that isn't the boot disk. */
static bool excused(const struct binding *b)
{
    return b->test || (b->kind == BIND_FS && b->other);
}

/* Restarts within the window. */
static unsigned recent(const struct binding *b, uint64_t t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SUP_RESTART_LIMIT; i++)
        n += b->restarted[i] && t - b->restarted[i] < SUP_WINDOW;
    return n;
}

/* The dead driver's channel goes (a filesystem service's is kept). */
static void drop_channel(struct binding *b)
{
    close_client(b);
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
}

/* How long b's restart waits after n restarts in the window. */
static uint64_t backoff(const struct binding *b, unsigned n)
{
    if (b->kind == BIND_FS && (b->killed || n == 0))
        return 0;   /* a deliberate kill, or the first crash in the window */
    unsigned step = b->kind == BIND_FS ? n - 1 : n;
    uint64_t delay = step < 16 ? SUP_BACKOFF_FIRST << step : SUP_BACKOFF_MAX;
    return delay > SUP_BACKOFF_MAX ? SUP_BACKOFF_MAX : delay;
}

/* b's driver is gone (or its restart failed): schedule the next start, or
 * give up. `why` says what happened; `expected`: not a problem. A
 * filesystem service's deliberate kill is never counted, so never gives up. */
static void schedule(struct binding *b, const char *why, bool expected)
{
    uint64_t t = now();
    unsigned n = recent(b, t);
    bool deliberate = b->kind == BIND_FS && b->killed;
    if (!fs_channel_kept(b))
        drop_channel(b);
    if (n >= SUP_RESTART_LIMIT && !deliberate) {
        b->state = DEVMGR_SUP_GAVE_UP;
        say(!excused(b) || b->test, "devmgr: %s %s %s after %u restarts in %lu s: giving up%s",
            bdf(b), b->path, why, n, (unsigned long)(SUP_WINDOW / NS_PER_S),
            b->test ? " (the crash-test driver: expected)"
            : excused(b) ? " (another stick's filesystem: left alone)" : "");
        if (!excused(b))
            problems++;
        if (b->kind == BIND_FS) {
            drop_channel(b);   /* its clients see ERR_PEER_CLOSED: the mount is gone */
            fs_kept_release(b);
            mounts_update();
        }
        return;
    }
    uint64_t delay = backoff(b, n);
    b->backoff_ms = (uint32_t)(delay / NS_PER_MS);
    b->restart_at = t + delay;
    b->deliberate = deliberate;
    b->state = DEVMGR_SUP_RESTARTING;
    /* The channel the restart will serve, handed out from now on. (If this
     * fails, start_driver makes one and GET_SERVICE says ERR_BAD_STATE
     * meanwhile.) A filesystem service's is the one it had. */
    if (b->kind != BIND_USB && !b->client && jam_channel_create(&b->client, &b->serve) != OK)
        b->client = b->serve = HANDLE_INVALID;
    if (deliberate)
        say(false, "devmgr: %s %s %s: not counted, restarted at once", bdf(b), b->path, why);
    else
        say(!expected, "devmgr: %s %s %s: restart %u in %u ms", bdf(b), b->path, why, n + 1,
            b->backoff_ms);
}

void sup_died(struct binding *b, uint32_t gen)
{
    struct process_info info;
    if (b->state != DEVMGR_SUP_RUNNING || !b->proc || gen != (b->gen & 0xffffu) ||
        jam_process_get_info(b->proc, &info) != OK || info.state != PROCESS_DEAD)
        return;   /* a stale packet, or it was handled already */
    kill_driver(b);   /* whatever else its job started */
    b->ended_at = now();
    bool no_volume = !info.killed && !b->killed && info.exit_code == FAT_EXIT_NO_VOLUME;
    if (b->kind == BIND_FS && fs_check_ended(b, no_volume))
        return;   /* no FAT volume on someone else's partition: no restart, no problem */
    if (b->kind == BIND_USB) {
        /* A USB class driver (usb.c): its interface gone = the end of it,
         * however it ended; exit 0 because the console went = reconnect. */
        if (usb_gone(b)) {
            /* A disk's filesystem services go first: they map buffers its
             * driver made, which stay charged to the driver's job until
             * they are gone too (a service in the middle of a request has
             * not ended by itself yet). */
            disk_stopped(b);
            bool clean = job_empty(b->job, b->path);
            forget_driver(b);
            problems += !clean;
            usb_retire(b, clean ? "ended, device gone" : "device gone, did not end cleanly");
            return;
        }
        if (!info.killed && info.exit_code == 0 && !b->killed && usb_console_gone(b)) {
            forget_driver(b);
            b->state = DEVMGR_SUP_RESTARTING;
            b->console_wait = true;
            /* Try at once: if the console is still down, usb_handles says
             * ERR_SHOULD_WAIT and sup_run_due parks it for SET_CONSOLE. (A
             * SET_CONSOLE may well have come before this death.) */
            b->restart_at = now();
            b->input_gen = 0;
            say(false, "devmgr: %s %s: its console went away: reconnecting", bdf(b), b->path);
            return;
        }
    }
    if (!info.killed && info.exit_code == 0 && !b->killed) {
        bool clean = job_empty(b->job, b->path);
        forget_driver(b);
        close_client(b);
        b->state = DEVMGR_SUP_FINISHED;
        say(!clean, "devmgr: %s %s exited%s", bdf(b), b->path,
            clean ? "" : ", but did not end cleanly");
        problems += !clean;
        return;
    }
    forget_driver(b);
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

uint64_t sup_next_deadline(void)
{
    uint64_t next = spare_next_deadline();
    for (unsigned i = 0; i < ndevs; i++)
        if (devs[i].state == DEVMGR_SUP_RESTARTING && devs[i].restart_at < next)
            next = devs[i].restart_at;
    return next;
}

void sup_run_due(void)
{
    for (unsigned i = 0; i < ndevs; i++) {
        struct binding *b = &devs[i];
        uint64_t t = now();
        if (b->state != DEVMGR_SUP_RESTARTING || b->restart_at > t)
            continue;
        bool reconnect = b->console_wait;   /* not a restart: the console came back */
        if (!reconnect && !b->deliberate)   /* a deliberate kill's isn't counted */
            b->restarted[b->restarts % SUP_RESTART_LIMIT] = t;
        if (!reconnect)
            b->restarts++;
        b->deliberate = false;
        b->killed = false;   /* if this start fails, that is no deliberate kill */
        b->last = b->kind == BIND_FS ? fs_run(b, true) : start_driver(b);
        if (b->last == OK) {
            b->console_wait = false;
            if (reconnect)
                say(false, "devmgr: %s %s started again (the console is back)", bdf(b), b->path);
            else
                say(false, "devmgr: %s %s restarted (restart %u since boot)", bdf(b), b->path,
                    b->restarts);
            continue;
        }
        if (b->kind == BIND_USB && b->last == ERR_SHOULD_WAIT) {
            b->console_wait = true;   /* the console is restarting: DEVMGR_SET_CONSOLE */
            b->restart_at = DEADLINE_NEVER;
            continue;
        }
        if (b->kind == BIND_USB && b->last == ERR_PEER_CLOSED) {
            usb_retire(b, "device gone before its restart");
            continue;
        }
        if (b->kind == BIND_FS && b->last == ERR_PEER_CLOSED) {
            say(false, "devmgr: %s %s: its disk went before its restart", bdf(b), b->path);
            fs_retire(b);
            continue;
        }
        b->console_wait = false;
        char why[48];
        snprintf(why, sizeof(why), "could not be restarted (%s)", status_str(b->last));
        if (!excused(b))
            problems++;
        schedule(b, why, excused(b));
    }
    spare_due();   /* after the restarts: a promotion is what makes it due */
}

void sup_reset(struct binding *b)
{
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
    for (unsigned i = 0; i < SUP_RESTART_LIMIT; i++)
        b->restarted[i] = 0;
    b->restart_at = 0;
    b->backoff_ms = 0;
    if (b->state == DEVMGR_SUP_RESTARTING)
        b->state = DEVMGR_SUP_NONE;
}
