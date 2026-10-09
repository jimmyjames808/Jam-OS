/* utest: what the kernel gives POSIX, from ring 3: small pids and tids,
 * the tid word, thread_interrupt on each interruptible wait, CPU time,
 * sys_public_info; each with its refusals (bad pointers, rights, handle
 * types). And the child mode "utest tidword" the kernel's
 * proc_tid_word_join_unmap test runs: joiners that unmap a thread's stack
 * the moment its tid word reads 0. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define UNMAPPED ((void *)0x10)   /* never mapped: a bad pointer */

static struct thread_info info_of_thread(handle_t th)
{
    struct thread_info ti;
    memset(&ti, 0xa5, sizeof(ti));
    if (jam_thread_get_info(th, &ti) != OK)
        memset(&ti, 0, sizeof(ti));
    return ti;
}

/* ---- ids ------------------------------------------------------------------- */

static uint32_t id_tid;   /* what the thread's own thread_set_tid_word returned */

static void id_thread(void *arg)
{
    (void)arg;
    id_tid = (uint32_t)jam_thread_set_tid_word(NULL);
}

/* Our pid and tid, a second thread's, a child's: small, nonzero, unique,
 * the same through every call that tells them. */
bool t_posix_ids(void)
{
    static uint8_t stack[8192] __attribute__((aligned(64)));
    handle_t self = startup_handle(SR_SELF_THREAD), proc = startup_handle(SR_SELF_PROCESS);
    struct process_info pi;
    CHECK_ST(jam_process_get_info(proc, &pi), OK);
    CHECK(pi.pid > 0 && pi.pid < IDS_MAX);
    struct thread_info ti = info_of_thread(self);
    CHECK(ti.tid > 0 && ti.tid < IDS_MAX);
    CHECK_EQ(ti.pid, pi.pid);
    CHECK_EQ(ti.process_koid, pi.koid);
    CHECK_EQ(ti.state, THREAD_RUNNING);
    CHECK_EQ(jam_thread_set_tid_word(NULL), ti.tid);

    handle_t th;
    CHECK_ST(thread_spawn("ids", id_thread, NULL, stack, sizeof(stack), &th), OK);
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + 5 * NS_PER_S, NULL), OK);
    struct thread_info t2 = info_of_thread(th);
    CHECK_EQ(t2.tid, id_tid);
    CHECK(t2.tid != ti.tid);
    CHECK_EQ(t2.pid, pi.pid);
    CHECK_EQ(t2.state, THREAD_DEAD);   /* its tid stays its own while we hold it */
    CHECK_ST(jam_handle_close(th), OK);

    handle_t job, child, vmar;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_process_create(job, "ids", 3, 0, &child, &vmar), OK);
    struct process_info ci;
    CHECK_ST(jam_process_get_info(child, &ci), OK);
    CHECK(ci.pid > 0 && ci.pid != pi.pid);
    CHECK_EQ(ci.cpu_ns, 0);   /* never ran */
    CHECK_ST(jam_handle_close(vmar), OK);
    CHECK_ST(jam_handle_close(child), OK);
    CHECK_ST(jam_handle_close(job), OK);

    /* The refusals. */
    uint32_t words[2];
    CHECK_EQ(jam_thread_set_tid_word((uint32_t *)((uint8_t *)words + 2)), ERR_INVALID_ARGS);
    CHECK_EQ(jam_thread_set_tid_word((uint32_t *)0xffff800000001000ull), ERR_INVALID_ARGS);
    CHECK_EQ(jam_thread_set_tid_word((uint32_t *)0x7ffffffffffcull), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_get_info(self, UNMAPPED), ERR_INVALID_ARGS);
    CHECK_ST(jam_thread_get_info(proc, &ti), ERR_WRONG_TYPE);
    CHECK_ST(jam_thread_get_info(HANDLE_INVALID, &ti), ERR_BAD_HANDLE);
    handle_t blind;
    CHECK_ST(jam_handle_duplicate(self, RIGHT_WAIT | RIGHT_DUPLICATE, &blind), OK);
    CHECK_ST(jam_thread_get_info(blind, &ti), ERR_ACCESS_DENIED);
    CHECK_ST(jam_thread_interrupt(blind), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(blind), OK);
    return true;
}

/* ---- thread_interrupt -------------------------------------------------------- */

static uint32_t ir_word;     /* the waiter's futex word: never changes */
static status_t ir_status;   /* what its futex_wait returned */

