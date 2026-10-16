/* utest: libjam's fork (<os.h> jam_fork, user/lib/fork.c; docs/FORK-PLAN.md
 * section 1, track FL1a) and the raw vmar_clone's arguments. Each test
 * runs a mode of ours ("utest fork-<what>") in a job of its own, which
 * forks, checks both sides and waits for its children: the job holds the
 * forking program and its children only, so its counts are theirs.
 *
 *   fork_memory: both sides return (the child 0, the parent a process
 *     handle); the child sees our memory as it was at the call (data, bss,
 *     heap, stack, a VMO's mapping) and our later writes never, we never
 *     see its writes, and a mapping made VMAR_CLONE_SHARE is one both
 *     ways; MXCSR, the x87 control word and the FS base are the caller's.
 *   fork_threads: threads hammering the heap and the namespace while we
 *     fork 16 times: each child has one thread, and malloc, free and a
 *     namespace lookup work there (no lock left held by a thread that
 *     isn't there).
 *   fork_handles: the child holds exactly what was passed (its job's
 *     handle count), its roles are the list's (its own process, address
 *     space and thread; our job, bootfs and namespace; a signal channel),
 *     its namespace's entries reach our entries' objects, and no value of
 *     ours it didn't get reaches our object.
 *   fork_nomem: a job that can't pay for the copy gets ERR_NO_MEMORY and
 *     nothing is left (pages, handles, threads as before); once it can,
 *     the fork works, the child's charge shows while it lives and is back
 *     once it has gone.
 *   fork_jit: with the JIT power (as our SR_USER): the child gets no power
 *     and no root, our writable JIT mapping isn't in it, and our
 *     execute-only one runs there and can't be made writable (CJR's
 *     finding 1, FORK-PLAN section 1.10).
 *   fork_vmar_clone_args: vmar_clone's arrays: n past VMAR_CLONE_MAX,
 *     flags, pointers that are NULL, unmapped, the kernel's, past the user
 *     range's top or read-only, handles that aren't VMOs of ours mapped as
 *     copies, a dst that isn't an empty unstarted process of ours: each
 *     refused with nothing made and no handle left; then n = 2 naming one
 *     VMO twice gives two handles to one child. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define PG         PAGE_SIZE
#define RW         (VMAR_READ | VMAR_WRITE)
#define WAIT       (10 * NS_PER_S)
#define TLS_MAGIC  0x5f5f7f7f1234abcdull
#define MXCSR_RC   0x6000u    /* MXCSR's rounding: toward zero */
#define FPUCW_RC   0x0c00u    /* the x87 control word's, the same */

/* In a mode: a check that fails prints its line and ends the mode with 1. */
static int failed(int line, const char *what)
{
    printf("utest-fork: FAILED at line %d: %s\n", line, what);
    return 1;
}
#define MUST(c)                                                             \
    do {                                                                    \
        if (!(c))                                                           \
            return failed(__LINE__, #c);                                    \
    } while (0)
#define MUST_ST(expr, want)                                                 \
    do {                                                                    \
        status_t _s = (expr);                                               \
        if (_s != (want)) {                                                 \
            printf("utest-fork: %s is %s\n", #expr, status_str(_s));        \
            return failed(__LINE__, #want);                                 \
        }                                                                   \
    } while (0)

static uint64_t koid_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.koid : 0;
}

/* A value of ours the child wasn't given reaches nothing of ours there:
 * no handle (almost always: its table's generations are random), or a
 * handle to one of its own objects. */
static bool stale(handle_t h, uint64_t our_koid)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) != OK || hi.koid != our_koid;
}

static uint64_t job_used(uint32_t kind)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[kind] : UINT64_MAX;
}

/* One message on ch within WAIT: its first byte. */
static status_t recv_byte(handle_t ch, char *c)
{
    signals_t seen;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + WAIT, &seen);
    uint32_t n = 0;
    struct channel_read_args r = {
        .h = ch, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)c,
        .rd_actual = (uint64_t)(uintptr_t)&n,
    };
    return st == OK ? jam_channel_read(&r) : st;
}

/* Wait for a child of ours to end: OK if it exited with 0. */
static status_t reap(handle_t proc)
{
    struct process_info info;
    status_t st = spawn_wait(proc, WAIT, &info);
    if (st == OK && (info.killed || info.exit_code != 0)) {
        printf("utest-fork: a child ended with %ld%s\n", (long)info.exit_code,
               info.killed ? " (killed)" : "");
        st = ERR_INTERNAL;
    }
    jam_handle_close(proc);
    return st;
}

/* ---- fork-memory -------------------------------------------------------------- */

