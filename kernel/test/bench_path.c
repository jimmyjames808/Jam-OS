/* Path breakdowns for the benchmark: what one call costs, piece by piece.
 *
 * For each case a trace (jam/pathstat.h) follows the threads of the call.
 * Over PATH_CALLS_N calls, after PATH_SKIP warm-up calls, it counts every
 * kernel entry, user and kernel copy, kmalloc, job charge, handle-table
 * operation, spinlock, scheduler pass, wake, FPU save and restore and CR3
 * load the call's threads make; for the first `marked` of those calls it
 * also keeps a TSC timestamp at each named point of the path. Counts are
 * exact and mean the same in QEMU as on the PC. The timeline only means
 * something on real hardware, and each of its steps includes the cost of
 * one mark (printed with it): compare steps with each other, and the sum
 * with the untraced line of the benchmark.
 *
 * Cases (P is the benchmark's P-core, P2 another one):
 *   switch  two kernel threads yielding on P; a round trip is two
 *           switches, so the counts are printed per switch
 *   kcall   channel_call between two kernel threads on P
 *   ucall   process->process channel_call, both on P (utest bench-call)
 *   rwcall  the same against a server on channel_reply_wait (bench-rwecho)
 *   dcall   the same with a deadline on every call, as libos's file calls
 *           have (bench-dcall)
 *   gcall   the same through generated code: a null.ping client stub with
 *           a time limit against null_serve (bench-gcall, bench-gecho)
 *   tcall   thread->thread channel_call inside one process on P (bench-tcall)
 *   ucall2  process->process channel_call, client on P, server on P2
 *           (counts only: two CPUs' timestamps interleave)
 *
 * Output: kprintf lines starting "path:" (the log), and one "bench: path"
 * summary line per case in the RESULTS box. */
#include <jam/bootfs.h>
#include <jam/channel.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pathstat.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/syscall_nums.h>
#include <jam/time.h>
#include <jam/x86.h>

#include "bench_internal.h"

#define PATH_SKIP    256           /* calls before counting starts */
#define PATH_CALLS_N 1024          /* calls counted */
#define PATH_MARKED  64            /* of those, calls with a timeline */
#define PATH_RUN_NS  5000000000ull /* a kernel case gives up after this long */

/* The TSC at which a kernel case started now gives up: the loops compare
 * it with rdtsc, not uptime_ns, so the harness adds no clock reads to the
 * counts (PATH_CLOCK is the path's own). */
static uint64_t give_up_tsc(void)
{
    return uptime_to_tsc(uptime_ns() + PATH_RUN_NS);
}

/* ---- the kernel-thread cases ------------------------------------------------ */

static struct thread *spawn_pinned(int cpu, void (*fn)(void *), void *arg)
{
    cpumask_t m;
    cpumask_one(&m, (uint32_t)cpu);
    return thread_create_on("bench path", fn, arg, PRIO_BENCH, &m);
}

static bool sw_stop;   /* atomic: the lead tells the partner to finish */

static void sw_partner(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&sw_stop, __ATOMIC_ACQUIRE))
        thread_yield();
}

static void sw_lead(void *arg)
{
    (void)arg;
    uint64_t until = give_up_tsc();
    while (!path_window_done() && rdtsc() < until)
        thread_yield();
    __atomic_store_n(&sw_stop, true, __ATOMIC_RELEASE);
}

bool bench_path_switch(int cpu, uint64_t marked, struct path_result *out)
{
    if (!path_begin(PATH_MK_YIELD, PATH_SKIP, PATH_CALLS_N, marked))
        return false;
    __atomic_store_n(&sw_stop, false, __ATOMIC_RELAXED);
    struct thread *partner = spawn_pinned(cpu, sw_partner, NULL);
    struct thread *lead = spawn_pinned(cpu, sw_lead, NULL);
    path_add_thread(lead);
    path_add_thread(partner);
    path_arm();
    thread_join(lead);
    thread_join(partner);
    path_end(out);
    return true;
}

static void kc_server(void *arg)
{
    struct channel *ep = arg;
    for (;;) {
        signals_t s = 0;
        object_wait_one((struct kobject *)ep, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                        &s);
        uint64_t m[2];
        uint32_t nb = 0;
        status_t st = channel_read(ep, m, sizeof(m), &nb, NULL, 0, NULL);
        if (st == OK)
            channel_write(ep, m, nb, NULL, 0);
        else if (st != ERR_SHOULD_WAIT)
            return;   /* the client closed its end */
    }
}

