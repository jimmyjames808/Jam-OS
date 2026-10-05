/* kill: kill a process by name, through init (abi/idl/initctl.idl): the
 * services init runs and the drivers devmgr runs (PCI drivers by their
 * name, "hda"; USB class drivers, "hid-6.1:0"). Both are started
 * again by whoever supervises them. `kill %2` (or the pid `jobs` shows)
 * ends one of this shell's background programs instead (sh_jobs.c): the
 * shell holds their jobs itself. */
#include <idl/initctl.h>
#include "sh.h"

#define KILL_WAIT (20 * NS_PER_S)   /* devmgr waits for a driver to die */

/* Background program n ended by kill: said, its status. */
static int kill_job(unsigned n, const char *arg)
{
    const struct sh_job *j = n ? sh_job_at(n - 1) : NULL;
    status_t st = sh_job_kill(n);
    if (st == OK) {
        sh_job_say_end(j);
        sh_job_forget(n);
        return 0;
    }
    if (st == ERR_NOT_FOUND) {
        sh_tty("kill: no background program %s (jobs lists them)\n", arg);
    } else if (st == ERR_BAD_STATE) {
        sh_job_say_end(j);   /* its notice, now rather than at the prompt */
        sh_job_forget(n);
        sh_tty("kill: %s had already ended\n", arg);
    } else {
        sh_tty("kill %s: %s\n", arg, status_str(st));
    }
    return 1;
}

SH_CMD(kill)
{
    if (argc != 2) {
        sh_tty("usage: kill <name> | %%<n> | <pid of a background program>\n");
        return 2;
    }
    uint64_t v;
    if (argv[1][0] == '%') {
        bool ok = sh_parse_u64(argv[1] + 1, &v) && v >= 1 && v <= SH_MAX_JOBS;
        return kill_job(ok ? (unsigned)v : 0, argv[1]);
    }
    if (sh_parse_u64(argv[1], &v) && sh_job_by_pid(v))
        return kill_job(sh_job_by_pid(v), argv[1]);
    if (!strcmp(argv[1], "init")) {
        /* Nobody restarts init: it supervises everything else (killed, the
         * kernel prints its RESULTS box while the rest runs on
         * unsupervised). devmgr may go: init starts it again, with its
         * drivers. */
        sh_tty("kill: %s is not restarted by anyone: not killing it\n", argv[1]);
        return 1;
    }
    uint8_t name[32] = { 0 };
    uint64_t koid = 0;
    status_t st = strlen(argv[1]) < sizeof(name) ? OK : ERR_INVALID_ARGS;
    if (st == OK && !sh_initctl())
        st = ERR_BAD_HANDLE;   /* a shell init didn't start */
    if (st == OK) {
        memcpy(name, argv[1], strlen(argv[1]));
        sh_flush();
        st = initctl_kill_until(sh_initctl(), now() + KILL_WAIT, name, &koid);
    }
    if (st == OK) {
        sh_say("shell: killed process %lu (%s)\n", (unsigned long)koid, argv[1]);
        return 0;
    }
    if (st == ERR_NOT_FOUND)
        sh_tty("kill: no process called \"%s\"\n", argv[1]);
    else
        sh_tty("kill %s: %s\n", argv[1], status_str(st));
    return 1;
}
