/* debug_command: the kernel's test entry points as shell
 * commands. One command at a time runs in a kernel thread of its own
 * ("dbgcmd", the priority the boot's main thread has); the caller waits
 * for it on an event, cancellably, so a killed shell stops waiting at once
 * while the command runs to its end (a ktest can't be stopped halfway).
 * The output is ordinary kernel log text, which the console follows with a
 * klog reader; the result comes back as the call's value.
 *
 *   ktest [prefix] [loops=N] [seed=S] [shuffle] [keep] [load[=N]]
 *                    ktest_run_opts (ktest.h has the words) with ktest_live
 *                    set: a failure panics,
 *                    as from the boot menu, but checks on system-wide counts
 *                    (user space allocates meanwhile) are not made and the
 *                    per-test leak check only logs (ktest.h); PCI functions
 *                    a driver holds (or devmgr ever bound: pci_in_use) are
 *                    hidden from the tests (pci_hide_in_use)
 *   soak begin       clear the soak record and note what the system holds
 *   soak end u=<runs>,<failed> io=<cycles>,<failed> [halt]
 *                    print the soak's summary (kernel/test/ktest_soak.c): the
 *                    number of failures; with halt, panic if there is one
 *   bench            bench_run(): 0
 *   stress <s>       stress_run(s), 1..600 s: 0 if every check held, else 1
 *   devices          pci_report(): the number of PCI functions
 *   ps               the caller's job tree (processes, jobs, what they use)
 *   mem              physical memory: total and free (the result: free MiB)
 *   panic            panic the kernel (a test: the panic screen must show
 *                    over the console, which owns the screen)
 *   crash [name]     the crash tests (selftest.c; boot words test<name>): alone,
 *                    list them (the result: how many); with a name, run it
 *                    in a thread pinned to CPU 0 (as the boot's main thread
 *                    ran them; the ones needing a second CPU use CPU 1).
 *                    Each panics on purpose, except bp, which returns 0
 *   memmap           the loader's memory map (the "memmap" boot word)
 *   iommu            the IOMMU (VT-d): the units, their translation,
 *                    interrupt-remapping and queue state, each covered
 *                    function's domain and its mapped pages, the DMA fault
 *                    counts and the interrupt remapping table's use
 *                    (iommu_report, <jam/iommu.h>): 0
 *
 * Killing a process by name is not here: that is init's (its control
 * channel, abi/idl/initctl.idl), with the handles init and devmgr hold.
 *
 * The boot menu entries still call the same functions from thread "main";
 * nothing here changes them. */
#include <jam/boot.h>
#include <jam/console_svc.h>
#include <jam/event.h>
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/selftest.h>
#include <jam/spinlock.h>
#include <jam/string.h>

#define CMD_MAX 64

struct run {
    char           cmd[CMD_MAX];  /* the command line, NUL-terminated */
    struct job    *scope;         /* a reference, or NULL */
    struct event  *done;          /* SIG_SIGNALED when result is set */
    int64_t        result;        /* exec's return value */
    int            refs;          /* the thread and the caller (atomic once it runs) */
};

static spinlock_t busy_lock = SPINLOCK_INIT("dbgcmd");
static bool busy;   /* a command runs: changed under busy_lock, read by dbgcmd_busy (atomic) */

bool dbgcmd_busy(void)
{
    return __atomic_load_n(&busy, __ATOMIC_RELAXED);
}

/* "word rest": the word's length; *rest after the spaces. */
static size_t word(const char *s, const char **rest)
{
    size_t n = 0;
    while (s[n] && s[n] != ' ')
        n++;
    const char *r = s + n;
    while (*r == ' ')
        r++;
    *rest = r;
    return n;
}

static bool is(const char *w, size_t n, const char *name)
{
    return strlen(name) == n && !memcmp(w, name, n);
}

static bool parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s && *s != ' '; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

status_t dbgcmd_check(const char *cmd, size_t len)
{
    if (len == 0 || len >= CMD_MAX || strlen(cmd) != len)
        return ERR_INVALID_ARGS;
    const char *rest;
    size_t n = word(cmd, &rest);
    uint64_t s;
    if (is(cmd, n, "ktest") || is(cmd, n, "bench") || is(cmd, n, "soak")) {
#ifdef JAM_NO_KTESTS
        return ERR_NOT_SUPPORTED;
#else
        struct ktest_opts o;
        const char *args;
        if (is(cmd, n, "ktest"))
            return ktest_parse_opts(rest, false, &o) ? OK : ERR_INVALID_ARGS;
        if (is(cmd, n, "bench"))
            return *rest ? ERR_INVALID_ARGS : OK;
        size_t k = word(rest, &args);
        if (is(rest, k, "begin"))
            return *args ? ERR_INVALID_ARGS : OK;
        return is(rest, k, "end") && ktest_soak_args_ok(args) ? OK : ERR_INVALID_ARGS;
#endif
    }
    if (is(cmd, n, "stress"))
        return parse_u64(rest, &s) && s >= 1 && s <= 600 ? OK : ERR_INVALID_ARGS;
    if (is(cmd, n, "crash")) {
        const char *after;
        size_t k = word(rest, &after);
        if (*after)
            return ERR_INVALID_ARGS;
        return !k || selftest_crash_known(rest, k) ? OK : ERR_NOT_FOUND;
    }
    if (is(cmd, n, "devices") || is(cmd, n, "ps") || is(cmd, n, "mem") || is(cmd, n, "panic") ||
        is(cmd, n, "memmap") || is(cmd, n, "iommu"))
        return *rest ? ERR_INVALID_ARGS : OK;
    return ERR_NOT_SUPPORTED;
}

