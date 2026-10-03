/* Path statistics: the trace behind the PATH_* probes (jam/pathstat.h has
 * the model). One static trace, claimed by path_begin and given back by
 * path_end, so a probe never touches freed memory: the worst a late probe
 * can do is add to a trace nobody reads any more.
 *
 * A probe's slow path runs with interrupts off and holds no lock (only
 * atomics), so it can be reached from spin_lock itself, from the
 * scheduler and from kmalloc without recursing into anything it counts.
 * path_end waits for `inflight` to drain before reading. */
#include <stddef.h>
#include <jam/pathstat.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

struct path_member {
    struct thread  *thread;   /* this kernel thread, or NULL */
    struct process *process;  /* every thread of this process, or NULL */
};

struct path_trace {
    enum path_mark     boundary;              /* the lead's mark that starts a call */
    uint64_t           lo;                    /* first counted call (boundaries seen) */
    uint64_t           hi;                    /* first call past the window */
    uint64_t           mark_hi;               /* first call past the timed part */
    uint64_t           seen;                  /* the lead's boundaries since path_arm (atomic) */
    uint32_t           nmembers;              /* set before arming, then read-only */
    struct path_member member[PATH_MEMBERS];  /* [0] is the lead */
    uint64_t           count[PATH_EV_N];      /* atomic */
    uint64_t           sys[PATH_SYS_N];       /* system calls by number (atomic) */
    uint32_t           nstamps;               /* stamps handed out (atomic; may pass the max) */
    uint32_t           inflight;              /* probes inside a slow path (atomic) */
    struct path_stamp  stamps[PATH_MARKS_MAX];
};

static struct path_trace trace;
static bool claimed;   /* path_begin .. path_end (atomic) */
struct path_trace *path_active;

static int member_of(const struct path_trace *pt, const struct thread *t)
{
    if (!t)
        return -1;
    for (uint32_t i = 0; i < pt->nmembers; i++) {
        const struct path_member *m = &pt->member[i];
        if (m->thread == t || (m->process && t->process == m->process))
            return (int)i;
    }
    return -1;
}

/* The member index of the current thread, or -1. Interrupts off. */
static int member_now(const struct path_trace *pt, bool irq_event)
{
    struct cpu *c = this_cpu();
    if (c->irq_depth && !irq_event)
        return -1;   /* an interrupt handler's work is not the call's */
    return member_of(pt, c->current);
}

/* prev's index if it is a member, else next's, else -1. Interrupts off. */
static int member_sw(const struct path_trace *pt, const struct thread *prev,
                     const struct thread *next)
{
    if (this_cpu()->irq_depth)
        return -1;
    int who = member_of(pt, prev);
    return who >= 0 ? who : member_of(pt, next);
}

static bool counting(const struct path_trace *pt, uint64_t seen)
{
    return seen >= pt->lo && seen < pt->hi;
}

/* Enter a slow path: false if pt is no longer the armed trace. */
static bool enter(struct path_trace *pt)
{
    __atomic_add_fetch(&pt->inflight, 1, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&path_active, __ATOMIC_SEQ_CST) == pt;
}

static void leave(struct path_trace *pt)
{
    __atomic_sub_fetch(&pt->inflight, 1, __ATOMIC_RELEASE);
}

void path_add_slow(struct path_trace *pt, enum path_ev ev, uint64_t n)
{
    uint64_t f = irq_save();
    if (enter(pt) && member_now(pt, ev == PATH_IRQ) >= 0 &&
        counting(pt, __atomic_load_n(&pt->seen, __ATOMIC_RELAXED)))
        __atomic_add_fetch(&pt->count[ev], n, __ATOMIC_RELAXED);
    leave(pt);
    irq_restore(f);
}

void path_sys_slow(struct path_trace *pt, uint64_t nr)
{
    uint64_t f = irq_save();
    if (enter(pt) && member_now(pt, false) >= 0 &&
        counting(pt, __atomic_load_n(&pt->seen, __ATOMIC_RELAXED))) {
        __atomic_add_fetch(&pt->count[PATH_SYSCALL], 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&pt->sys[nr < PATH_SYS_N ? nr : PATH_SYS_N - 1], 1,
                           __ATOMIC_RELAXED);
    }
    leave(pt);
    irq_restore(f);
}

void path_sw_add_slow(struct path_trace *pt, const struct thread *prev,
                      const struct thread *next, enum path_ev ev)
{
    uint64_t f = irq_save();
    if (enter(pt) && member_sw(pt, prev, next) >= 0 &&
        counting(pt, __atomic_load_n(&pt->seen, __ATOMIC_RELAXED)))
        __atomic_add_fetch(&pt->count[ev], 1, __ATOMIC_RELAXED);
    leave(pt);
    irq_restore(f);
}