static volatile uint64_t g_data = 0x1111;
static volatile uint64_t g_bss;

struct tls_block {
    uint64_t self;    /* %fs:0, as a C library's thread pointer */
    uint64_t magic;
};

struct mem_test {
    volatile uint8_t  *heap;     /* malloc's */
    volatile uint64_t *copy;     /* a VMO's mapping: a copy in the child */
    volatile uint64_t *shared;   /* one mapped VMAR_CLONE_SHARE: the same pages */
    volatile uint64_t *stack;    /* a word of the forking function's frame */
    uint64_t           koid;     /* our process's */
    uint32_t           pid;
};

static uint16_t fpucw(void)
{
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

static void set_fpucw(uint16_t cw)
{
    __asm__ volatile("fldcw %0" : : "m"(cw));
}

static uint64_t fs_word(uint64_t i)
{
    uint64_t v;
    __asm__ volatile("mov %%fs:(%1), %0" : "=r"(v) : "r"(i * 8));
    return v;
}

/* Everything as it was at the fork. */
static bool at_fork(const struct mem_test *t)
{
    return g_data == 0x2222 && g_bss == 0x3333 && t->heap[0] == 0xb2 && t->heap[PG] == 0xb3 &&
           t->copy[0] == 0x4444 && t->copy[PG / 8] == 0x4445 && *t->stack == 0x5555;
}

static int memory_child(const struct mem_test *t, const struct tls_block *tls, uint32_t mx,
                        uint16_t cw)
{
    MUST((mx & MXCSR_RC) == MXCSR_RC && (cw & FPUCW_RC) == FPUCW_RC);
    MUST(fs_word(0) == (uint64_t)(uintptr_t)tls && fs_word(1) == TLS_MAGIC);
    struct process_info pi;
    MUST_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    MUST(pi.koid != t->koid && pi.pid != t->pid && pi.threads == 1);
    MUST(at_fork(t));
    char c = 0;
    MUST_ST(recv_byte(startup_handle(SR_SIGNALS), &c), OK);   /* the parent has written */
    MUST(c == 'w');
    MUST(at_fork(t));
    g_data = 0x9999;
    t->heap[0] = 0xc3;
    t->copy[0] = 0x9998;
    *t->stack = 0x9997;
    MUST(g_data == 0x9999 && t->heap[0] == 0xc3 && t->copy[0] == 0x9998 && *t->stack == 0x9997);
    *t->shared = 0x4242;
    void *p = malloc(3 * PG);
    MUST(p);
    memset(p, 1, 3 * PG);
    free(p);
    return 0;
}

static int fork_memory(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), v, s;
    uint64_t copy_at = 0, shared_at = 0;
    MUST_ST(jam_vmo_create(2 * PG, 0, &v), OK);
    MUST_ST(jam_vmar_map(vmar, v, 0, 2 * PG, RW, &copy_at), OK);
    MUST_ST(jam_vmo_create(PG, 0, &s), OK);
    MUST_ST(jam_vmar_map(vmar, s, 0, PG, RW | VMAR_CLONE_SHARE, &shared_at), OK);
    volatile uint64_t word = 0;
    struct process_info pi;
    MUST_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    struct mem_test t = {
        .heap = malloc(2 * PG), .copy = (volatile uint64_t *)(uintptr_t)copy_at,
        .shared = (volatile uint64_t *)(uintptr_t)shared_at, .stack = &word, .koid = pi.koid,
        .pid = pi.pid,
    };
    MUST(t.heap);
    g_data = 0x2222;
    g_bss = 0x3333;
    t.heap[0] = 0xb2;
    t.heap[PG] = 0xb3;
    t.copy[0] = 0x4444;
    t.copy[PG / 8] = 0x4445;
    word = 0x5555;
    *t.shared = 1;
    static struct tls_block tls;
    tls = (struct tls_block){ .self = (uint64_t)(uintptr_t)&tls, .magic = TLS_MAGIC };
    MUST_ST(jam_thread_set_fs_base((uint64_t)(uintptr_t)&tls), OK);
    uint32_t mx0 = __builtin_ia32_stmxcsr();
    uint16_t cw0 = fpucw();
    __builtin_ia32_ldmxcsr(mx0 | MXCSR_RC);
    set_fpucw((uint16_t)(cw0 | FPUCW_RC));

    handle_t sig = HANDLE_INVALID;
    struct jam_fork_args a = { .fs_base = (uint64_t)(uintptr_t)&tls, .signals = &sig };
    int64_t r = jam_fork(&a);
    uint32_t mx = __builtin_ia32_stmxcsr();
    uint16_t cw = fpucw();
    __builtin_ia32_ldmxcsr(mx0);
    set_fpucw(cw0);
    if (r == 0)
        return memory_child(&t, &tls, mx, cw);
    jam_thread_set_fs_base(0);
    MUST(r > 0);
    MUST((mx & MXCSR_RC) == MXCSR_RC);   /* ours unchanged by the call */
    handle_t proc = (handle_t)r;
    struct handle_info hi;
    MUST_ST(jam_handle_info(proc, &hi), OK);
    MUST(hi.type == OBJ_PROCESS && hi.koid != t.koid);
    MUST(sig != HANDLE_INVALID);
    MUST(at_fork(&t));
    g_data = 0x7777;
    t.heap[0] = 0x77;
    t.copy[0] = 0x7776;
    word = 0x7775;
    MUST_ST(jam_channel_write(sig, "w", 1, NULL, 0), OK);
    MUST_ST(reap(proc), OK);
    MUST(g_data == 0x7777 && t.heap[0] == 0x77 && t.copy[0] == 0x7776 && word == 0x7775);
    MUST(g_bss == 0x3333 && t.heap[PG] == 0xb3 && t.copy[PG / 8] == 0x4445);
    MUST(*t.shared == 0x4242);   /* the child's write, in the pages we share */
    free((void *)t.heap);
    jam_handle_close(sig);
    jam_vmar_unmap(vmar, copy_at, 2 * PG);
    jam_vmar_unmap(vmar, shared_at, PG);
    jam_handle_close(v);
    jam_handle_close(s);
    return 0;
}

