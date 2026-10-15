/* The named AF_UNIX socket broker, bin/unix (abi/idl/unix.idl; docs/CR-PLAN.md
 * section 4, track CU2): init makes its shared channel once, keeps both
 * ends and publishes the client end as /svc/unix (a channel per opener),
 * so a restarted broker serves the same channel. The broker gets the
 * server end (UNIX_SR_CTL) and nothing else: no namespace, no root.
 *
 * Shell mode: a supervised service like the file server (services.c starts
 * it through unixsvc_start). The init run: started before the programs
 * (unixsvc_run_start) and stopped after them (unixsvc_run_stop: the name
 * taken away and init's ends closed, it exits by itself). */
#include <os.h>
#include <roles.h>
#include "init.h"

#define UNIX_PATH "bin/unix"

static handle_t unix_srv, unix_cli;    /* the shared channel's ends (0: none) */
static handle_t run_proc, run_job;     /* the init run's broker (0: none) */

void unixsvc_init(void)
{
    if (jam_channel_create(&unix_cli, &unix_srv) != OK) {
        unix_cli = unix_srv = HANDLE_INVALID;
        return;
    }
    kept_end_mark(unix_srv);
}

handle_t unixsvc_client(void)
{
    return unix_cli;
}

/* The server end for a new broker (a duplicate), or HANDLE_INVALID when
 * there is none or no bin/unix to start. */
static handle_t server_end(void)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    handle_t d = HANDLE_INVALID;
    if (!unix_srv || bootfs_default(&fs) != OK ||
        bootfs_lookup(fs, UNIX_PATH, &data, &size) != OK ||
        jam_handle_duplicate(unix_srv, RIGHT_SAME, &d) != OK) {
        printf("init: no %s (or no channel for it): no named sockets\n", UNIX_PATH);
        return HANDLE_INVALID;
    }
    return d;
}

status_t unixsvc_start(unsigned i)
{
    struct spawn_handle x[] = { { UNIX_SR_CTL, server_end() } };
    if (!x[0].h) {
        svcs[i].given_up = true;
        return OK;
    }
    return svc_start1(i, x, 1);
}

void unixsvc_given_up(void)
{
    if (unix_srv)
        jam_handle_close(unix_srv);   /* calls waiting for a broker fail now */
    unix_srv = HANDLE_INVALID;
}

bool unixsvc_run_start(void)
{
    unixsvc_init();
    struct spawn_handle x[] = { { UNIX_SR_CTL, server_end() } };
    if (!x[0].h)
        return true;   /* no broker: posixtest's unix section says so and skips */
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &run_job);
    if (st == OK) {
        const char *argv[] = { UNIX_PATH };
        struct spawn_args a = { .path = UNIX_PATH, .argc = 1, .argv = argv, .job = run_job,
                                .extra = x, .nextra = 1 };
        st = spawn(&a, &run_proc);   /* consumes the server end */
        x[0].h = HANDLE_INVALID;
    }
    if (st == OK) {
        handle_t d = HANDLE_INVALID;
        if (jam_handle_duplicate(unix_cli, RIGHT_SAME, &d) == OK)
            st = ns_svc_set(SVC_UNIX, d, true);   /* the programs reach it through our namespace */
    }
    if (st != OK) {
        init_say("init: can't start %s (%s)", UNIX_PATH, status_str(st));
        if (x[0].h)
            jam_handle_close(x[0].h);
        return false;
    }
    return true;
}

bool unixsvc_run_stop(void)
{
    if (!run_proc)
        return true;
    (void)ns_svc_remove(SVC_UNIX);   /* our copies too */
    jam_handle_close(unix_cli);
    jam_handle_close(unix_srv);
    unix_cli = unix_srv = HANDLE_INVALID;   /* its shared channel's peer gone: it exits */
    struct process_info info;
    status_t st = spawn_wait(run_proc, 10 * NS_PER_S, &info);
    if (st == ERR_TIMED_OUT) {
        init_say("init: %s still running 10 s after its channel closed: killing its job", UNIX_PATH);
        jam_job_kill(run_job);
        st = spawn_wait(run_proc, 10 * NS_PER_S, &info);
    }
    bool ok = st == OK && !info.killed && info.exit_code == 0;
    if (!ok)
        init_say("init: %s ended badly (%s, code %ld)", UNIX_PATH, status_str(st),
                 st == OK ? (long)info.exit_code : 0L);
    struct job_info ji;
    if (jam_job_get_info(run_job, &ji) == OK)
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            if (ji.used[k]) {
                init_say("init: %s's job still has %lu units of kind %u", UNIX_PATH,
                         (unsigned long)ji.used[k], k);
                ok = false;
            }
    jam_handle_close(run_proc);
    jam_handle_close(run_job);
    run_proc = run_job = HANDLE_INVALID;
    return ok;
}