static int64_t exec(const char *cmd, struct job *scope)
{
    const char *rest;
    size_t n = word(cmd, &rest);
#ifndef JAM_NO_KTESTS
    if (is(cmd, n, "ktest")) {
        /* Devices devmgr gave to drivers are theirs: the tests skip them. */
        __atomic_store_n(&pci_hide_in_use, true, __ATOMIC_RELAXED);
        uint32_t in_use = 0;
        for (uint32_t i = 0; i < pci_count(); i++)
            in_use += pci_in_use(pci_get(i));
        kprintf("ktest: from the shell: %u PCI function(s) in use by drivers are skipped\n",
                in_use);
        /* User space runs meanwhile: global counts are not checked
         * (ktest.h: KT_GLOBAL_EQ, KT_SKIP_LIVE, the leak check logs). */
        struct ktest_opts o;
        ktest_parse_opts(rest, false, &o);   /* dbgcmd_check saw it parse */
        ktest_live = true;
        int r = ktest_run_opts(&o);
        ktest_live = false;
        __atomic_store_n(&pci_hide_in_use, false, __ATOMIC_RELAXED);
        return r;
    }
    if (is(cmd, n, "bench")) {
        bench_run();
        return 0;
    }
    if (is(cmd, n, "soak")) {
        const char *args;
        size_t k = word(rest, &args);
        if (is(rest, k, "begin")) {
            ktest_soak_begin(scope);
            return 0;
        }
        return ktest_soak_end(scope, args);
    }
#endif
    if (is(cmd, n, "stress")) {
        uint64_t s = 0;
        (void)parse_u64(rest, &s);   /* dbgcmd_check saw it parse */
        return stress_run(s) ? 0 : 1;
    }
    if (is(cmd, n, "devices")) {
        pci_report();
        return pci_count();
    }
    if (is(cmd, n, "crash"))
        return *rest ? selftest_crash_run(rest) : selftest_crash_list();
    if (is(cmd, n, "memmap")) {
        kmain_print_memmap();
        return 0;
    }
    if (is(cmd, n, "iommu")) {
        iommu_report();
        return 0;
    }
    if (is(cmd, n, "panic"))
        panic("debug_command: panic asked for (a test of the panic screen)");
    if (is(cmd, n, "mem")) {
        uint64_t total, free;
        pmm_stats(&total, &free);
        kprintf("mem: %lu MiB managed, %lu MiB free, %lu pages in the thread stack cache\n",
                total >> 8, free >> 8, sched_stack_cache_pages());
        return (int64_t)(free >> 8);
    }
    if (is(cmd, n, "ps")) {
        if (scope)
            job_print_tree(scope, 0);
        else
            kprintf("ps: no job tree\n");
        return 0;
    }
    return ERR_NOT_SUPPORTED;
}

static void run_put(struct run *r)
{
    if (__atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL))
        return;
    job_unref(r->scope);
    kobject_unref(&r->done->base);
    kfree(r);
}

static void run_thread(void *arg)
{
    struct run *r = arg;
    kprintf("dbgcmd: %s\n", r->cmd);
    r->result = exec(r->cmd, r->scope);
    kprintf("dbgcmd: %s -> %ld\n", r->cmd, r->result);
    uint64_t f = spin_lock_irqsave(&busy_lock);
    __atomic_store_n(&busy, false, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&busy_lock, f);
    event_signal(r->done, 0, SIG_SIGNALED);
    run_put(r);
}

int64_t dbgcmd_run(const char *cmd, size_t len, struct job *scope)
{
    status_t st = dbgcmd_check(cmd, len);
    if (st != OK)
        return st;
    struct run *r = kzalloc(sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    if ((st = event_create(&r->done)) != OK) {
        kfree(r);
        return st;
    }
    memcpy(r->cmd, cmd, len);
    r->cmd[len] = '\0';
    job_ref(scope);
    r->scope = scope;
    r->refs = 2;

    uint64_t f = spin_lock_irqsave(&busy_lock);
    bool was = __atomic_load_n(&busy, __ATOMIC_RELAXED);
    __atomic_store_n(&busy, true, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&busy_lock, f);
    if (was) {
        r->refs = 1;
        run_put(r);
        return ERR_BAD_STATE;
    }
    /* A crash test runs on CPU 0, as from the boot menu: the ones that
     * need a second CPU (stuck, watchdog, lockirq) take CPU 1. */
    cpumask_t cpu0;
    cpumask_one(&cpu0, 0);
    const char *rest;
    bool crash = is(r->cmd, word(r->cmd, &rest), "crash") && *rest;
    struct thread *t = thread_try_create_on("dbgcmd", run_thread, r, PRIO_DEFAULT,
                                            crash ? &cpu0 : NULL);
    if (!t) {
        f = spin_lock_irqsave(&busy_lock);
        __atomic_store_n(&busy, false, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&busy_lock, f);
        r->refs = 1;
        run_put(r);
        return ERR_NO_MEMORY;
    }
    thread_detach(t);
    st = object_wait_one(&r->done->base, SIG_SIGNALED, DEADLINE_NEVER, NULL);
    int64_t result = st == OK ? r->result : st;
    run_put(r);
    return result;
}
