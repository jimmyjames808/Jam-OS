/* linuxbench: the order of the run, as Jam OS's bench orders its lines
 * (one CPU, then across CPUs, then the system and ring 3), then the
 * per-operation lines when --dir names a directory on a stick.
 *
 *     linuxbench [--dir DIR] [--out FILE] [--as-is]
 *
 * --dir    where the per-operation lines write their scratch file (the
 *          SanDisk's mount point); without it they are left out
 * --out    the output file; by default linuxbench-<boot>.txt in DIR (or
 *          here), <boot> "default" or "mitigations-off" from the kernel's
 *          command line, never overwriting one already there
 * --as-is  leave the cpufreq governor and energy preference as they are
 * Run it as root: SCHED_FIFO and the governor need it. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *calibrate_thread(void *arg)
{
    (void)arg;
    calibrate();
    return NULL;
}

/* linuxbench-<boot>.txt in dir, or -2, -3, ... if taken. */
static void default_out(const char *dir, char *out, size_t n)
{
    for (int i = 1; i < 100; i++) {
        char suffix[8] = "";
        if (i > 1)
            snprintf(suffix, sizeof(suffix), "-%d", i);
        snprintf(out, n, "%s/linuxbench-%s%s.txt", dir, sys_boot_tag(), suffix);
        if (access(out, F_OK) != 0)
            return;
    }
}

static int usage(void)
{
    fprintf(stderr, "usage: linuxbench [--dir DIR] [--out FILE] [--as-is]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *outp = NULL;
    bool as_is = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            outp = argv[++i];
        else if (!strcmp(argv[i], "--as-is"))
            as_is = true;
        else
            return usage();
    }
    char outbuf[4096];
    if (!outp) {
        default_out(dir ? dir : ".", outbuf, sizeof(outbuf));
        outp = outbuf;
    }
    if (!out_open(outp)) {
        perror(outp);
        return 1;
    }
    if (geteuid() != 0)
        say("bench: WARNING: not root: no SCHED_FIFO, no governor change; run it with sudo\n");

    pick_cpus();
    /* The orchestrating thread stays on CPU 0, away from every measured
     * CPU (as bench's does), at the normal priority. */
    cpu_set_t zero;
    CPU_ZERO(&zero);
    CPU_SET(0, &zero);
    sched_setaffinity(0, sizeof(zero), &zero);
    run_on(cpu_p, calibrate_thread, NULL);

    sys_header();
    sys_performance(as_is);
    say("bench: median and p99 of %d samples (%d for the sleeps, %d for the stick's lines), "
        "every thread pinned, SCHED_FIFO %d; output also in %s\n", SAMPLES, TIMER_SAMPLES,
        DEV_SAMPLES, FIFO_PRIO, outp);
    fprintf(stderr, "linuxbench: running (about a minute); nothing is printed while measuring\n");

    bench_local();
    bench_wake_same();
    if (ncpus_online > 1)
        bench_cross();
    bench_sleep();
    bench_tlb();
    bench_user();
    bench_ipc();
    if (dir)
        bench_files(dir);
    else
        say("bench: per-operation lines: left out (no --dir)\n");

    say("bench: SCHED_FIFO %d for every benchmark thread: %s\n", FIFO_PRIO,
        fifo_ok ? "yes" : "NO (not root?): these numbers don't compare");
    say("bench: done\n");
    out_close();
    fprintf(stderr, "linuxbench: done; the lines are in %s\n", outp);
    return 0;
}