/* ---- fork-threads ----------------------------------------------------------------- */

#define THREAD_FORKS 16

static volatile bool stop;

/* Takes and gives the heap's lock as fast as it can. */
static void heap_hammer(void *arg)
{
    (void)arg;
    for (unsigned i = 0; !stop; i++) {
        void *p = malloc(64 + (i % 64) * 64);
        if (p)
            memset(p, (int)i, 64);
        free(p);
    }
}

/* Takes and gives the namespace's lock as fast as it can. */
static void ns_hammer(void *arg)
{
    (void)arg;
    while (!stop) {
        handle_t h;
        if (ns_channel("/fk", &h) == OK)
            jam_handle_close(h);
    }
}

static int threads_child(uint64_t mounted)
{
    struct process_info pi;
    MUST_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    MUST(pi.threads == 1);
    for (unsigned i = 0; i < 64; i++) {
        void *p = malloc(64 + i * 64);
        MUST(p);
        free(p);
    }
    handle_t h;
    MUST_ST(ns_channel("/fk", &h), OK);
    MUST(koid_of(h) == mounted);
    jam_handle_close(h);
    return 0;
}

static int fork_threads(void)
{
    handle_t ends[2];
    MUST_ST(jam_channel_create(&ends[0], &ends[1]), OK);
    uint64_t mounted = koid_of(ends[0]);
    MUST_ST(ns_mount("/fk", ends[0]), OK);
    static uint8_t stacks[2][16384] __attribute__((aligned(16)));
    handle_t th[2];
    stop = false;
    MUST_ST(thread_spawn("heap", heap_hammer, NULL, stacks[0], sizeof(stacks[0]), &th[0]), OK);
    MUST_ST(thread_spawn("ns", ns_hammer, NULL, stacks[1], sizeof(stacks[1]), &th[1]), OK);
    for (unsigned i = 0; i < THREAD_FORKS; i++) {
        int64_t r = jam_fork(NULL);
        if (r == 0)
            return threads_child(mounted);
        if (r < 0)
            stop = true;
        MUST(r > 0);
        MUST_ST(reap((handle_t)r), OK);
    }
    stop = true;
    signals_t seen;
    for (unsigned i = 0; i < 2; i++) {
        MUST_ST(jam_object_wait_one(th[i], SIG_TERMINATED, now() + WAIT, &seen), OK);
        jam_handle_close(th[i]);
    }
    MUST_ST(ns_unmount("/fk"), OK);
    jam_handle_close(ends[1]);
    return 0;
}

/* ---- fork-handles ----------------------------------------------------------------- */

struct handles_test {
    handle_t ev[3];          /* events, not passed */
    uint64_t ev_koid[3];
    handle_t other[2];       /* the other ends of what /fk and /svc/fk hold: not passed */
    uint64_t other_koid[2];
    uint64_t mount_koid;     /* the objects /fk and /svc/fk name */
    uint64_t svc_koid;
    handle_t self;           /* our SR_SELF_PROCESS: its value names nothing of ours there */
    uint64_t self_koid;
    uint64_t job_koid, bootfs_koid;
    uint64_t used;           /* our job's handles just before the fork */
};

