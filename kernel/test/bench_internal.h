/* What the benchmark's files (bench.c, bench_path.c) and the path tests
 * (test_pathstat.c) share. The method is in bench.c's header. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/pathstat.h>

#define PRIO_BENCH 24   /* every benchmark thread, kernel or user, runs at this */

/* The CPUs the benchmark measures on (picked once per boot): P, a P-core
 * away from CPU 0, and P2, another P-core (-1 when there is none). */
void bench_cpus(int *p, int *p2);
/* TSC cycles to picoseconds. */
uint64_t bench_cycles_to_ps(uint64_t c);
/* Run "utest bench-<what>" pinned to cpu (with a bench-echo server pinned
 * to server_cpu for "call"; -1: none; "rwcall" is bench-call against
 * bench-rwecho, the server on channel_reply_wait) and wait for its result. Without
 * `trace` its samples go into the benchmark's sample buffer; with it the
 * caller has path_begin'd a trace, which gets the client process as its
 * lead and the server as its second member and is armed once both run
 * (the caller ends it), and the samples are dropped. False (after
 * reporting why under `label`) if there was no result. */
bool bench_user_run(const char *what, int cpu, int server_cpu, const char *label, bool trace);

/* The path breakdowns (bench_path.c): counts per call and the timeline
 * of a call for the switch, the kernel channel_call and the ring-3
 * calls. Printed with kprintf ("path:" lines) plus a summary line per
 * case in the RESULTS box. */
void bench_path_run(void);

/* One case each, for the tests: run it on `cpu` and fill *out (false if
 * it could not run: no trace free, no memory, no bin/utest). The stamps
 * in *out stay valid until the next trace starts. */
bool bench_path_switch(int cpu, uint64_t marked, struct path_result *out);
bool bench_path_kcall(int cpu, uint64_t marked, struct path_result *out);
bool bench_path_ucall(const char *what, int cpu, int server_cpu, uint64_t marked,
                      struct path_result *out);

/* The usual shape of a traced call: the steps of the round trips that
 * look like most of the others, with each step's median. */
#define PATH_STEPS_MAX 64
struct path_shape {
    uint32_t trips;                        /* round trips with this shape */
    uint32_t total;                        /* round trips seen */
    uint32_t nsteps;                       /* steps: one per stamp, the next boundary closes */
    struct path_stamp first[PATH_STEPS_MAX + 1];   /* one trip's stamps (marks, who, args) */
    uint64_t median_cycles[PATH_STEPS_MAX];        /* each step's median, TSC cycles */
};
/* Find it in r (stamps of a trace whose boundary is `boundary`); false
 * if there is no complete round trip. */
bool path_shape_of(const struct path_result *r, enum path_mark boundary,
                   struct path_shape *out);
