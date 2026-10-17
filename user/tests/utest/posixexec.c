/* utest: execve in place seen from a native starter, the shell's way
 * (docs/FORK-PLAN.md, track FL3).
 *
 *   exec_native   posixtest, started with libjam's spawn in a job of its
 *                 own with a hand-over (descriptor 1 a pipe to us) and a
 *                 signal channel (SR_SIGNALS), as the shell starts a POSIX
 *                 program, execs itself at once ("posixtest --child-exec
 *                 native"). We see one process throughout: the new image
 *                 says "ready" on the pipe (having checked its pid is the
 *                 one it had), our handle's pid and start time unchanged,
 *                 its two threads the job's only ones; our SIGTERM on the
 *                 signal channel reaches it, and the wait on the handle we
 *                 had sees it end by that signal (the POSIX layer's exit
 *                 code for "ended by signal 15"); the job is then empty. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/signal.h>
#include <os.h>
#include <posixfds.h>
#include "utest.h"

#define WAIT_NS        (20 * NS_PER_S)
#define SIG_TERM       15        /* Linux's SIGTERM */
#define EXIT_BY_SIGNAL 0x10000   /* the POSIX layer's exit code for "ended by signal N" */

bool t_exec_native(void)
{
    handle_t job, proc, pipe_ours, pipe_theirs, sig_ours, sig_theirs;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&pipe_ours, &pipe_theirs), OK);
    CHECK_ST(jam_channel_create(&sig_ours, &sig_theirs), OK);
    struct posixfds_desc d = { .fd = 1, .kind = POSIXFDS_PIPE, .status = 1,
                               .handles = &pipe_theirs };
    struct posixfds_out ho = { .cwd = "/", .fds = &d, .nfds = 1 };
    struct spawn_handle x = { SR_SIGNALS, sig_theirs };
    const char *argv[] = { "posixtest", "--child-exec", "native" };
    struct spawn_args a = { .path = "bin/posixtest", .argc = 3, .argv = argv, .job = job,
                            .extra = &x, .nextra = 1, .handover = &ho,
                            .stack_size = 8ull << 20 };
    CHECK_ST(spawn(&a, &proc), OK);
    struct process_info before, after, end;
    CHECK_ST(jam_process_get_info(proc, &before), OK);

    signals_t seen;
    CHECK_ST(jam_object_wait_one(pipe_ours, SIG_READABLE, now() + WAIT_NS, &seen), OK);
    char line[16] = { 0 };
    uint32_t n = 0, nh = 0;
    struct channel_read_args r = {
        .h = pipe_ours, .rd_cap = sizeof(line) - 1, .rd_bytes = (uint64_t)(uintptr_t)line,
        .rd_actual = (uint64_t)(uintptr_t)&n, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(!strcmp(line, "ready"));
    CHECK_ST(jam_process_get_info(proc, &after), OK);
    CHECK_EQ(after.pid, before.pid);
    CHECK_EQ(after.start_ns, before.start_ns);
    CHECK_EQ(after.state, PROCESS_RUNNING);
    CHECK_EQ(after.threads, 2u);   /* the new image and its signal thread */
    struct job_info ji;
    CHECK_ST(info_of(job, &ji), OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 2u);   /* no second process waiting on it */

    CHECK_ST(signal_send(sig_ours, SIG_TERM, SIGNAL_CODE_USER, 0), OK);
    CHECK_ST(spawn_wait(proc, WAIT_NS, &end), OK);
    CHECK_EQ(end.exit_code, EXIT_BY_SIGNAL + SIG_TERM);
    CHECK_EQ(end.pid, before.pid);
    jam_handle_close(proc);
    jam_handle_close(pipe_ours);
    jam_handle_close(sig_ours);
    uint64_t deadline = now() + 2 * NS_PER_S;
    do
        CHECK_ST(info_of(job, &ji), OK);
    while (ji.used[JOB_LIMIT_THREADS] && now() < deadline &&
           jam_sleep_until(now() + 10 * NS_PER_MS) == OK);
    CHECK_EQ(ji.used[JOB_LIMIT_THREADS], 0u);
    jam_handle_close(job);
    return true;
}