static void stamp_put(struct path_trace *pt, uint64_t tsc, enum path_mark mk, int who,
                      uint32_t arg)
{
    uint32_t i = __atomic_fetch_add(&pt->nstamps, 1, __ATOMIC_RELAXED);
    if (i >= PATH_MARKS_MAX)
        return;
    pt->stamps[i] = (struct path_stamp){ tsc, (uint16_t)mk, (uint8_t)who,
                                         (uint8_t)this_cpu()->index, arg };
}

void path_mark_slow(struct path_trace *pt, enum path_mark mk, uint32_t arg)
{
    uint64_t tsc = rdtsc();   /* first: the rest of this function is overhead */
    uint64_t f = irq_save();
    int who = enter(pt) ? member_now(pt, false) : -1;
    if (who >= 0) {
        uint64_t seen = __atomic_load_n(&pt->seen, __ATOMIC_RELAXED);
        bool boundary = who == 0 && mk == pt->boundary;
        if (boundary) {
            seen = __atomic_add_fetch(&pt->seen, 1, __ATOMIC_RELAXED);
            if (counting(pt, seen))
                __atomic_add_fetch(&pt->count[PATH_CALLS], 1, __ATOMIC_RELAXED);
        }
        /* The boundary that closes the last timed call is kept too, so
         * that call's last step has an end. */
        if ((seen >= pt->lo && seen < pt->mark_hi) || (boundary && seen == pt->mark_hi))
            stamp_put(pt, tsc, mk, who, arg);
    }
    leave(pt);
    irq_restore(f);
}

void path_sw_mark_slow(struct path_trace *pt, const struct thread *prev,
                       const struct thread *next, enum path_mark mk)
{
    uint64_t tsc = rdtsc();
    uint64_t f = irq_save();
    int who = enter(pt) ? member_sw(pt, prev, next) : -1;
    uint64_t seen = __atomic_load_n(&pt->seen, __ATOMIC_RELAXED);
    if (who >= 0 && seen >= pt->lo && seen < pt->mark_hi)
        stamp_put(pt, tsc, mk, who, 0);
    leave(pt);
    irq_restore(f);
}