/* The VMOs vmar_clone made for proc, one for each VMO of the source's
 * mapped as a copy (two mappings of one VMO map one child): each VMO
 * charges its job a handle unit, as a process does. */
static unsigned clone_vmos(handle_t proc)
{
    uint64_t seen[256];
    unsigned n = 0;
    struct map_info mi[16];
    uint64_t from = 0;
    int64_t got;
    while ((got = jam_process_get_maps(proc, from, mi, 16)) > 0)
        for (int64_t i = 0; i < got; i++) {
            from = mi[i].base + mi[i].len;
            bool again = false;
            for (unsigned j = 0; j < n && !again; j++)
                again = seen[j] == mi[i].vmo_koid;
            if (!(mi[i].flags & (VMAR_CLONE_SHARE | VMAR_CLONE_NONE)) && !again && n < 256)
                seen[n++] = mi[i].vmo_koid;
        }
    return n;
}

static int handles_child(const struct handles_test *t)
{
    char c = 0;
    MUST_ST(recv_byte(startup_handle(SR_SIGNALS), &c), OK);   /* the parent holds still now */
    MUST(c == 'q');
    /* The roles: exactly the list's for a program with no stdout,
     * console or image of its own (utest's modes have none). */
    static const uint32_t want[] = { SR_SELF_PROCESS, SR_SELF_VMAR, SR_SELF_THREAD, SR_JOB,
                                     SR_BOOTFS, SR_NS, SR_SIGNALS };
    unsigned nroles = startup_handle_count();
    MUST(nroles == sizeof(want) / sizeof(want[0]));
    for (unsigned i = 0; i < nroles; i++)
        MUST(startup_handle(want[i]) != HANDLE_INVALID);
    struct handle_info hi;
    struct process_info pi;
    MUST_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    MUST(pi.koid != t->self_koid && koid_of(startup_handle(SR_SELF_PROCESS)) == pi.koid);
    MUST_ST(jam_handle_info(startup_handle(SR_SELF_THREAD), &hi), OK);
    MUST(hi.type == OBJ_THREAD && hi.related_koid == pi.koid);
    MUST_ST(jam_handle_info(startup_handle(SR_SELF_VMAR), &hi), OK);
    MUST(hi.type == OBJ_VMAR);
    MUST(koid_of(startup_handle(SR_JOB)) == t->job_koid);
    MUST(koid_of(startup_handle(SR_BOOTFS)) == t->bootfs_koid);
    MUST_ST(jam_handle_info(startup_handle(SR_BOOTFS), &hi), OK);
    MUST(!(hi.rights & RIGHT_WRITE));
    /* The namespace: our entries' objects, through handles of its own. */
    handle_t h;
    MUST_ST(ns_channel("/fk", &h), OK);
    MUST(koid_of(h) == t->mount_koid);
    jam_handle_close(h);
    MUST_ST(svc_open("fk", &h), OK);
    MUST(koid_of(h) == t->svc_koid);
    jam_handle_close(h);
    /* svc_get's cache: ours held our value, which names nothing here. */
    MUST(koid_of(svc_get("fk")) == t->svc_koid);
    char name[NS_NAME_MAX];
    unsigned nns = 0;
    for (; ns_mount_at(nns, name); nns++)
        ;
    MUST(nns == 1);
    for (unsigned i = 0; ns_svc_at(i, name); i++)
        nns++;
    MUST(nns == 2);
    /* Nothing of ours it wasn't given. */
    for (unsigned i = 0; i < 3; i++)
        MUST(stale(t->ev[i], t->ev_koid[i]));
    for (unsigned i = 0; i < 2; i++)
        MUST(stale(t->other[i], t->other_koid[i]));
    MUST(stale(t->self, t->self_koid));
    /* And nothing else. The job holds the parent, with its handles of
     * before the fork, ours and the signal channel's client end; our
     * process (a unit while it lives) and its copies of the parent's VMOs;
     * and our handles: the startup channel, the roles, the namespace's
     * entries and svc_get's channel. */
    unsigned vmos = clone_vmos(startup_handle(SR_SELF_PROCESS));
    uint64_t used = job_used(JOB_LIMIT_HANDLES);
    uint64_t expect = t->used + 2 + 1 + vmos + 1 + nroles + nns + 1;
    if (used != expect)
        printf("utest-fork: the job holds %lu handles, want %lu (%lu before the fork, %u "
               "VMOs, %u roles, %u entries)\n", (unsigned long)used, (unsigned long)expect,
               (unsigned long)t->used, vmos, nroles, nns);
    MUST(used == expect);
    return 0;
}