static void kc_lead(void *arg)
{
    struct channel *ep = arg;
    uint64_t until = give_up_tsc();
    while (!path_window_done() && rdtsc() < until) {
        uint64_t req[2] = { 0, 42 }, rep[2];
        uint32_t n = 0;
        if (channel_call(ep, req, sizeof(req), NULL, 0, rep, sizeof(rep), &n, NULL, 0, NULL,
                         DEADLINE_NEVER) != OK)
            return;
    }
}

bool bench_path_kcall(int cpu, uint64_t marked, struct path_result *out)
{
    struct channel *a, *b;
    if (channel_create(&a, &b) != OK)
        return false;
    if (!path_begin(PATH_MK_CALL, PATH_SKIP, PATH_CALLS_N, marked)) {
        kobject_unref((struct kobject *)a);
        kobject_unref((struct kobject *)b);
        return false;
    }
    struct thread *srv = spawn_pinned(cpu, kc_server, b);
    struct thread *lead = spawn_pinned(cpu, kc_lead, a);
    path_add_thread(lead);
    path_add_thread(srv);
    path_arm();
    thread_join(lead);
    path_end(out);
    kobject_unref((struct kobject *)a);   /* closes it: the server sees PEER_CLOSED */
    thread_join(srv);
    kobject_unref((struct kobject *)b);
    return true;
}

bool bench_path_ucall(const char *what, int cpu, int server_cpu, uint64_t marked,
                      struct path_result *out)
{
    const void *img;
    uint64_t size;
    if (bootfs_data("bin/utest", &img, &size) != OK)
        return false;
    if (!path_begin(PATH_MK_CALL, PATH_SKIP, PATH_CALLS_N, marked))
        return false;
    bool ok = bench_user_run(what, cpu, server_cpu, "path", true);
    path_end(out);
    return ok;
}

/* ---- the usual shape of a round trip ------------------------------------------ */

/* Indices of r's stamps in time order (stamps from two CPUs can be out of
 * order by a little). Insertion sort: they are nearly sorted already. */
static uint16_t *time_order(const struct path_result *r)
{
    uint16_t *ix = kmalloc(r->nstamps * sizeof(uint16_t) + 1);
    if (!ix)
        return NULL;
    for (uint32_t i = 0; i < r->nstamps; i++) {
        uint32_t j = i;
        for (; j && r->stamps[ix[j - 1]].tsc > r->stamps[i].tsc; j--)
            ix[j] = ix[j - 1];
        ix[j] = (uint16_t)i;
    }
    return ix;
}

static uint64_t trip_key(const struct path_result *r, const uint16_t *ix, uint32_t from,
                         uint32_t to)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a over (mark, who, arg) */
    for (uint32_t i = from; i < to; i++) {
        const struct path_stamp *s = &r->stamps[ix[i]];
        uint64_t v = (uint64_t)s->mark << 40 | (uint64_t)s->who << 32 | s->arg;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

#define TRIPS_MAX 128

/* The round trips: starts[k] is the k-th boundary of the lead in ix. */
static uint32_t find_trips(const struct path_result *r, const uint16_t *ix,
                           enum path_mark boundary, uint32_t *starts)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->nstamps && n < TRIPS_MAX; i++) {
        const struct path_stamp *s = &r->stamps[ix[i]];
        if (s->mark == boundary && s->who == 0)
            starts[n++] = i;
    }
    return n;
}

static void sort_u64(uint64_t *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j && a[j - 1] > a[j]; j--) {
            uint64_t x = a[j];
            a[j] = a[j - 1];
            a[j - 1] = x;
        }
}

/* Each step's median over the trips whose key and length are the chosen one. */
static void step_medians(const struct path_result *r, const uint16_t *ix, const uint32_t *starts,
                         const uint64_t *keys, uint32_t ntrips, uint32_t pick,
                         struct path_shape *out)
{
    uint32_t len = starts[pick + 1] - starts[pick];
    uint64_t d[TRIPS_MAX];
    for (uint32_t step = 0; step < len; step++) {
        uint32_t n = 0;
        for (uint32_t k = 0; k < ntrips; k++) {
            if (keys[k] != keys[pick] || starts[k + 1] - starts[k] != len)
                continue;
            uint32_t a = starts[k] + step;
            d[n++] = r->stamps[ix[a + 1]].tsc - r->stamps[ix[a]].tsc;
        }
        sort_u64(d, n);
        out->median_cycles[step] = n ? d[n / 2] : 0;
    }
    for (uint32_t i = 0; i <= len; i++)
        out->first[i] = r->stamps[ix[starts[pick] + i]];
}