static void ir_waiter(void *arg)
{
    (void)arg;
    ir_status = jam_futex_wait(&ir_word, 0, DEADLINE_NEVER, 0);
}

/* Interrupt ourselves, then make each wait: it ends at once with
 * ERR_INTERRUPTED (latched), and the one after times out (taken once). */
static bool each_wait_interrupted(handle_t self)
{
    handle_t ev, port;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_port_create(&port), OK);
    for (unsigned kind = 0; kind < 4; kind++) {
        CHECK_ST(jam_thread_interrupt(self), OK);
        CHECK_ST(jam_thread_interrupt(self), OK);   /* not counted */
        uint64_t far = now() + 5 * NS_PER_S, soon = now() + NS_PER_MS;
        struct port_packet pkt;
        status_t a, b;
        if (kind == 0) {
            a = jam_object_wait_one(ev, SIG_SIGNALED, far, NULL);
            b = jam_object_wait_one(ev, SIG_SIGNALED, soon, NULL);
        } else if (kind == 1) {
            a = jam_port_wait(port, far, &pkt);
            b = jam_port_wait(port, soon, &pkt);
        } else if (kind == 2) {
            a = jam_futex_wait(&ir_word, 0, far, 0);
            b = jam_futex_wait(&ir_word, 0, soon, 0);
        } else {
            a = jam_sleep_until(far);
            b = jam_sleep_until(soon);
        }
        CHECK_ST(a, ERR_INTERRUPTED);
        CHECK_ST(b, kind == 3 ? OK : ERR_TIMED_OUT);
    }
    /* A wait whose condition is met returns it and leaves the interrupt. */
    CHECK_ST(jam_event_signal(ev, 0, SIG_SIGNALED), OK);
    CHECK_ST(jam_thread_interrupt(self), OK);
    CHECK_ST(jam_object_wait_one(ev, SIG_SIGNALED, DEADLINE_NEVER, NULL), OK);
    CHECK_ST(jam_sleep_until(now() + 5 * NS_PER_S), ERR_INTERRUPTED);
    CHECK_ST(jam_handle_close(port), OK);
    CHECK_ST(jam_handle_close(ev), OK);
    return true;
}

bool t_thread_interrupt_calls(void)
{
    static uint8_t stack[8192] __attribute__((aligned(64)));
    handle_t self = startup_handle(SR_SELF_THREAD), th;
    if (!each_wait_interrupted(self))
        return false;
    /* Another thread, blocked (or about to block) for good. */
    ir_word = 0;
    ir_status = ERR_IO;
    CHECK_ST(thread_spawn("intr", ir_waiter, NULL, stack, sizeof(stack), &th), OK);
    jam_sleep_until(now() + 2 * NS_PER_MS);
    CHECK_ST(jam_thread_interrupt(th), OK);
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + 5 * NS_PER_S, NULL), OK);
    CHECK_ST(ir_status, ERR_INTERRUPTED);
    CHECK_ST(jam_thread_interrupt(th), ERR_BAD_STATE);   /* gone */
    CHECK_ST(jam_handle_close(th), OK);
    /* Not started; the wrong type; no handle. */
    CHECK_ST(jam_thread_create(startup_handle(SR_SELF_PROCESS), "new", 3, 0, &th), OK);
    CHECK_ST(jam_thread_interrupt(th), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(th), OK);
    CHECK_ST(jam_thread_interrupt(startup_handle(SR_SELF_PROCESS)), ERR_WRONG_TYPE);
    CHECK_ST(jam_thread_interrupt(HANDLE_INVALID), ERR_BAD_HANDLE);
    return true;
}

/* ---- CPU time and the public facts ---------------------------------------------- */

static uint32_t sl_going;   /* the sleeper is about to sleep (RELEASE) */

static void sleeper(void *arg)
{
    (void)arg;
    __atomic_store_n(&sl_going, 1, __ATOMIC_RELEASE);
    jam_sleep_until(now() + 100 * NS_PER_MS);
}

