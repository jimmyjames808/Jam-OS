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
 *     the crash-test driver does, is expected. (The filesystem services of
 *     the disks are volumes', supervised there by the rule of a service
 *     that outlives its process: user/services/volumes/supervise.c.)
 *
 * The restart itself is a start from scratch (bind.c: new dma_cap, so Bus
 * Master Enable stays off until the new driver has quiesced the device;
 * new interrupt object; the function woken to D0). As soon as the driver
 * has died, devmgr makes the channel the restarted driver will serve and
 * GET_SERVICE hands it out: clients that saw ERR_PEER_CLOSED reconnect at
 * once and their calls wait in the channel until the new driver reads
 * them (the reconnect rule, <devmgr.h>).
 *
 * On the desktop (devmgr_notices, <notice.h>) a real driver's first crash
 * in its window and its give-up are notices in plain words ("Sound
 * driver crashed / Jam OS started it again"); a deliberate kill, the
 * crash-test driver and anything at a shutdown are not. The log lines are
 * as they were. */
#include "internal.h"

/* b ending is never a problem of ours: a test's. */
static bool excused(const struct binding *b)
{
    return b->test;
}

/* What a user calls b's driver, for a notice ("Sound driver"). */
static void plain_name(const struct binding *b, char *buf, size_t n)
{
    static const struct { const char *path, *name; } names[] = {
        { "drv/hda", "Sound driver" },          { "drv/usb-bus", "USB driver" },
        { "drv/hid", "Keyboard and mouse driver" }, { "drv/usb-storage", "USB stick driver" },
        { "drv/rtl8125", "Network driver" },    { "drv/e1000e", "Network driver" },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (b->path && !strcmp(b->path, names[i].path)) {
            snprintf(buf, n, "%s", names[i].name);
            return;
        }
    snprintf(buf, n, "Driver %s", b->path ? b->path : "?");
}

/* A notice of b's crash (first: the first in its window) or give-up, if
 * it is news to a user. */
static void tell_desktop(const struct binding *b, bool gave_up, bool first)
{
    char title[64];
    if (excused(b) || shutdown_asked || (!gave_up && !first))
        return;
    plain_name(b, title, sizeof(title));
    size_t n = strlen(title);
    snprintf(title + n, sizeof(title) - n, gave_up ? " stopped" : " crashed");
    notice_post(&devmgr_notices, title,
                gave_up ? "It kept crashing; Jam OS gave up."
                        : "Jam OS started it again.",
                '!', NOTICE_RASPBERRY);
}

/* The dead driver's channel goes. */
static void drop_channel(struct binding *b)
{
    close_client(b);
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
}

/* b's driver is gone (or its restart failed): schedule the next start, or
 * give up. `why` says what happened; `expected`: not a problem. */
static void schedule(struct binding *b, const char *why, bool expected)
{
    uint64_t t = now();
    unsigned n = sup_window_recent(&b->win, t);
    uint64_t at;   /* when the restart is due */
    drop_channel(b);
    if (!sup_window_end(&b->win, t, b->killed ? SUP_END_ON_PURPOSE : SUP_END_COUNTED, false,
                        &at)) {
        b->state = DEVMGR_SUP_GAVE_UP;
        say(!excused(b) || b->test, "devmgr: %s %s %s after %u restarts in %lu s: giving up%s",
            bdf(b), b->path, why, n, (unsigned long)(SUP_WINDOW / NS_PER_S),
            b->test ? " (the crash-test driver: expected)" : "");
        if (!excused(b))
            problems++;
        tell_desktop(b, true, false);
        return;
    }
    b->restart_at = at;
    b->state = DEVMGR_SUP_RESTARTING;
    /* The channel the restart will serve, handed out from now on. (If this
     * fails, start_driver makes one and GET_SERVICE says ERR_BAD_STATE
     * meanwhile.) */
    if (b->kind != BIND_USB && !b->client && jam_channel_create(&b->client, &b->serve) != OK)
        b->client = b->serve = HANDLE_INVALID;
    say(!expected, "devmgr: %s %s %s: restart %u in %u ms", bdf(b), b->path, why, n + 1,
        b->win.backoff_ms);
    if (!expected)
        tell_desktop(b, false, n == 0);
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
         * however it ended; exit 0 because the compositor went = reconnect. */
        if (usb_gone(b)) {
            /* A disk's filesystem services (volumes') map buffers its
             * driver made, which stay charged to the driver's job until
             * volumes has killed them too: sup_job_empty looks again for a
             * while before it calls the job not clean. */
            bool clean = sup_job_empty(b->job, b->path);
            forget_driver(b);
            problems += !clean;
            usb_retire(b, clean ? "ended, device gone" : "device gone, did not end cleanly");
            return;
        }
        if (!info.killed && info.exit_code == 0 && !b->killed && usb_input_gone(b)) {
            forget_driver(b);
            b->state = DEVMGR_SUP_RESTARTING;
            b->input_wait = true;
            /* Try at once: if the compositor is still down, usb_handles says
             * ERR_SHOULD_WAIT and sup_run_due parks it for set_input. (A
             * set_input may well have come before this death.) */
            b->restart_at = now();
            b->input_gen = 0;
            say(false, "devmgr: %s %s: its %s went away: reconnecting", bdf(b), b->path,
                usb_hub_name());
            return;
        }
    }
    if (!info.killed && info.exit_code == 0 && !b->killed) {
        bool clean = sup_job_empty(b->job, b->path);
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
        bool reconnect = b->input_wait;   /* not a restart: the compositor came back */
        if (!reconnect)
            sup_window_started(&b->win, t, true);
        b->killed = false;   /* if this start fails, that is no deliberate kill */
        b->last = start_driver(b);
        if (b->last == OK) {
            b->input_wait = false;
            if (reconnect)
                say(false, "devmgr: %s %s started again (the %s is back)", bdf(b), b->path,
                    usb_hub_name());
            else
                say(false, "devmgr: %s %s restarted (restart %u since boot)", bdf(b), b->path,
                    b->win.restarts);
            continue;
        }
        if (b->kind == BIND_USB && b->last == ERR_SHOULD_WAIT) {
            b->input_wait = true;   /* the compositor is restarting: devmgrctl.set_input */
            b->restart_at = DEADLINE_NEVER;
            continue;
        }
        if (b->kind == BIND_USB && b->last == ERR_PEER_CLOSED) {
            usb_retire(b, "device gone before its restart");
            continue;
        }
        b->input_wait = false;
        char why[48];
        snprintf(why, sizeof(why), "could not be restarted (%s)", status_str(b->last));
        if (!excused(b))
            problems++;
        schedule(b, why, excused(b));
    }
}

void sup_reset(struct binding *b)
{
    if (b->serve)
        jam_handle_close(b->serve);
    b->serve = HANDLE_INVALID;
    sup_window_reset(&b->win);
    b->restart_at = 0;
    if (b->state == DEVMGR_SUP_RESTARTING)
        b->state = DEVMGR_SUP_NONE;
}