static int fork_handles(void)
{
    struct handles_test t = { .self = startup_handle(SR_SELF_PROCESS) };
    for (unsigned i = 0; i < 3; i++) {
        MUST_ST(jam_event_create(&t.ev[i]), OK);
        t.ev_koid[i] = koid_of(t.ev[i]);
    }
    handle_t m, s;
    MUST_ST(jam_channel_create(&m, &t.other[0]), OK);
    MUST_ST(jam_channel_create(&s, &t.other[1]), OK);
    for (unsigned i = 0; i < 2; i++)
        t.other_koid[i] = koid_of(t.other[i]);
    t.mount_koid = koid_of(m);
    t.svc_koid = koid_of(s);
    MUST_ST(ns_mount("/fk", m), OK);
    MUST_ST(ns_svc_set("fk", s, false), OK);
    MUST(koid_of(svc_get("fk")) == t.svc_koid);   /* kept by svc_get: our value */
    t.self_koid = koid_of(t.self);
    t.job_koid = koid_of(startup_handle(SR_JOB));
    t.bootfs_koid = koid_of(startup_handle(SR_BOOTFS));
    MUST(t.self_koid && t.job_koid && t.bootfs_koid);
    MUST(!startup_handle(SR_RESOURCE) && !startup_handle(SR_USER));
    handle_t sig = HANDLE_INVALID;
    struct jam_fork_args a = { .signals = &sig };
    free(malloc(64));   /* our heap made now: its VMO counts in our job too */
    t.used = job_used(JOB_LIMIT_HANDLES);
    int64_t r = jam_fork(&a);
    if (r == 0)
        return handles_child(&t);
    MUST(r > 0);
    MUST_ST(jam_channel_write(sig, "q", 1, NULL, 0), OK);
    MUST_ST(reap((handle_t)r), OK);
    jam_handle_close(sig);
    MUST(job_used(JOB_LIMIT_HANDLES) == t.used);
    for (unsigned i = 0; i < 3; i++)
        jam_handle_close(t.ev[i]);
    MUST_ST(ns_unmount("/fk"), OK);
    MUST_ST(ns_svc_remove("fk"), OK);
    jam_handle_close(t.other[0]);
    jam_handle_close(t.other[1]);
    return 0;
}

/* ---- fork-nomem --------------------------------------------------------------------- */