bool path_shape_of(const struct path_result *r, enum path_mark boundary,
                   struct path_shape *out)
{
    memset(out, 0, sizeof(*out));
    uint16_t *ix = time_order(r);
    uint32_t *starts = kmalloc(TRIPS_MAX * sizeof(uint32_t));
    uint64_t *keys = kmalloc(TRIPS_MAX * sizeof(uint64_t));
    bool ok = ix && starts && keys;
    uint32_t nb = ok ? find_trips(r, ix, boundary, starts) : 0;
    uint32_t ntrips = nb ? nb - 1 : 0;   /* the last boundary only closes a trip */
    uint32_t best = 0, pick = 0;
    for (uint32_t k = 0; k < ntrips; k++)
        keys[k] = trip_key(r, ix, starts[k], starts[k + 1]);
    for (uint32_t k = 0; k < ntrips; k++) {
        uint32_t len = starts[k + 1] - starts[k], same = 0;
        if (len > PATH_STEPS_MAX)
            continue;
        for (uint32_t m = 0; m < ntrips; m++)
            same += keys[m] == keys[k] && starts[m + 1] - starts[m] == len;
        if (same > best) {
            best = same;
            pick = k;
        }
    }
    ok = ok && best;
    if (ok) {
        out->trips = best;
        out->total = ntrips;
        out->nsteps = starts[pick + 1] - starts[pick];
        step_medians(r, ix, starts, keys, ntrips, pick, out);
    }
    kfree(ix);
    kfree(starts);
    kfree(keys);
    return ok;
}

/* ---- printing ----------------------------------------------------------------- */

/* "12.34": v / div with two decimals, rounded. */
static void fmt_ratio(char *buf, size_t n, uint64_t v, uint64_t div)
{
    uint64_t x = div ? (v * 100 + div / 2) / div : 0;
    ksnprintf(buf, n, "%lu.%02lu", x / 100, x % 100);
}

struct case_info {
    const char *name;          /* short name, for the summary line */
    const char *what;          /* the case, for the "path:" lines */
    const char *unit;          /* "call" or "switch" */
    uint64_t    per_trip;      /* units per round trip (a yield round trip is 2 switches) */
    enum path_mark boundary;   /* the lead's mark that starts a round trip */
    const char *who[2];        /* the members' names */
};

static void print_group(const struct path_result *r, uint64_t units, const char *title,
                        const enum path_ev *evs, unsigned n)
{
    char line[160], v[24];
    int len = ksnprintf(line, sizeof(line), "path:   %s:", title);
    for (unsigned i = 0; i < n && len < (int)sizeof(line); i++) {
        fmt_ratio(v, sizeof(v), r->count[evs[i]], units);
        len += ksnprintf(line + len, sizeof(line) - (size_t)len, "%s %s %s", i ? "," : "", v,
                         path_ev_name(evs[i]));
    }
    kprintf("%s\n", line);
}

static const char *sys_name(uint32_t nr);

/* The system calls by number, most frequent first (at most 6). */
static void print_sys(const struct path_result *r, uint64_t units)
{
    char line[160], v[24];
    int len = ksnprintf(line, sizeof(line), "path:   syscalls by number:");
    bool used[PATH_SYS_N] = { false };
    for (unsigned k = 0; k < 6 && len < (int)sizeof(line); k++) {
        unsigned best = PATH_SYS_N;
        for (unsigned i = 0; i < PATH_SYS_N; i++)
            if (!used[i] && r->sys[i] && (best == PATH_SYS_N || r->sys[i] > r->sys[best]))
                best = i;
        if (best == PATH_SYS_N)
            break;
        used[best] = true;
        fmt_ratio(v, sizeof(v), r->sys[best], units);
        const char *n = sys_name(best);
        if (n)
            len += ksnprintf(line + len, sizeof(line) - (size_t)len, "%s %s %s", k ? "," : "",
                             v, n);
        else
            len += ksnprintf(line + len, sizeof(line) - (size_t)len, "%s %s #%u", k ? "," : "",
                             v, best);
    }
    if (r->count[PATH_SYSCALL])
        kprintf("%s\n", line);
}

