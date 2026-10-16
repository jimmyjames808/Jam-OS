/* ps: every process with its job, threads, state, CPU time and job memory,
 * indented by job, and "[jit]" after the name of one that has mapped JIT
 * memory to run (PROCESS_FLAG_JIT: memory it may write and run, its list's `right
 * jit`, docs/CR-PLAN.md section 12); -k the kernel's own listing, into the
 * kernel log. */
#include "sh.h"

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
    int jit = 0;
    for (int i = 0; i < n; i++) {
        char t[24], m[24];
        bool marked = p[i].flags & PROCESS_FLAG_JIT;
        jit += marked;
        sh_fmt_cpu_time(p[i].cpu_ns, t, sizeof(t));
        sh_say("%5lu %5lu  %3u  %-5s  %10s %8s  %*s%s%s\n", (unsigned long)p[i].koid,
               (unsigned long)p[i].job_koid, p[i].threads, state_name(p[i].state), t,
               sh_human(p[i].job_pages * 4096, m, sizeof(m)), (int)(p[i].depth * 2), "",
               p[i].name, marked ? " [jit]" : "");
    }
    sh_say("%d process%s\n", n, n == 1 ? "" : "es");
    if (jit)
        sh_say("[jit]: writes and runs its own code (JIT memory)\n");
    free(p);
    return 0;
}