static int fork_nomem(void)
{
    handle_t job = startup_handle(SR_JOB), vmar = startup_handle(SR_SELF_VMAR), v;
    struct job_info ji;
    MUST_ST(jam_job_get_info(job, &ji), OK);
    uint64_t room = ji.limit[JOB_LIMIT_PAGES] - ji.used[JOB_LIMIT_PAGES];
    MUST(ji.limit[JOB_LIMIT_PAGES] != JOB_NO_LIMIT && room > 512);
    /* Three quarters of what is left, committed and mapped as a copy: a
     * fork must pay for all of it again. */
    uint64_t size = room * 3 / 4 * PG, at = 0;
    MUST_ST(jam_vmo_create(size, 0, &v), OK);
    MUST_ST(jam_vmo_commit(v, 0, size), OK);
    MUST_ST(jam_vmar_map(vmar, v, 0, size, RW, &at), OK);
    MUST(jam_fork(NULL) == ERR_NO_MEMORY);   /* the stack pages its calls touch, once */
    MUST_ST(jam_job_get_info(job, &ji), OK);
    MUST(jam_fork(NULL) == ERR_NO_MEMORY);
    /* Nothing left: what the refused fork made goes as its last handle
     * closes (allow its teardown a moment). */
    struct job_info after;
    uint64_t until = now() + WAIT;
    do
        MUST_ST(jam_job_get_info(job, &after), OK);
    while (memcmp(after.used, ji.used, sizeof(ji.used)) && now() < until &&
           jam_sleep_until(now() + NS_PER_MS) == OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (after.used[k] != ji.used[k]) {
            printf("utest-fork: kind %u: %lu used before the refused fork, %lu after\n", k,
                   (unsigned long)ji.used[k], (unsigned long)after.used[k]);
            MUST(after.used[k] == ji.used[k]);
        }
    /* Room again: the fork works, and its charge goes when the child does. */
    MUST_ST(jam_vmar_unmap(vmar, at, size), OK);
    MUST_ST(jam_handle_close(v), OK);
    for (unsigned round = 0; round < 2; round++) {   /* the first warms up our heap */
        handle_t sig = HANDLE_INVALID;
        struct jam_fork_args a = { .signals = &sig };
        uint64_t before = job_used(JOB_LIMIT_PAGES);
        int64_t r = jam_fork(&a);
        if (r == 0) {
            char c;
            return recv_byte(startup_handle(SR_SIGNALS), &c) == OK ? 0 : 2;
        }
        MUST(r > 0);
        MUST(job_used(JOB_LIMIT_PAGES) > before);   /* the child's copy, paid for now */
        MUST_ST(jam_channel_write(sig, "x", 1, NULL, 0), OK);
        MUST_ST(reap((handle_t)r), OK);
        jam_handle_close(sig);
        if (round)
            MUST(job_used(JOB_LIMIT_PAGES) == before);
    }
    return 0;
}

/* ---- fork-jit ----------------------------------------------------------------------- */

typedef int (*ret_fn)(void);

static int jit_fork_child(uint64_t w_at, uint64_t x_at, const handle_t *ours,
                          const uint64_t *koids)
{
    MUST(!startup_handle(SR_USER) && !startup_handle(SR_RESOURCE));
    for (unsigned i = 0; i < 3; i++)
        MUST(stale(ours[i], koids[i]));
    struct map_info mi;
    int64_t n = jam_process_get_maps(startup_handle(SR_SELF_PROCESS), w_at, &mi, 1);
    MUST(n == 0 || (n == 1 && mi.base > w_at));   /* the writable JIT mapping: none */
    MUST(((ret_fn)(uintptr_t)x_at)() == 42);       /* the execute-only one runs */
    MUST_ST(jam_vmar_protect(startup_handle(SR_SELF_VMAR), x_at, PG, RW), ERR_ACCESS_DENIED);
    return 0;
}

static int fork_jit(void)
{
    handle_t power = startup_handle(SR_USER), vmar = startup_handle(SR_SELF_VMAR), v, x, xr;
    MUST(power);
    MUST_ST(jam_vmo_create(PG, 0, &v), OK);
    MUST_ST(jam_vmo_make_jit(v, power, &x), OK);
    uint64_t w_at = 0, x_at = 0;
    MUST_ST(jam_vmar_map(vmar, x, 0, PG, RW | VMAR_EXEC, &w_at), OK);
    volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)w_at;
    code[0] = 0xb8;   /* mov eax, 42; ret */
    code[1] = 42;
    code[2] = code[3] = code[4] = 0;
    code[5] = 0xc3;
    MUST_ST(jam_handle_duplicate(x, RIGHT_READ | RIGHT_EXEC | RIGHT_MAP | RIGHT_DUPLICATE, &xr),
            OK);
    MUST_ST(jam_vmar_map(vmar, xr, 0, PG, VMAR_READ | VMAR_EXEC, &x_at), OK);
    MUST(((ret_fn)(uintptr_t)x_at)() == 42);
    const handle_t ours[3] = { power, x, xr };
    const uint64_t koids[3] = { koid_of(power), koid_of(x), koid_of(xr) };
    int64_t r = jam_fork(NULL);
    if (r == 0)
        return jit_fork_child(w_at, x_at, ours, koids);
    MUST(r > 0);
    MUST_ST(reap((handle_t)r), OK);
    MUST(((ret_fn)(uintptr_t)w_at)() == 42);   /* ours untouched */
    jam_vmar_unmap(vmar, w_at, PG);
    jam_vmar_unmap(vmar, x_at, PG);
    jam_handle_close(xr);
    jam_handle_close(x);
    jam_handle_close(v);
    return 0;
}

/* ---- fork-clone-args ---------------------------------------------------------------- */

#define USER_TOP 0x00007fffffffe000ull
#define KERNEL   0xffff800000001000ull

/* A dst left as it was: not started, nothing mapped; and our job's handles
 * as they were. */
static bool untouched(handle_t dst, uint64_t used)
{
    struct process_info pi;
    return jam_process_get_info(dst, &pi) == OK && pi.state == PROCESS_NEW && !pi.mappings &&
           job_used(JOB_LIMIT_HANDLES) == used;
}