static void print_counts(const struct case_info *ci, const struct path_result *r)
{
    uint64_t units = r->calls * ci->per_trip;
    kprintf("path: %s: per %s, over %lu %ss (%lu round trips)\n", ci->what, ci->unit, units,
            ci->unit, r->calls);
    static const enum path_ev entries[] = { PATH_SYSCALL, PATH_IRQ, PATH_TRAP };
    static const enum path_ev copies[] = { PATH_UCOPY_IN, PATH_UCOPY_IN_B, PATH_UCOPY_OUT,
                                           PATH_UCOPY_OUT_B, PATH_KCOPY, PATH_KCOPY_B };
    static const enum path_ev memory[] = { PATH_KMALLOC, PATH_KFREE, PATH_JOB, PATH_JOB_LEVEL,
                                           PATH_HANDLE, PATH_LOCK };
    static const enum path_ev sched[] = { PATH_SCHED, PATH_SWITCH, PATH_HANDOFF, PATH_WAKE,
                                          PATH_IPI, PATH_SLEEPQ, PATH_TIMER_ARM };
    static const enum path_ev arch[] = { PATH_FPU_SAVE, PATH_FPU_CALLED, PATH_FPU_RESTORE,
                                         PATH_FPU_KEPT, PATH_CR3, PATH_CR3_FLUSH };
    static const enum path_ev other[] = { PATH_EMPTY_READ, PATH_OBSERVER, PATH_CLOCK,
                                          PATH_LOCK_SLOW };
    print_group(r, units, "kernel entries", entries, 3);
    print_sys(r, units);
    print_group(r, units, "copies", copies, 6);
    print_group(r, units, "memory, handles, locks", memory, 6);
    print_group(r, units, "scheduler", sched, 7);
    print_group(r, units, "FPU and address space", arch, 6);
    print_group(r, units, "other", other, 4);
}

/* "1.5": v / div with one decimal, rounded (the RESULTS box is narrow). */
static void fmt_short1(char *buf, size_t n, uint64_t v, uint64_t div)
{
    uint64_t x = div ? (v * 10 + div / 2) / div : 0;
    ksnprintf(buf, n, "%lu.%lu", x / 10, x % 10);
}

/* One line for the RESULTS box (at most ~115 characters). */
static void summary(const struct case_info *ci, const struct path_result *r)
{
    uint64_t units = r->calls * ci->per_trip;
    static const enum path_ev evs[9] = { PATH_SYSCALL, PATH_UCOPY_IN, PATH_KCOPY, PATH_KMALLOC,
                                         PATH_HANDLE, PATH_LOCK, PATH_SCHED, PATH_FPU_SAVE,
                                         PATH_CR3 };
    static const char *const names[9] = { "sys", "ucopy", "kcopy", "kmalloc", "handle", "lock",
                                          "sched", "fpu", "cr3" };
    char line[128], v[16];
    int len = ksnprintf(line, sizeof(line), "bench: path %s /%s:", ci->name, ci->unit);
    for (unsigned i = 0; i < 9 && len < (int)sizeof(line); i++) {
        uint64_t c = r->count[evs[i]];
        if (evs[i] == PATH_UCOPY_IN)
            c += r->count[PATH_UCOPY_OUT];
        if (evs[i] == PATH_FPU_SAVE)
            c += r->count[PATH_FPU_RESTORE];
        fmt_short1(v, sizeof(v), c, units);
        len += ksnprintf(line + len, sizeof(line) - (size_t)len, " %s %s", v, names[i]);
    }
    report("%s", line);
}

static const char *sys_name(uint32_t nr)
{
    switch (nr) {
    case SYS_channel_call:    return "call";
    case SYS_channel_reply_wait: return "reply_wait";
    case SYS_channel_read:    return "read";
    case SYS_channel_write:   return "write";
    case SYS_object_wait_one: return "wait_one";
    case SYS_port_wait:       return "port_wait";
    case SYS_clock_get:       return "clock_get";
    default:                  return NULL;
    }
}

static void stamp_text(char *buf, size_t n, const struct case_info *ci,
                       const struct path_stamp *s)
{
    const char *who = s->who < 2 ? ci->who[s->who] : "?";
    const char *mk = path_mark_name((enum path_mark)s->mark);
    bool sys = s->mark == PATH_MK_SYS_ENTER || s->mark == PATH_MK_SYS_EXIT;
    const char *sn = sys ? sys_name(s->arg) : NULL;
    if (sys && sn)
        ksnprintf(buf, n, "%s %s(%s)", who, mk, sn);
    else if (sys || s->mark == PATH_MK_READ)
        ksnprintf(buf, n, "%s %s(%u)", who, mk, s->arg);
    else
        ksnprintf(buf, n, "%s %s", who, mk);
}

