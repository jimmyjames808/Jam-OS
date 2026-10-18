/* ps: every process with its job, threads, state, CPU time and job memory,
 * indented by job, and "[jit]" after the name of one that has mapped JIT
 * memory to run (PROCESS_FLAG_JIT: memory it may write and run, its list's `right
 * jit`, docs/CR-PLAN.md section 12); -k the kernel's own listing, into the
 * kernel log; -t <pid|name> where that process's threads wait (the
 * kernel's "threads" listing, into the kernel log too: each thread's state,
 * what it waits for, its kernel frames and a user backtrace, which
 * tools/symbolize-threads.py names on the host). */
#include "sh.h"

static const char *state_name(uint32_t s)
{
    static const char *const names[] = { "new", "run", "dying", "dead" };
    return s < 4 ? names[s] : "?";
}

static bool digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

SH_CMD(ps)
{
    if (argc > 1 && !strcmp(argv[1], "-k")) {
        sh_kcmd("ps");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-t")) {
        char cmd[64];
        if (argc < 3 || argc > 4 || !argv[2][0] || strchr(argv[2], ' ') ||
            (argc == 4 && !digits(argv[3])) ||
            snprintf(cmd, sizeof(cmd), "threads %s%s%s", argv[2], argc == 4 ? " " : "",
                     argc == 4 ? argv[3] : "") >= (int)sizeof(cmd)) {
            sh_tty("usage: ps -t <pid|name> [stack words to scan, 1..4096]\n");
            return 2;
        }
        return sh_kcmd(cmd) < 0;
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
