/* mem: physical memory from the kernel (its report goes to the log), and
 * what the shell's job uses. */
#include "../sh.h"

SH_CMD(mem)
{
    (void)argc;
    (void)argv;
    int64_t free_mib = sh_kcmd("mem");
    struct job_info ji;
    if (jam_job_get_info(startup_handle(SR_JOB), &ji) == OK)
        sh_say("shell's job: %lu pages, %lu handles, %lu threads, %lu message bytes\n",
               (unsigned long)ji.used[JOB_LIMIT_PAGES], (unsigned long)ji.used[JOB_LIMIT_HANDLES],
               (unsigned long)ji.used[JOB_LIMIT_THREADS],
               (unsigned long)ji.used[JOB_LIMIT_MSG_BYTES]);
    if (free_mib >= 0)
        sh_say("mem: %ld MiB free\n", (long)free_mib);
    return 0;
}
