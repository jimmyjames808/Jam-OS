/* utest: what the kernel gives POSIX, from ring 3: small pids and tids,
 * the tid word, thread_interrupt on each interruptible wait, CPU time,
 * sys_public_info, VMAR_REPLACE; each with its refusals (bad pointers,
 * rights, handle types). And the child mode "utest tidword" the kernel's
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
    uint64_t before = now();
    CHECK_ST(jam_process_create(job, "ids", 3, 0, &child, &vmar), OK);
    struct process_info ci;
    CHECK_ST(jam_process_get_info(child, &ci), OK);
    CHECK(ci.pid > 0 && ci.pid != pi.pid);
    CHECK_EQ(ci.cpu_ns, 0);   /* never ran */
    /* When it was made (the uptime) and the name it was made with. */
    CHECK(ci.start_ns >= before && ci.start_ns <= now());
    CHECK(!strcmp(ci.name, "ids"));
    CHECK(pi.start_ns > 0 && pi.start_ns < ci.start_ns);   /* we are older */
    CHECK(pi.name[0] && !pi.name[sizeof(pi.name) - 1]);
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
    CHECK(s.mem_free_pages > 0 && s.mem_free_pages <= s.mem_total_pages);
    /* The free figure is the allocator's: 16 MiB committed takes at least
     * half of it from the count, and giving it back puts it back. */
    handle_t vmo;
    CHECK_ST(jam_vmo_create(4096 * PAGE_SIZE, 0, &vmo), OK);
    CHECK_ST(jam_vmo_commit(vmo, 0, 4096 * PAGE_SIZE), OK);
    struct sys_public_info s2;
    CHECK_ST(jam_sys_public_info(&s2), OK);
    CHECK(s2.mem_free_pages + 2048 <= s.mem_free_pages);
    CHECK_EQ(s2.mem_total_pages, s.mem_total_pages);
    CHECK_ST(jam_handle_close(vmo), OK);
    CHECK_ST(jam_sys_public_info(&s2), OK);
    CHECK(s2.mem_free_pages >= s.mem_free_pages - 1024);
    CHECK_ST(jam_sys_public_info(UNMAPPED), ERR_INVALID_ARGS);
    CHECK_ST(jam_sys_public_info((struct sys_public_info *)0xffff800000000000ull),
             ERR_INVALID_ARGS);
    return true;
}

/* ---- process_get_maps ---------------------------------------------------------- */

/* The entry of the list from `from` that starts at base, or NULL. */
static const struct map_info *entry_at(const struct map_info *m, long n, uint64_t base)
{
    for (long i = 0; i < n; i++)
        if (m[i].base == base)
            return &m[i];
    return NULL;
}

/* Our own mappings, as the call lists them: what we map shows with its
 * VMO, offset, permissions and pages present; the list is in order and as
 * long as process_info says; a process that never ran lists what is mapped
 * for it, with nothing present; the refusals. */