bool t_cpu_ns(void)
{
    static uint8_t stack[8192] __attribute__((aligned(64)));
    handle_t self = startup_handle(SR_SELF_THREAD), proc = startup_handle(SR_SELF_PROCESS);
    struct process_info p0, p1;
    CHECK_ST(jam_process_get_info(proc, &p0), OK);
    uint64_t t0 = info_of_thread(self).cpu_ns, end = now() + 10 * NS_PER_MS;
    while ((uint64_t)now() < end)
        ;   /* run: our time grows */
    uint64_t t1 = info_of_thread(self).cpu_ns;
    CHECK_ST(jam_process_get_info(proc, &p1), OK);
    CHECK(t1 > t0);
    CHECK(p1.cpu_ns > p0.cpu_ns);
    CHECK(p1.cpu_ns >= t1);   /* ours and the other threads' */
    /* A thread asleep: its time stays where it is. */
    handle_t th;
    sl_going = 0;
    CHECK_ST(thread_spawn("sleeper", sleeper, NULL, stack, sizeof(stack), &th), OK);
    while (!__atomic_load_n(&sl_going, __ATOMIC_ACQUIRE))
        jam_sleep_until(now() + NS_PER_MS);
    jam_sleep_until(now() + 5 * NS_PER_MS);
    uint64_t s0 = info_of_thread(th).cpu_ns;
    jam_sleep_until(now() + 30 * NS_PER_MS);   /* within its 100 ms */
    uint64_t s1 = info_of_thread(th).cpu_ns;
    CHECK(s0 > 0);
    CHECK(s1 - s0 < NS_PER_MS);   /* at most a stray wake, never its 30 ms */
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, now() + 5 * NS_PER_S, NULL), OK);
    CHECK_ST(jam_handle_close(th), OK);
    return true;
}

bool t_sys_public_info(void)
{
    struct sys_public_info s;
    memset(&s, 0xa5, sizeof(s));
    CHECK_ST(jam_sys_public_info(&s), OK);
    CHECK(s.version[0] && !s.version[sizeof(s.version) - 1]);   /* zeroed past the text */
    CHECK(!strcmp(s.machine, "x86_64"));
    CHECK_EQ(s.page_size, 4096);
    CHECK(s.cpus_online >= 1);
    CHECK(s.mem_total_pages > 1000);
    CHECK_ST(jam_sys_public_info(UNMAPPED), ERR_INVALID_ARGS);
    CHECK_ST(jam_sys_public_info((struct sys_public_info *)0xffff800000000000ull),
             ERR_INVALID_ARGS);
    return true;
}

/* ---- "utest tidword" ------------------------------------------------------------ */

#define TW_ROUNDS 300
#define TW_MAP    (16 * 1024)   /* each thread's mapping: its word at the bottom, its stack */

static void tw_thread(void *arg)
{
    uint32_t *word = arg;
    *word = (uint32_t)jam_thread_set_tid_word(word);
    volatile uint8_t scratch[512];   /* its stack in use up to the end */
    for (unsigned i = 0; i < sizeof(scratch); i++)
        scratch[i] = (uint8_t)i;
}

/* Threads whose stack and tid word share one mapping, unmapped by the
 * joiner the moment the word reads 0: if the kernel cleared it before the
 * thread was done with its stack, the thread would fault (and we with
 * it). 0: every round joined; else what went wrong. */
int tidword_child(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR);
    for (unsigned r = 0; r < TW_ROUNDS; r++) {
        handle_t vmo, th;
        uint64_t addr = 0;
        if (jam_vmo_create(TW_MAP, 0, &vmo) != OK ||
            jam_vmar_map(vmar, vmo, 0, TW_MAP, VMAR_READ | VMAR_WRITE, &addr) != OK)
            return 2;
        jam_handle_close(vmo);   /* the mapping keeps it */
        uint32_t *word = (uint32_t *)(uintptr_t)addr;
        *word = UINT32_MAX;      /* not 0 until the thread's end */
        if (thread_spawn("tw", tw_thread, word, word + 16, TW_MAP - 64, &th) != OK)
            return 3;
        uint64_t end = now() + 10 * NS_PER_S;
        for (uint32_t v; (v = __atomic_load_n(word, __ATOMIC_ACQUIRE)) != 0;) {
            if ((uint64_t)now() > end)
                return 4;
            status_t st = jam_futex_wait(word, v, end, 0);
            if (st != OK && st != ERR_BAD_STATE && st != ERR_TIMED_OUT)
                return 5;
        }
        if (jam_vmar_unmap(vmar, addr, TW_MAP) != OK)   /* at once */
            return 6;
        if (jam_object_wait_one(th, SIG_TERMINATED, now() + 10 * NS_PER_S, NULL) != OK)
            return 7;
        jam_handle_close(th);
    }
    return 0;
}
