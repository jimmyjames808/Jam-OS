/* debug_command (M7 Track C): the kernel's test entry points as shell
 * commands. One command at a time runs in a kernel thread of its own
 * ("dbgcmd", the priority the boot's main thread has); the caller waits
 * for it on an event, cancellably, so a killed shell stops waiting at once
 * while the command runs to its end (a ktest can't be stopped halfway).
 * The output is ordinary kernel log text, which the console follows with a
 * klog reader; the result comes back as the call's value.
 *
 *   ktest [prefix]   ktest_run(prefix): the tests run (a failure panics, as
 *                    from the boot menu); PCI functions a driver process
 *                    holds are hidden from the tests (pci_hide_in_use)
 *   bench            bench_run(): 0
 *   stress <s>       stress_run(s), 1..600 s: 0 if every check held, else 1
 *   devices          pci_report(): the number of PCI functions
 *   ps               the caller's job tree (processes, jobs, what they use)
 *   mem              physical memory: total and free (the result: free MiB)
 *   panic            panic the kernel (a test: the panic screen must show
 *                    over the console, which owns the screen)
 *   kill <name>      kill the first process of that name in the whole job
 *                    tree, except those of the caller's ancestor jobs
 *                    (init: nothing would supervise the caller any more);
 *                    tests of restarts (the console, serialin, the HID
 *                    drivers); the result is its koid
 *
 * The boot menu entries still call the same functions from thread "main";
 * nothing here changes them. */
#include <jam/console_svc.h>
#include <jam/event.h>
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
    char           cmd[CMD_MAX];
    struct job    *scope;     /* a reference, or NULL */
    struct job    *caller;    /* a reference, or NULL: "kill" spares its ancestors */
    struct event  *done;      /* SIG_SIGNALED when result is set */
    int64_t        result;
    volatile int   refs;      /* the thread and the caller */
};

static spinlock_t busy_lock = SPINLOCK_INIT("dbgcmd");
static bool busy;

bool dbgcmd_busy(void)
{
    return busy;
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
    if (is(cmd, n, "ktest") || is(cmd, n, "bench")) {
#ifdef JAM_NO_KTESTS
        return ERR_NOT_SUPPORTED;
#else
        return is(cmd, n, "bench") && *rest ? ERR_INVALID_ARGS : OK;
#endif
    }
    if (is(cmd, n, "stress"))
        return parse_u64(rest, &s) && s >= 1 && s <= 600 ? OK : ERR_INVALID_ARGS;
    if (is(cmd, n, "kill"))
    {
        const char *after;
        return *rest && word(rest, &after) && !*after ? OK : ERR_INVALID_ARGS;
    }
    if (is(cmd, n, "devices") || is(cmd, n, "ps") || is(cmd, n, "mem") || is(cmd, n, "panic"))
        return *rest ? ERR_INVALID_ARGS : OK;
    return ERR_NOT_SUPPORTED;
}

static int64_t exec(const char *cmd, struct job *scope, struct job *caller)
{
    const char *rest;
    size_t n = word(cmd, &rest);
#ifndef JAM_NO_KTESTS
    if (is(cmd, n, "ktest")) {
        /* Devices devmgr gave to drivers are theirs: the tests skip them. */
        pci_hide_in_use = true;
        uint32_t busy = 0;
        for (uint32_t i = 0; i < pci_count(); i++)
            busy += pci_get(i)->proc_users != 0;
        kprintf("ktest: from the shell: %u PCI function(s) in use by drivers are skipped\n", busy);
        int r = ktest_run(rest);
        pci_hide_in_use = false;
        return r;
    }
    if (is(cmd, n, "bench")) {
        bench_run();
        return 0;
    }
#endif
    if (is(cmd, n, "stress")) {
        uint64_t s = 0;
        parse_u64(rest, &s);
        return stress_run(s) ? 0 : 1;
    }
    if (is(cmd, n, "devices")) {
        pci_report();
        return pci_count();
    }
    if (is(cmd, n, "panic"))
        panic("debug_command: panic asked for (a test of the panic screen)");
    if (is(cmd, n, "kill")) {
        struct process *p = scope ? job_find_process(scope, rest, caller) : NULL;
        if (!p) {
            kprintf("kill: no process called \"%s\"\n", rest);
            return ERR_NOT_FOUND;
        }
        struct process_info info;
        process_get_info(p, &info);
        kprintf("kill: process %lu \"%s\"\n", info.koid, rest);
        process_kill(p, PROCESS_KILLED_CODE, true);
        kobject_unref(process_kobject(p));
        return (int64_t)info.koid;
    }
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
    job_unref(r->caller);
    kobject_unref(&r->done->base);
    kfree(r);
}

static void run_thread(void *arg)
{
    struct run *r = arg;
    kprintf("dbgcmd: %s\n", r->cmd);
    r->result = exec(r->cmd, r->scope, r->caller);
    kprintf("dbgcmd: %s -> %ld\n", r->cmd, r->result);
    uint64_t f = spin_lock_irqsave(&busy_lock);
    busy = false;
    spin_unlock_irqrestore(&busy_lock, f);
    event_signal(r->done, 0, SIG_SIGNALED);
    run_put(r);
}

int64_t dbgcmd_run(const char *cmd, size_t len, struct job *scope)
{
    return dbgcmd_run_from(cmd, len, scope, NULL);
}

int64_t dbgcmd_run_from(const char *cmd, size_t len, struct job *scope, struct job *caller)
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
    job_ref(caller);
    r->caller = caller;
    r->refs = 2;

    uint64_t f = spin_lock_irqsave(&busy_lock);
    bool was = busy;
    busy = true;
    spin_unlock_irqrestore(&busy_lock, f);
    if (was) {
        r->refs = 1;
        run_put(r);
        return ERR_BAD_STATE;
    }
    struct thread *t = thread_try_create_on("dbgcmd", run_thread, r, PRIO_DEFAULT, NULL);
    if (!t) {
        f = spin_lock_irqsave(&busy_lock);
        busy = false;
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