bool t_process_maps(void)
{
    handle_t self = startup_handle(SR_SELF_PROCESS), vmar = startup_handle(SR_SELF_VMAR);
    handle_t a, b;
    CHECK_ST(jam_vmo_create(8 * PAGE_SIZE, 0, &a), OK);
    CHECK_ST(jam_vmo_create(4 * PAGE_SIZE, 0, &b), OK);
    uint64_t pa = 0, pb = 0;
    CHECK_ST(jam_vmar_map(vmar, a, 0, 8 * PAGE_SIZE, VMAR_READ | VMAR_WRITE, &pa), OK);
    CHECK_ST(jam_vmar_map(vmar, b, PAGE_SIZE, 2 * PAGE_SIZE, VMAR_READ, &pb), OK);
    struct handle_info ia, ib;
    CHECK_ST(jam_handle_info(a, &ia), OK);
    CHECK_ST(jam_handle_info(b, &ib), OK);

    struct map_info m[8];
    long n = jam_process_get_maps(self, pa, m, 8);
    CHECK(n >= 1);
    CHECK_EQ(m[0].base, pa);   /* the first that ends after pa is a's */
    CHECK_EQ(m[0].len, 8 * PAGE_SIZE);
    CHECK_EQ(m[0].vmo_koid, ia.koid);
    CHECK_EQ(m[0].vmo_off, 0);
    CHECK_EQ(m[0].perms, VMAR_READ | VMAR_WRITE);
    CHECK_EQ(m[0].resident, 0);   /* nothing touched */
    CHECK_EQ(m[0].flags, 0);

    /* Pages present follow the touches: two of a's, one of b's. */
    volatile uint8_t *wa = (volatile uint8_t *)(uintptr_t)pa;
    wa[PAGE_SIZE] = 1;
    wa[5 * PAGE_SIZE] = 1;
    volatile uint8_t *rb = (volatile uint8_t *)(uintptr_t)pb;
    (void)rb[0];
    n = jam_process_get_maps(self, pa, m, 8);
    CHECK(n >= 1 && m[0].base == pa);
    CHECK_EQ(m[0].resident, 2);
    const struct map_info *mb = entry_at(m, n, pb);
    if (!mb) {   /* more mappings than 8 between them: ask from b */
        n = jam_process_get_maps(self, pb, m, 8);
        mb = entry_at(m, n, pb);
    }
    CHECK(mb != NULL);
    CHECK_EQ(mb->len, 2 * PAGE_SIZE);
    CHECK_EQ(mb->vmo_koid, ib.koid);
    CHECK_EQ(mb->vmo_off, PAGE_SIZE);
    CHECK_EQ(mb->perms, VMAR_READ);
    CHECK_EQ(mb->resident, 1);

    /* A protect splits a's mapping; one to no access takes its pages out of the count. */
    CHECK_ST(jam_vmar_protect(vmar, pa + PAGE_SIZE, PAGE_SIZE, VMAR_READ), OK);
    n = jam_process_get_maps(self, pa, m, 8);
    CHECK(n >= 3);
    CHECK_EQ(m[0].len, PAGE_SIZE);
    CHECK_EQ(m[0].resident, 0);
    CHECK_EQ(m[1].base, pa + PAGE_SIZE);
    CHECK_EQ(m[1].len, PAGE_SIZE);
    CHECK_EQ(m[1].perms, VMAR_READ);
    CHECK_EQ(m[1].resident, 1);
    CHECK_EQ(m[2].base, pa + 2 * PAGE_SIZE);
    CHECK_EQ(m[2].resident, 1);   /* page 5 is in this one's range */
    CHECK_ST(jam_vmar_protect(vmar, pa + 4 * PAGE_SIZE, 4 * PAGE_SIZE, 0), OK);
    n = jam_process_get_maps(self, pa + 4 * PAGE_SIZE, m, 8);
    CHECK(n >= 1 && m[0].base == pa + 4 * PAGE_SIZE && m[0].perms == 0 && m[0].resident == 0);

    /* The whole list: in order, no overlap, as many as process_info counts. */
    for (unsigned try = 0; try < 4; try++) {
        struct process_info pi;
        CHECK_ST(jam_process_get_info(self, &pi), OK);
        uint64_t from = 0, last_end = 0;
        long total = 0;
        for (;;) {
            n = jam_process_get_maps(self, from, m, 8);
            CHECK(n >= 0 && n <= 8);
            for (long i = 0; i < n; i++) {
                CHECK(m[i].len > 0 && !(m[i].base & (PAGE_SIZE - 1)) && !(m[i].len & (PAGE_SIZE - 1)));
                CHECK(m[i].base >= last_end);
                last_end = m[i].base + m[i].len;
            }
            total += n;
            if (n < 8)
                break;
            from = last_end;
        }
        if (total == pi.mappings)
            break;
        CHECK(try < 3);   /* a thread of ours mapped meanwhile: three tries */
    }

    /* A process that never ran: what is mapped into it is listed, none of it present. */
    handle_t job, child, cvmar;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_process_create(job, "maps", 4, 0, &child, &cvmar), OK);
    CHECK_EQ(jam_process_get_maps(child, 0, m, 8), 0);
    uint64_t pc = 0;
    CHECK_ST(jam_vmar_map(cvmar, a, 0, 2 * PAGE_SIZE, VMAR_READ, &pc), OK);
    CHECK_EQ(jam_process_get_maps(child, 0, m, 8), 1);
    CHECK(m[0].base == pc && m[0].len == 2 * PAGE_SIZE && m[0].vmo_koid == ia.koid &&
          m[0].resident == 0);   /* the VMO has pages; this address space has none yet */
    CHECK_EQ(jam_process_get_maps(child, pc + 2 * PAGE_SIZE, m, 8), 0);
    CHECK_ST(jam_handle_close(cvmar), OK);
    CHECK_ST(jam_handle_close(child), OK);
    CHECK_ST(jam_handle_close(job), OK);

    /* Unmapped, they are gone. */
    CHECK_ST(jam_vmar_unmap(vmar, pa, 8 * PAGE_SIZE), OK);
    CHECK_ST(jam_vmar_unmap(vmar, pb, 2 * PAGE_SIZE), OK);
    n = jam_process_get_maps(self, pa, m, 8);
    CHECK(n >= 0 && (n == 0 || m[0].base > pb));
    CHECK(!entry_at(m, n, pa) && !entry_at(m, n, pb));

    /* The refusals. */
    CHECK_EQ(jam_process_get_maps(self, 0, m, 0), ERR_OUT_OF_RANGE);
    CHECK_EQ(jam_process_get_maps(self, 0, m, MAP_INFO_MAX + 1), ERR_OUT_OF_RANGE);
    CHECK_EQ(jam_process_get_maps(HANDLE_INVALID, 0, m, 8), ERR_BAD_HANDLE);
    CHECK_EQ(jam_process_get_maps(a, 0, m, 8), ERR_WRONG_TYPE);
    CHECK_EQ(jam_process_get_maps(self, 0, (struct map_info *)UNMAPPED, 8), ERR_INVALID_ARGS);
    CHECK_EQ(jam_process_get_maps(self, UINT64_MAX, m, 8), 0);
    handle_t blind;
    CHECK_ST(jam_handle_duplicate(self, RIGHT_WAIT | RIGHT_DUPLICATE, &blind), OK);
    CHECK_EQ(jam_process_get_maps(blind, 0, m, 8), ERR_ACCESS_DENIED);   /* no RIGHT_INSPECT */
    CHECK_ST(jam_handle_close(blind), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    return true;
}