static int fork_clone_args(void)
{
    handle_t job = startup_handle(SR_JOB), vmar = startup_handle(SR_SELF_VMAR);
    handle_t dst, dvmar, v, s, u, ro, ev, nodup, dst_ro;
    uint64_t at = 0, s_at = 0, ro_at = 0;
    MUST_ST(jam_process_create(job, "utest-clone-dst", 15, 0, &dst, &dvmar), OK);
    MUST_ST(jam_vmo_create(PG, 0, &v), OK);
    MUST_ST(jam_vmar_map(vmar, v, 0, PG, RW, &at), OK);
    MUST_ST(jam_vmo_create(PG, 0, &s), OK);
    MUST_ST(jam_vmar_map(vmar, s, 0, PG, RW | VMAR_CLONE_SHARE, &s_at), OK);
    MUST_ST(jam_vmo_create(PG, 0, &u), OK);   /* mapped nowhere */
    MUST_ST(jam_vmo_create(2 * PG, 0, &ro), OK);   /* a page read-only, and a hole after it */
    MUST_ST(jam_vmar_map(vmar, ro, 0, 2 * PG, VMAR_READ, &ro_at), OK);
    MUST_ST(jam_vmar_unmap(vmar, ro_at + PG, PG), OK);
    MUST_ST(jam_event_create(&ev), OK);
    MUST_ST(jam_handle_duplicate(v, RIGHT_READ | RIGHT_MAP, &nodup), OK);
    MUST_ST(jam_handle_duplicate(dst, RIGHT_READ | RIGHT_WAIT, &dst_ro), OK);
    handle_t vm[2] = { v, v }, ch[2] = { 0, 0 };
    uint64_t used = job_used(JOB_LIMIT_HANDLES);

    /* n, flags */
    MUST_ST(jam_vmar_clone(dst, vm, ch, VMAR_CLONE_MAX + 1, 0), ERR_OUT_OF_RANGE);
    MUST_ST(jam_vmar_clone(dst, vm, ch, UINT32_MAX, 0), ERR_OUT_OF_RANGE);
    MUST_ST(jam_vmar_clone(dst, NULL, NULL, 0, 1), ERR_INVALID_ARGS);
    MUST_ST(jam_vmar_clone(dst, vm, ch, 1, 1u << 31), ERR_INVALID_ARGS);
    MUST(untouched(dst, used));
    /* vmos: NULL, unmapped, the kernel's, across the user range's top, an
     * array running off its mapping (our read-only page's end) */
    const handle_t *bad_in[] = {
        NULL, (const handle_t *)8, (const handle_t *)(uintptr_t)KERNEL,
        (const handle_t *)(uintptr_t)(USER_TOP - 4),
        (const handle_t *)(uintptr_t)(ro_at + PG - 4),
    };
    for (unsigned i = 0; i < sizeof(bad_in) / sizeof(bad_in[0]); i++) {
        MUST_ST(jam_vmar_clone(dst, bad_in[i], ch, 2, 0), ERR_INVALID_ARGS);
        MUST(untouched(dst, used));
    }
    MUST_ST(jam_vmar_clone(dst, (const handle_t *)(uintptr_t)ro_at, ch, 1025, 0),
            ERR_INVALID_ARGS);   /* 4 KiB of zeros, then nothing mapped */
    MUST(untouched(dst, used));
    /* children: NULL, unmapped, the kernel's, read-only (our code, our
     * read-only page), across the top */
    handle_t *bad_out[] = {
        NULL, (handle_t *)8, (handle_t *)(uintptr_t)KERNEL,
        (handle_t *)(uintptr_t)fork_clone_args, (handle_t *)(uintptr_t)ro_at,
        (handle_t *)(uintptr_t)(USER_TOP - 4),
    };
    for (unsigned i = 0; i < sizeof(bad_out) / sizeof(bad_out[0]); i++) {
        MUST_ST(jam_vmar_clone(dst, vm, bad_out[i], 2, 0), ERR_INVALID_ARGS);
        MUST(untouched(dst, used));
    }
    /* what vmos names: not mapped, mapped shared, not a VMO, nothing, no
     * RIGHT_DUPLICATE; the bad one second, after a good one */
    const struct {
        handle_t h;
        status_t want;
    } bad_vmo[] = {
        { u, ERR_INVALID_ARGS }, { s, ERR_INVALID_ARGS }, { ev, ERR_WRONG_TYPE },
        { (60000u << 15) | 1, ERR_BAD_HANDLE }, { nodup, ERR_ACCESS_DENIED },
    };
    for (unsigned i = 0; i < sizeof(bad_vmo) / sizeof(bad_vmo[0]); i++) {
        handle_t two[2] = { v, bad_vmo[i].h };
        MUST_ST(jam_vmar_clone(dst, two, ch, 2, 0), bad_vmo[i].want);
        MUST(untouched(dst, used));
    }
    /* dst: ours (started), no RIGHT_WRITE, not a process */
    MUST_ST(jam_vmar_clone(startup_handle(SR_SELF_PROCESS), NULL, NULL, 0, 0), ERR_BAD_STATE);
    MUST_ST(jam_vmar_clone(dst_ro, NULL, NULL, 0, 0), ERR_ACCESS_DENIED);
    MUST_ST(jam_vmar_clone(dvmar, NULL, NULL, 0, 0), ERR_WRONG_TYPE);
    MUST(untouched(dst, used));

    /* One VMO named twice: one child, two handles to it. */
    MUST_ST(jam_vmar_clone(dst, vm, ch, 2, 0), OK);
    struct handle_info h0, h1;
    MUST_ST(jam_handle_info(ch[0], &h0), OK);
    MUST_ST(jam_handle_info(ch[1], &h1), OK);
    MUST(h0.type == OBJ_VMO && h0.koid == h1.koid && h0.koid != koid_of(v));
    /* and a unit for each VMO it made: one for each of ours mapped as a copy */
    MUST(job_used(JOB_LIMIT_HANDLES) == used + 2 + clone_vmos(dst));
    struct process_info ours, theirs;
    MUST_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &ours), OK);
    MUST_ST(jam_process_get_info(dst, &theirs), OK);
    MUST(theirs.state == PROCESS_NEW && theirs.mappings == ours.mappings);
    MUST_ST(jam_vmar_clone(dst, NULL, NULL, 0, 0), ERR_BAD_STATE);   /* not empty now */
    jam_handle_close(ch[0]);
    jam_handle_close(ch[1]);
    jam_handle_close(dst_ro);
    jam_handle_close(dvmar);
    jam_handle_close(dst);   /* never started: it dies with its last handle */
    /* n = 0 with no arrays, on a fresh one */
    MUST_ST(jam_process_create(job, "utest-clone-dst", 15, 0, &dst, &dvmar), OK);
    MUST_ST(jam_vmar_clone(dst, NULL, NULL, 0, 0), OK);
    jam_handle_close(dvmar);
    jam_handle_close(dst);
    jam_vmar_unmap(vmar, at, PG);
    jam_vmar_unmap(vmar, s_at, PG);
    jam_vmar_unmap(vmar, ro_at, PG);
    handle_t all[] = { v, s, u, ro, ev, nodup };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        jam_handle_close(all[i]);
    return 0;
}