bool path_begin(enum path_mark boundary, uint64_t skip, uint64_t calls, uint64_t marked)
{
    bool expect = false;
    if (!__atomic_compare_exchange_n(&claimed, &expect, true, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return false;
    /* No probe reads the trace while path_active is NULL and inflight has
     * drained (path_end), so plain stores are fine until path_arm. */
    memset(&trace, 0, offsetof(struct path_trace, stamps));   /* stamps: written before read */
    trace.boundary = boundary;
    trace.lo = skip + 1;   /* `seen` counts the boundary that starts a call */
    trace.hi = trace.lo + calls;
    trace.mark_hi = trace.lo + (marked < calls ? marked : calls);
    return true;
}

static bool add_member(struct thread *t, struct process *p)
{
    if (trace.nmembers == PATH_MEMBERS)
        return false;
    trace.member[trace.nmembers++] = (struct path_member){ t, p };
    return true;
}

bool path_add_thread(struct thread *t)
{
    return add_member(t, NULL);
}

bool path_add_process(struct process *p)
{
    return add_member(NULL, p);
}

void path_arm(void)
{
    __atomic_store_n(&path_active, &trace, __ATOMIC_SEQ_CST);
}

bool path_window_done(void)
{
    return __atomic_load_n(&trace.seen, __ATOMIC_RELAXED) >= trace.hi;
}

void path_end(struct path_result *out)
{
    __atomic_store_n(&path_active, NULL, __ATOMIC_SEQ_CST);
    /* A slow path that read the old pointer is a few dozen instructions
     * with interrupts off; give up after a second rather than hang (a
     * vCPU the host stopped), at the price of a count that may still
     * move by one. */
    uint64_t until = uptime_ns() + 1000000000ull;
    while (__atomic_load_n(&trace.inflight, __ATOMIC_ACQUIRE) && uptime_ns() < until)
        cpu_relax();
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < PATH_EV_N; i++)
        out->count[i] = __atomic_load_n(&trace.count[i], __ATOMIC_RELAXED);
    for (unsigned i = 0; i < PATH_SYS_N; i++)
        out->sys[i] = __atomic_load_n(&trace.sys[i], __ATOMIC_RELAXED);
    out->calls = out->count[PATH_CALLS];
    uint32_t n = __atomic_load_n(&trace.nstamps, __ATOMIC_RELAXED);
    out->full = n > PATH_MARKS_MAX;
    out->nstamps = out->full ? PATH_MARKS_MAX : n;
    out->stamps = trace.stamps;
    __atomic_store_n(&claimed, false, __ATOMIC_RELEASE);
}

#define COST_MARKS 64

uint64_t path_mark_cost(void)
{
    if (!path_begin(PATH_MK_YIELD, 0, 1, 1))
        return 0;   /* a trace is running */
    struct path_result r;
    path_add_thread(current_thread());
    /* No boundary is ever passed, so `seen` stays 0: open the window at 0. */
    trace.lo = 0;
    path_arm();
    for (unsigned i = 0; i < COST_MARKS; i++)
        PATH_MARK(PATH_MK_SCHED_IN);
    path_end(&r);
    uint64_t d[COST_MARKS - 1];
    unsigned n = 0;
    for (uint32_t i = 1; i < r.nstamps && n < COST_MARKS - 1; i++)
        d[n++] = r.stamps[i].tsc - r.stamps[i - 1].tsc;
    if (!n)
        return 0;
    for (unsigned i = 1; i < n; i++)   /* insertion sort: n is small */
        for (unsigned j = i; j && d[j - 1] > d[j]; j--) {
            uint64_t x = d[j];
            d[j] = d[j - 1];
            d[j - 1] = x;
        }
    return d[n / 2];
}

static const char *const ev_names[PATH_EV_N] = {
    [PATH_SYSCALL] = "syscalls",        [PATH_IRQ] = "interrupts",
    [PATH_TRAP] = "exceptions",         [PATH_UCOPY_IN] = "user copies in",
    [PATH_UCOPY_IN_B] = "bytes in",     [PATH_UCOPY_OUT] = "user copies out",
    [PATH_UCOPY_OUT_B] = "bytes out",   [PATH_KCOPY] = "kernel copies",
    [PATH_KCOPY_B] = "kernel bytes",    [PATH_KMALLOC] = "kmallocs",
    [PATH_KFREE] = "kfrees",            [PATH_HANDLE] = "handle ops",
    [PATH_LOCK] = "locks",              [PATH_SCHED] = "sched passes",
    [PATH_SWITCH] = "switches",         [PATH_WAKE] = "wakes",
    [PATH_IPI] = "IPIs",                [PATH_FPU_SAVE] = "FPU saves",
    [PATH_FPU_CALLED] = "FPU call saves",
    [PATH_FPU_RESTORE] = "FPU restores", [PATH_FPU_KEPT] = "FPU kept",
    [PATH_CR3] = "CR3 loads",           [PATH_CR3_FLUSH] = "CR3 flushes",
    [PATH_JOB] = "job charges",         [PATH_JOB_LEVEL] = "job levels",
    [PATH_SLEEPQ] = "sleeper inserts",  [PATH_TIMER_ARM] = "timer arms",
    [PATH_EMPTY_READ] = "empty reads",  [PATH_OBSERVER] = "observers",
    [PATH_CALLS] = "calls",
};

static const char *const mk_names[PATH_MK_N] = {
    [PATH_MK_SYS_ENTER] = "sys_enter",  [PATH_MK_SYS_EXIT] = "sys_exit",
    [PATH_MK_CALL] = "call",            [PATH_MK_YIELD] = "yield",
    [PATH_MK_MSG_MADE] = "msg_made",    [PATH_MK_SENT] = "sent",
    [PATH_MK_REPLY] = "reply",          [PATH_MK_READ] = "read",
    [PATH_MK_WAIT_IN] = "wait_in",      [PATH_MK_WAIT_OUT] = "wait_out",
    [PATH_MK_BLOCK] = "block",          [PATH_MK_SCHED_IN] = "sched_in",
    [PATH_MK_SCHED_LOCKED] = "sched_locked", [PATH_MK_SCHED_PICKED] = "sched_picked",
    [PATH_MK_ARCH_FPU] = "arch_fpu",    [PATH_MK_ARCH_DONE] = "arch_done",
    [PATH_MK_SCHED_DONE] = "sched_done",
};

const char *path_ev_name(enum path_ev ev)
{
    return (unsigned)ev < PATH_EV_N ? ev_names[ev] : "?";
}

const char *path_mark_name(enum path_mark mk)
{
    return (unsigned)mk < PATH_MK_N ? mk_names[mk] : "?";
}
