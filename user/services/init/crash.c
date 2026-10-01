/* init in a crash kernel's boot (argv[1] "crash"): save the log of the
 * kernel that panicked, then end, so the kernel can show where it went
 * (kernel/kexec/crashlog.c draws the last screen when init has ended).
 *
 * The kernel hands init the crashed kernel's log as SR_CRASHLOG (a
 * read-only VMO). init starts devmgr with "storage": the PCI drivers as on
 * every boot, so each controller the dead kernel left running is reset by
 * its driver, but of the USB interfaces only mass storage (no keyboard:
 * there is nothing to type to). It waits for the stick's /data (at most
 * DATA_WAIT_S: a stick that enumerates slowly, or retries a port, still
 * makes it), runs `bin/logd crash` with the log and /data, and stops
 * devmgr, which stops fat cleanly, so the volume is left marked clean.
 * No console, no shell, no splash, no sound: the kernel's own log is on
 * the screen. Every line that matters goes into the RESULTS box. */
#include <os.h>
#include "init.h"

#define DATA_WAIT_S 30
#define LOGD_WAIT_S 60
#define POLL_MS     250

static const char *const data_only[] = { DATA_MOUNT, NULL };

/* Is /data in our namespace yet? (mounts.c puts devmgr's mounts there.) */
static bool have_data(void)
{
    handle_t h;
    if (ns_channel(DATA_MOUNT, &h) != OK)
        return false;
    jam_handle_close(h);
    return true;
}

static bool wait_for_data(void)
{
    uint64_t deadline = now() + DATA_WAIT_S * NS_PER_S;
    while (!have_data()) {
        if (now() >= deadline)
            return false;
        jam_nanosleep(now() + POLL_MS * NS_PER_MS);
    }
    return true;
}

/* bin/logd crash, in a job of its own, with the log and /data. */
static bool run_logd(handle_t log)
{
    handle_t job, proc, dup;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st == OK)
        st = jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &dup);
    if (st != OK) {
        init_say("crash: the log was NOT saved: no job or handle for logd (%s)", status_str(st));
        return false;
    }
    const char *argv[] = { "bin/logd", "crash" };
    struct spawn_handle x = { SR_CRASHLOG, dup };
    struct spawn_args a = {
        .path = "bin/logd", .argc = 2, .argv = argv, .job = job, .extra = &x, .nextra = 1,
        .ns = data_only,
    };
    st = spawn(&a, &proc);   /* consumes dup */
    struct process_info info = { 0 };
    if (st == OK) {
        st = spawn_wait(proc, LOGD_WAIT_S * NS_PER_S, &info);
        if (st == ERR_TIMED_OUT) {
            init_say("crash: logd still writing after %d s: killed", LOGD_WAIT_S);
            jam_job_kill(job);
        }
        jam_handle_close(proc);
    } else {
        init_say("crash: the log was NOT saved: logd didn't start (%s)", status_str(st));
    }
    jam_handle_close(job);
    /* logd itself says where the log went, or why not. */
    return st == OK && !info.killed && info.exit_code == 0;
}

int init_crash(void)
{
    handle_t log = startup_handle(SR_CRASHLOG);
    if (!log) {
        init_say("crash: the log was NOT saved: the kernel had no log of the crashed kernel "
                 "to give (its lines above say why)");
        return 1;
    }
    bool ok = init_start_devmgr(HANDLE_INVALID, "storage");
    if (wait_for_data()) {
        ok &= run_logd(log);
    } else {
        init_say("crash: the log was NOT saved: no /data after %d s (is the Jam OS stick "
                 "plugged in?)", DATA_WAIT_S);
        ok = false;
    }
    ok &= init_stop_devmgr();
    return ok ? 0 : 1;
}
