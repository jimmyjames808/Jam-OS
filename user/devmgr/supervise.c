/* devmgr: driver supervision (M7). When a driver's process terminates:
 *
 *   - exit code 0 by itself (not killed): the driver is finished (a
 *     one-shot driver like xhci-noop). Not restarted; its job must be
 *     empty.
 *   - anything else -- a crash (the kernel killed it), a kill by anyone
 *     (DEVMGR_KILL included), an exit with an error: restarted. The rest
 *     of its job is killed first. Backoff: 100 ms after the first death,
 *     doubling with every restart in the last 60 s, at most 5 s. The death
 *     after 5 restarts within 60 s gives up: no more restarts, a log line
 *     and a RESULTS line. A real driver's crash, error exit or give-up is
 *     a problem (devmgr exits 1 at the end); a DEVMGR_KILL, and anything
 *     the crash-test driver does, is expected.
 *
 * The restart itself is a start from scratch (bind.c: new dma_cap, so Bus
 * Master Enable stays off until the new driver has quiesced the device;
 * new interrupt object; the function woken to D0). As soon as the driver
 * has died, devmgr makes the channel the restarted driver will serve and
 * GET_SERVICE hands it out: clients that saw ERR_PEER_CLOSED reconnect at
 * once and their calls wait in the channel until the new driver reads
 * them (the reconnect rule, <devmgr.h>). */
#include "internal.h"

static uint64_t now(void)
{
    return (uint64_t)jam_clock_get();
}

/* Restarts within the window. */
static unsigned recent(const struct binding *b, uint64_t t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SUP_RESTART_LIMIT; i++)
        n += b->restarted[i] && t - b->restarted[i] < SUP_WINDOW;
    return n;
}

/* b's driver is gone (or its restart failed): schedule the next start, or
 * give up. `why` says what happened; `expected`: not a problem. */
static void schedule(struct binding *b, const char *why, bool expected)
{
    uint64_t t = now();
    unsigned n = recent(b, t);
    close_client(b);   /* the dead driver's channel */
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
    if (n >= SUP_RESTART_LIMIT) {
        b->state = DEVMGR_SUP_GAVE_UP;
        say(true, "devmgr: %s %s %s after %u restarts in %lu s: giving up%s", bdf(b), b->path,
            why, n, (unsigned long)(SUP_WINDOW / S),
            b->test ? " (the crash-test driver: expected)" : "");
        if (!b->test)
            problems++;
        return;
    }
    uint64_t delay = SUP_BACKOFF_FIRST << n;
    if (delay > SUP_BACKOFF_MAX)
        delay = SUP_BACKOFF_MAX;
    b->backoff_ms = (uint32_t)(delay / MS);
    b->restart_at = t + delay;
    b->state = DEVMGR_SUP_RESTARTING;
    /* The channel the restart will serve, handed out from now on. (If this
     * fails, start_driver makes one and GET_SERVICE says ERR_BAD_STATE
     * meanwhile.) */
    if (b->kind != BIND_USB && jam_channel_create(&b->client, &b->serve) != OK)
        b->client = b->serve = HANDLE_INVALID;
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
    if (b->kind == BIND_USB) {
        /* A USB class driver (usb.c): its interface gone = the end of it,
         * however it ended; exit 0 because the console went = reconnect. */
        if (usb_gone(b)) {
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
    bool expected = b->killed || b->test;
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
    uint64_t next = DEADLINE_NEVER;
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
        if (!reconnect) {
            b->restarted[b->restarts % SUP_RESTART_LIMIT] = t;
            b->restarts++;
        }
        b->last = start_driver(b);
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
        b->console_wait = false;
        char why[48];
        snprintf(why, sizeof(why), "could not be restarted (%s)", status_str(b->last));
        if (!b->test)
            problems++;
        schedule(b, why, b->test);
    }
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