int fork_child_mode(int argc, char **argv)
{
    (void)argc;
    const char *m = argv[1];
    if (!strcmp(m, "fork-memory"))     return fork_memory();
    if (!strcmp(m, "fork-threads"))    return fork_threads();
    if (!strcmp(m, "fork-handles"))    return fork_handles();
    if (!strcmp(m, "fork-nomem"))      return fork_nomem();
    if (!strcmp(m, "fork-jit"))        return fork_jit();
    if (!strcmp(m, "fork-clone-args")) return fork_clone_args();
    return 127;
}

/* ---- the tests ------------------------------------------------------------------------ */

/* Run "utest <mode>" to its end in a job of its own (kind/limit on it; 0:
 * none) with extra as its SR_USER: it exited with 0 and left the job
 * empty (its children all gone). */
static bool run_mode(const char *mode, uint32_t kind, uint64_t limit, handle_t extra)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    if (kind)
        CHECK_ST(jam_job_set_limit(job, kind, limit), OK);
    CHECK_ST(child(mode, NULL, job, extra, &proc), OK);
    struct process_info info = { 0 };
    status_t st = spawn_wait(proc, 30 * NS_PER_S, &info);
    if (st != OK || info.killed || info.exit_code != 0)
        jam_job_kill(job);   /* it, and anything it forked */
    jam_handle_close(proc);
    CHECK_ST(st, OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    struct job_info ji;
    CHECK_ST(jam_job_get_info(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("\"%s\" left %lu units of job kind %u", mode, (unsigned long)ji.used[k], k);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

bool t_fork_memory(void)
{
    return run_mode("fork-memory", 0, 0, HANDLE_INVALID);
}

bool t_fork_threads(void)
{
    return run_mode("fork-threads", 0, 0, HANDLE_INVALID);
}

bool t_fork_handles(void)
{
    return run_mode("fork-handles", 0, 0, HANDLE_INVALID);
}

bool t_fork_nomem(void)
{
    return run_mode("fork-nomem", JOB_LIMIT_PAGES, 8192, HANDLE_INVALID);   /* 32 MiB */
}

bool t_fork_jit(void)
{
    handle_t root = startup_handle(SR_RESOURCE), power;
    if (!root || jam_handle_duplicate(root, RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_ROOT_JIT,
                                      &power) != OK) {
        printf("utest: %s: no RIGHT_ROOT_JIT on our root: skipped\n", utest_cur);
        return true;
    }
    return run_mode("fork-jit", 0, 0, power);   /* power moved */
}

bool t_fork_vmar_clone_args(void)
{
    return run_mode("fork-clone-args", 0, 0, HANDLE_INVALID);
}
