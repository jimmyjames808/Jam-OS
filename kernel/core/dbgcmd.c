/* debug_command (M7 Track C): the kernel's test entry points as shell
 * commands. One command at a time runs in a kernel thread of its own
 * ("dbgcmd", the priority the boot's main thread has); the caller waits
 * for it on an event, cancellably, so a killed shell stops waiting at once
 * while the command runs to its end (a ktest can't be stopped halfway).
 * The output is ordinary kernel log text, which the console follows with a
 * klog reader; the result comes back as the call's value.
 *
 *   ktest [prefix]   ktest_run(prefix): the tests run (a failure panics, as
 *                    from the boot menu)
 *   bench            bench_run(): 0
 *   stress <s>       stress_run(s), 1..600 s: 0 if every check held, else 1
 *   devices          pci_report(): the number of PCI functions
 *   ps               the caller's job tree (processes, jobs, what they use)
 *
 * The boot menu entries still call the same functions from thread "main";
 * nothing here changes them. */
#include <jam/console_svc.h>
#include <jam/event.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
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
    if (is(cmd, n, "devices") || is(cmd, n, "ps"))
        return *rest ? ERR_INVALID_ARGS : OK;
    return ERR_NOT_SUPPORTED;
}

static int64_t exec(const char *cmd, struct job *scope)
{
    const char *rest;
    size_t n = word(cmd, &rest);
#ifndef JAM_NO_KTESTS
    if (is(cmd, n, "ktest"))
        return ktest_run(rest);
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
    busy = false;
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