static void print_timeline(const struct case_info *ci, const struct path_result *r,
                           uint64_t mark_ps)
{
    struct path_shape *sh = kmalloc(sizeof(*sh));
    if (!sh || !path_shape_of(r, ci->boundary, sh)) {
        kprintf("path:   timeline: none (%u stamps%s)\n", r->nstamps, r->full ? ", full" : "");
        kfree(sh);
        return;
    }
    kprintf("path:   timeline: %u of %u round trips have the usual shape, %u steps; each step "
            "includes one mark (%lu.%lu ns here)\n", sh->trips, sh->total, sh->nsteps,
            mark_ps / 1000, mark_ps / 100 % 10);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < sh->nsteps; i++) {
        char a[48], b[48];
        stamp_text(a, sizeof(a), ci, &sh->first[i]);
        stamp_text(b, sizeof(b), ci, &sh->first[i + 1]);
        uint64_t ps = bench_cycles_to_ps(sh->median_cycles[i]);
        sum += ps;
        kprintf("path:     %2u  %-26s -> %-26s %6lu.%lu ns\n", i, a, b, ps / 1000,
                ps / 100 % 10);
    }
    kprintf("path:   timeline: the steps add up to %lu.%lu ns a round trip, marks included\n",
            sum / 1000, sum / 100 % 10);
    kfree(sh);
}

static void print_case(const struct case_info *ci, const struct path_result *r,
                       uint64_t mark_ps, bool timeline)
{
    print_counts(ci, r);
    summary(ci, r);
    if (timeline)
        print_timeline(ci, r, mark_ps);
}

/* ---- the run --------------------------------------------------------------------- */

static uint64_t mark_cycles;   /* path_mark_cost on P */

static void measure_mark(void *arg)
{
    (void)arg;
    mark_cycles = path_mark_cost();
}

void bench_path_run(void)
{
    int p, p2;
    bench_cpus(&p, &p2);
    thread_join(spawn_pinned(p, measure_mark, NULL));
    uint64_t mark_ps = bench_cycles_to_ps(mark_cycles);
    kprintf("path: per-call breakdowns on cpu%d (P), counted over %u calls after %u; "
            "timelines from %u of them\n", p, PATH_CALLS_N, PATH_SKIP, PATH_MARKED);
    static const struct case_info cases[] = {
        { "switch", "context switch, 2 kernel threads yielding (P)", "switch", 2,
          PATH_MK_YIELD, { "lead", "partner" } },
        { "kcall", "kernel channel_call, same CPU (P)", "call", 1, PATH_MK_CALL,
          { "client", "server" } },
        { "ucall", "user process->process channel_call, same CPU (P)", "call", 1, PATH_MK_CALL,
          { "client", "server" } },
        { "rwcall", "user process->process channel_call, reply-and-wait server, same CPU (P)",
          "call", 1, PATH_MK_CALL, { "client", "server" } },
        { "dcall", "user process->process channel_call with a deadline, same CPU (P)", "call",
          1, PATH_MK_CALL, { "client", "server" } },
        { "gcall", "user process->process generated call (null.ping), same CPU (P)", "call",
          1, PATH_MK_CALL, { "client", "server" } },
        { "tcall", "user thread->thread channel_call, 1 process (P)", "call", 1, PATH_MK_CALL,
          { "proc", "-" } },
        { "ucall2", "user process->process channel_call P->P2", "call", 1, PATH_MK_CALL,
          { "client", "server" } },
    };
    struct path_result r;
    if (bench_path_switch(p, PATH_MARKED, &r))
        print_case(&cases[0], &r, mark_ps, true);
    if (bench_path_kcall(p, PATH_MARKED, &r))
        print_case(&cases[1], &r, mark_ps, true);
    if (bench_path_ucall("call", p, p, PATH_MARKED, &r))
        print_case(&cases[2], &r, mark_ps, true);
    if (bench_path_ucall("rwcall", p, p, PATH_MARKED, &r))
        print_case(&cases[3], &r, mark_ps, true);
    if (bench_path_ucall("dcall", p, p, PATH_MARKED, &r))
        print_case(&cases[4], &r, mark_ps, true);
    if (bench_path_ucall("gcall", p, p, PATH_MARKED, &r))
        print_case(&cases[5], &r, mark_ps, true);
    if (bench_path_ucall("tcall", p, -1, PATH_MARKED, &r))
        print_case(&cases[6], &r, mark_ps, true);
    if (p2 >= 0 && bench_path_ucall("call", p, p2, 0, &r))
        print_case(&cases[7], &r, mark_ps, false);
}