/* VMAR_REPLACE (MAP_FIXED's call): refused without VMAR_FIXED and with
 * VMAR_KEPT_ONLY, with the caller's address word as it was; over the
 * second page of a mapping it replaces that page in one call and answers
 * with the address it was given. */
bool t_vmar_replace_calls(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), a, b;
    CHECK_ST(jam_vmo_create(2 * PAGE_SIZE, 0, &a), OK);
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &b), OK);
    uint64_t addr = 0;
    CHECK_ST(jam_vmar_map(vmar, a, 0, 2 * PAGE_SIZE, VMAR_READ | VMAR_WRITE, &addr), OK);
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    p[0] = 1;
    p[PAGE_SIZE / 4] = 2;
    uint64_t want = addr + PAGE_SIZE, at = want;
    CHECK_ST(jam_vmar_map(vmar, b, 0, PAGE_SIZE, VMAR_READ | VMAR_REPLACE, &at),
             ERR_INVALID_ARGS);
    CHECK_EQ(at, want);
    CHECK_ST(jam_vmar_map(vmar, b, 0, PAGE_SIZE,
                          VMAR_READ | VMAR_FIXED | VMAR_REPLACE | VMAR_KEPT_ONLY, &at),
             ERR_INVALID_ARGS);
    CHECK_EQ(at, want);
    CHECK_EQ(p[PAGE_SIZE / 4], 2);   /* nothing changed */
    CHECK_ST(jam_vmar_map(vmar, b, 0, PAGE_SIZE,
                          VMAR_READ | VMAR_WRITE | VMAR_FIXED | VMAR_REPLACE, &at),
             OK);
    CHECK_EQ(at, want);
    CHECK_EQ(p[0], 1);
    CHECK_EQ(p[PAGE_SIZE / 4], 0);   /* b's page now */
    CHECK_ST(jam_vmar_unmap(vmar, addr, 2 * PAGE_SIZE), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
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
