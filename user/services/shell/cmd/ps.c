/* ps: every process with its job, threads, state, CPU time and job memory,
 * indented by job; -k the kernel's own listing, into the kernel log. */
#include "../sh.h"

static const char *state_name(uint32_t s)
{
    static const char *const names[] = { "new", "run", "dying", "dead" };
    return s < 4 ? names[s] : "?";
}

SH_CMD(ps)
{
    if (argc > 1 && !strcmp(argv[1], "-k")) {
        sh_kcmd("ps");
        return 0;
    }
    struct proc_stat *p = calloc(SH_MAX_PROCS, sizeof(*p));
    if (!p)
        return 1;
    int n = sh_procs(p, "ps");
    if (n < 0) {
        free(p);
        return 1;
    }
    sh_say("  PID   JOB  THR  STATE    CPU TIME  JOB MEM  NAME\n");
    for (int i = 0; i < n; i++) {
        char t[24], m[24];
        sh_fmt_cpu_time(p[i].cpu_ns, t, sizeof(t));
        sh_say("%5lu %5lu  %3u  %-5s  %10s %8s  %*s%s\n", (unsigned long)p[i].koid,
               (unsigned long)p[i].job_koid, p[i].threads, state_name(p[i].state), t,
               sh_human(p[i].job_pages * 4096, m, sizeof(m)), (int)(p[i].depth * 2), "",
               p[i].name);
    }
    sh_say("%d process%s\n", n, n == 1 ? "" : "es");
    free(p);
    return 0;
}
