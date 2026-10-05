/* jobs: the programs started with & (sh_jobs.c): number, pid, state and
 * the command. One that ended is listed once more as done (or killed) and
 * then forgotten, as the prompt's notice would have. */
#include "sh.h"

SH_CMD(jobs)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: jobs\n");
        return 2;
    }
    sh_jobs_poll(false);   /* the ends up to now */
    for (unsigned i = 0; i < SH_MAX_JOBS; i++) {
        const struct sh_job *j = sh_job_at(i);
        if (!j)
            continue;
        char state[24];
        if (!j->ended)
            snprintf(state, sizeof(state), "running");
        else if (j->killed)
            snprintf(state, sizeof(state), "killed");
        else
            snprintf(state, sizeof(state), "done (exit %ld)", (long)j->code);
        sh_say("[%u] %6lu  %-15s %s%s%s\n", j->n, (unsigned long)j->pid, state, j->name,
               j->args[0] ? " " : "", j->args);
        if (j->ended)
            sh_job_forget(j->n);
    }
    return 0;
}
