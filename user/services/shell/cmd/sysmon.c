/* sysmon: bin/sysmon, the graphical system monitor: CPU use per CPU,
 * memory, context switches and the busiest processes, on the screen it
 * borrows from the console, until q.
 *
 * The figures come from sys_info, cpu_stat and proc_list, which need
 * RIGHT_READ on the root resource. A program `run` starts has no such
 * handle, so this command starts bin/sysmon itself and gives it one: a
 * duplicate of the shell's root with RIGHT_READ only (no RIGHT_MANAGE:
 * no debug commands, no reboot), which arrives without RIGHT_DUPLICATE
 * and RIGHT_TRANSFER, so the program can't pass it on. Like any program
 * it gets a PROGRAM-level console channel and a job of its own, which is
 * killed when it ends or on Ctrl+C. */
#include <idl/console.h>
#include "sh.h"

#define SYSMON_PATH "bin/sysmon"

/* The handles it starts with: its console channel and the root resource
 * to read. They are moved into x (spawn consumes them). */
static status_t sysmon_handles(struct spawn_handle x[2])
{
    handle_t con, res;
    status_t st = console_new_client_until(sh_console(), now() + 5 * NS_PER_S, 2, &con);
    if (st != OK)
        return st;
    st = jam_handle_duplicate(sh_root(), RIGHTS_BASIC | RIGHT_READ, &res);
    if (st != OK) {
        jam_handle_close(con);
        return st;
    }
    x[0] = (struct spawn_handle){ SR_CONSOLE, con };
    x[1] = (struct spawn_handle){ SR_RESOURCE, res };
    return OK;
}

/* Wait for it to end; Ctrl+C kills its job. */
static status_t sysmon_wait(handle_t proc, handle_t job, struct process_info *info)
{
    status_t st;
    bool killed = false;
    while ((st = spawn_wait(proc, 50 * NS_PER_MS, info)) == ERR_TIMED_OUT)
        if (sh_interrupted() && !killed) {
            sh_tty("^C: killing " SYSMON_PATH "\n");
            sh_flush();
            jam_job_kill(job);
            killed = true;
        }
    return st;
}

SH_CMD(sysmon)
{
    /* What the child's copies keep: the console channel as it is, the
     * resource readable but neither duplicable nor transferable. */
    static const rights_t rights[2] = { RIGHT_SAME, RIGHT_READ | RIGHT_WAIT | RIGHT_INSPECT };
    struct spawn_handle x[2];
    handle_t job, proc;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st == OK && (st = sysmon_handles(x)) != OK)
        jam_handle_close(job);
    if (st != OK) {
        sh_tty("sysmon: can't set it up (%s)\n", status_str(st));
        return 126;
    }
    const char *args[20] = { SYSMON_PATH };
    int n = 1;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    struct spawn_args a = {
        .path = SYSMON_PATH, .argc = n, .argv = args, .job = job, .extra = x, .nextra = 2,
        .extra_rights = rights,
    };
    st = spawn(&a, &proc);
    if (st != OK) {
        sh_tty("sysmon: can't start " SYSMON_PATH " (%s)\n", status_str(st));
        jam_handle_close(job);
        return 126;
    }
    sh_tty("sysmon: " SYSMON_PATH " started (q quits it)\n");
    sh_flush();
    struct process_info info = { 0 };
    st = sysmon_wait(proc, job, &info);
    jam_job_kill(job);   /* anything it left running goes with it */
    jam_handle_close(proc);
    jam_handle_close(job);
    if (st != OK || info.killed) {
        sh_tty("sysmon: " SYSMON_PATH " was killed\n");
        return 137;
    }
    sh_tty("sysmon: " SYSMON_PATH " exited with code %ld\n", (long)info.exit_code);
    return info.exit_code < 0 || info.exit_code > 255 ? 1 : (int)info.exit_code;
}
