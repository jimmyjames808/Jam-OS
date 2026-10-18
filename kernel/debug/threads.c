/* debug_command "threads <pid|name>": where a process's threads wait
 * (<jam/threadlook.h> has the rules). For a stuck program: each thread's
 * state, its wait note (the futex word, the channel, port or object, the
 * pager VMO's page, a sleep's deadline), the user registers of its last
 * kernel entry, its kernel frames and a user backtrace, all into the
 * kernel log. tools/symbolize-threads.py names the user frames on the host
 * from the program's unstripped binary.
 *
 * The listing is taken in two steps. Under the process's lock, each
 * thread's fields and its own kernel stack's words are copied
 * (thread_look; the lock keeps the stack). Then, with no lock held, the
 * user backtrace reads the process's memory a word at a time through its
 * page tables (aspace_peek): no fault, no wait, nothing held across two
 * words. */
#include <jam/aspace.h>
#include <jam/kprintf.h>
#include <jam/ksyms.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/syscall.h>
#include <jam/syscall_nums.h>
#include <jam/threadlook.h>
#include <jam/time.h>
#include <jam/trap.h>
#include <jam/uentry.h>

#define LOOK_THREADS 64                 /* threads listed per process */
#define FRAME_SPAN   (16ull << 20)      /* a frame record lies this close above rsp */

/* ---- one thread, under its process's lock ------------------------------------- */

/* A kernel stack word at addr, if [addr, addr + 8) lies in [lo, hi). */
static bool kword(uint64_t lo, uint64_t hi, uint64_t addr, uint64_t *out)
{
    if (addr < lo || addr > hi - 8 || (addr & 7))
        return false;
    *out = *(const volatile uint64_t *)addr;
    return true;
}

/* t is off a CPU: its kernel return addresses from its saved stack
 * pointer (switch_context's frame: r15 r14 r13 r12 rbx rbp, then the
 * return address), then up the frame-pointer chain, every read within
 * its own stack. */
static void kernel_frames(struct thread *t, struct thread_look *o)
{
    uint64_t hi = (uint64_t)t->stack_top, lo = hi - THREAD_STACK_SIZE;
    uint64_t sp = __atomic_load_n(&t->rsp, __ATOMIC_RELAXED), rbp, ret;
    if (!kword(lo, hi, sp + 5 * 8, &rbp) || !kword(lo, hi, sp + 6 * 8, &ret))
        return;
    o->kframes[o->nk++] = ret;
    while (o->nk < LOOK_KFRAMES && kword(lo, hi, rbp + 8, &ret) && ret) {
        o->kframes[o->nk++] = ret;
        uint64_t next;
        if (!kword(lo, hi, rbp, &next) || next <= rbp)
            break;   /* the entry's zero, or not a chain */
        rbp = next;
    }
}

void thread_look(struct thread *t, struct thread_look *o)
{
    memset(o, 0, sizeof(*o));
    size_t l = strlen(t->name);
    memcpy(o->name, t->name, l < sizeof(o->name) - 1 ? l : sizeof(o->name) - 1);
    o->state = thread_state(t);
    o->cpu = thread_cpu(t);
    o->on_cpu = __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE);
    o->wait_on = __atomic_load_n(&t->wait_on, __ATOMIC_ACQUIRE);
    o->wait_koid = __atomic_load_n(&t->wait_koid, __ATOMIC_RELAXED);
    o->wait_arg = __atomic_load_n(&t->wait_arg, __ATOMIC_RELAXED);
    o->deadline_ns = __atomic_load_n(&t->wake_at_ns, __ATOMIC_RELAXED);
    uint64_t off = __atomic_load_n(&t->off_tsc, __ATOMIC_RELAXED), now = rdtsc();
    o->off_ns = off && now > off ? tsc_to_ns(now - off) : 0;
    o->cpu_ns = tsc_to_ns(thread_cpu_tsc(t));
    o->mutex_since = __atomic_load_n(&t->mutex_since, __ATOMIC_RELAXED);
    o->cancelled = __atomic_load_n(&t->cancel_pending, __ATOMIC_RELAXED);
    o->interrupted = __atomic_load_n(&t->interrupt_pending, __ATOMIC_RELAXED);
    o->exc_window = __atomic_load_n(&t->exc_window, __ATOMIC_RELAXED);
    o->exc_rip = __atomic_load_n(&t->exc_rip, __ATOMIC_RELAXED);
    o->exc_addr = __atomic_load_n(&t->exc_addr, __ATOMIC_RELAXED);
    o->in_syscall = __atomic_load_n(&t->in_syscall, __ATOMIC_RELAXED);
    if (t->uthread && t->stack_top) {
        /* Its last entry from ring 3 left its frame at the top of its
         * stack: a system call's while it is inside one, else a trap's. */
        uint64_t top = (uint64_t)t->stack_top;
        if (o->in_syscall) {
            const volatile struct syscall_frame *f =
                (const volatile struct syscall_frame *)(top - sizeof(struct syscall_frame));
            o->sys_nr = f->nr;
            for (int i = 0; i < 3; i++)
                o->sys_args[i] = f->args[i];
            o->urip = f->user_rip;
            o->ursp = f->user_rsp;
            o->urbp = f->rbp;
        } else {
            const volatile struct trap_frame *f =
                (const volatile struct trap_frame *)(top - sizeof(struct trap_frame));
            o->urip = f->rip;
            o->ursp = f->rsp;
            o->urbp = f->rbp;
        }
        o->have_user = true;
    }
    if (o->state != T_RUNNING && !o->on_cpu && t->stack_top)
        kernel_frames(t, o);
}

/* ---- the user stack, with no lock held ---------------------------------------- */

static bool user_code(struct aspace *as, uint64_t a)
{
    return a >= USER_BASE && a < USER_TOP && aspace_peek_exec(as, a);
}

/* Does rec (above rsp) hold a frame record: a saved rbp, then a return
 * address into code? With `strict` (a record a scan finds, not one a
 * chain points at), the saved rbp must also point higher up the stack: a
 * stale word pair under a zero looks like the chain's start too often. */
static bool frame_record(struct aspace *as, uint64_t rsp, uint64_t rec, bool strict)
{
    uint64_t next, ret;
    if (rec < rsp || rec - rsp > FRAME_SPAN || (rec & 7))
        return false;
    if (!aspace_peek(as, rec, &next) || !aspace_peek(as, rec + 8, &ret) || !user_code(as, ret))
        return false;
    return !strict || (next > rec && next - rec <= FRAME_SPAN);
}

/* The first frame record in the LOOK_SCAN words from `from` up (0: none). */
static uint64_t record_above(struct aspace *as, uint64_t rsp, uint64_t from)
{
    for (uint32_t i = 0; i < LOOK_SCAN; i++)
        if (frame_record(as, rsp, from + 8 * i, true))
            return from + 8 * i;
    return 0;
}

uint32_t user_backtrace(struct aspace *as, uint64_t rip, uint64_t rsp, uint64_t rbp,
                        uint64_t *out, bool *scanned, uint32_t cap)
{
    uint32_t n = 0;
    if (cap > LOOK_UFRAMES)
        cap = LOOK_UFRAMES;
    rsp &= ~7ull;
    bool found = false;   /* the record now was found by a scan */
    if (n < cap) {
        out[n] = rip;
        if (scanned)
            scanned[n] = false;
        n++;
    }
    if (!frame_record(as, rsp, rbp, false)) {
        rbp = record_above(as, rsp, rsp);
        found = true;
    }
    while (rbp && n < cap) {
        uint64_t next, ret;
        if (!aspace_peek(as, rbp, &next) || !aspace_peek(as, rbp + 8, &ret) || !ret)
            break;
        out[n] = ret;
        if (scanned)
            scanned[n] = found;
        n++;
        if (!next)
            break;   /* the chain's start */
        found = !(next > rbp && frame_record(as, rsp, next, false));
        /* Code without frame pointers between this frame and the next
         * used rbp for something else: the next record above, if any.
         * Each step goes up, so the walk ends. */
        rbp = found ? record_above(as, rsp, rbp + 16) : next;
    }
    return n;
}

uint32_t user_stack_scan(struct aspace *as, uint64_t rsp, uint32_t words, uint32_t *off,
                         uint64_t *val, uint32_t cap)
{
    uint32_t n = 0;
    rsp &= ~7ull;
    for (uint32_t i = 0; i < words && n < cap; i++) {
        uint64_t w;
        if (!aspace_peek(as, rsp + 8 * i, &w))
            break;   /* the stack's top, or a page not there */
        if (user_code(as, w)) {
            off[n] = 8 * i;
            val[n++] = w;
        }
    }
    return n;
}

/* ---- printing ----------------------------------------------------------------- */

static const char *state_name(int s)
{
    switch (s) {
    case T_RUNNING:   return "running";
    case T_READY:     return "ready";
    case T_BLOCKED:   return "blocked";
    case T_MIGRATING: return "migrating";
    case T_DEAD:      return "dead";
    default:          return "?";
    }
}

/* The system calls a thread waits in, by name (the rest by number). */
static const char *sys_name(uint64_t nr)
{
    switch (nr) {
    case SYS_sleep_until:        return "sleep_until";
    case SYS_object_wait_one:    return "object_wait_one";
    case SYS_channel_read:       return "channel_read";
    case SYS_channel_call:       return "channel_call";
    case SYS_channel_reply_wait: return "channel_reply_wait";
    case SYS_port_wait:          return "port_wait";
    case SYS_futex_wait:         return "futex_wait";
    default:                     return NULL;
    }
}

/* "12.345 s" of ns, into buf. */
static const char *secs(uint64_t ns, char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%lu.%03lu s", (uint64_t)(ns / NS_PER_S),
              (uint64_t)(ns % NS_PER_S / NS_PER_MS));
    return buf;
}

static void print_wait(const struct thread_look *o)
{
    char what[96], dl[48], b[24];
    switch (o->wait_on) {
    case WAITON_FUTEX:
        ksnprintf(what, sizeof(what), "futex at %#lx", o->wait_arg);
        break;
    case WAITON_OBJECT:
        ksnprintf(what, sizeof(what), "object %lu for signals %#lx", o->wait_koid, o->wait_arg);
        break;
    case WAITON_READ:
        ksnprintf(what, sizeof(what), "a message on channel %lu", o->wait_koid);
        break;
    case WAITON_CALL:
        ksnprintf(what, sizeof(what), "the reply to a call on channel %lu", o->wait_koid);
        break;
    case WAITON_PORT:
        ksnprintf(what, sizeof(what), "a packet on port %lu", o->wait_koid);
        break;
    case WAITON_PAGER:
        ksnprintf(what, sizeof(what), "page %lu of pager VMO %lu", o->wait_arg, o->wait_koid);
        break;
    case WAITON_WRITEBACK:
        ksnprintf(what, sizeof(what), "a write-back of pager VMO %lu (page %lu on)",
                  o->wait_koid, o->wait_arg);
        break;
    case WAITON_SLEEP:
        ksnprintf(what, sizeof(what), "a sleep");
        break;
    default:
        if (o->mutex_since)
            ksnprintf(what, sizeof(what), "a kernel mutex (%s)",
                      secs(uptime_ns() - (o->mutex_since & ~1ull), b, sizeof(b)));
        else
            ksnprintf(what, sizeof(what), "nothing noted (its kernel frames say)");
        break;
    }
    uint64_t now = uptime_ns();
    if (o->deadline_ns == DEADLINE_NEVER)
        ksnprintf(dl, sizeof(dl), "no deadline");
    else if (o->deadline_ns >= now)
        ksnprintf(dl, sizeof(dl), "deadline in %s", secs(o->deadline_ns - now, b, sizeof(b)));
    else
        ksnprintf(dl, sizeof(dl), "deadline %s ago", secs(now - o->deadline_ns, b, sizeof(b)));
    kprintf("    waits for %s, %s%s%s\n", what, dl, o->cancelled ? " [cancelled]" : "",
            o->interrupted ? " [interrupt latched]" : "");
}

static void print_thread(struct aspace *as, const struct thread_look *o, uint32_t words)
{
    char t1[24], t2[24];
    if (o->state == T_RUNNING || o->on_cpu)
        kprintf("  thread %lu \"%s\" tid %u: running on CPU %u, CPU time %s\n", o->koid,
                o->name, o->tid, o->cpu, secs(o->cpu_ns, t1, sizeof(t1)));
    else
        kprintf("  thread %lu \"%s\" tid %u: %s for %s, CPU time %s\n", o->koid, o->name,
                o->tid, state_name(o->state), secs(o->off_ns, t1, sizeof(t1)),
                secs(o->cpu_ns, t2, sizeof(t2)));
    if (o->state == T_BLOCKED)
        print_wait(o);
    if (o->exc_window)
        kprintf("    in its exception entry's window (the fault at rip %#lx, address %#lx)\n",
                o->exc_rip, o->exc_addr);
    if (!o->have_user)
        return;
    if (o->in_syscall) {
        const char *sn = sys_name(o->sys_nr);
        kprintf("    in system call %lu%s%s%s (%#lx, %#lx, %#lx)\n", o->sys_nr, sn ? " (" : "",
                sn ? sn : "", sn ? ")" : "", o->sys_args[0], o->sys_args[1], o->sys_args[2]);
    }
    kprintf("    user rip %#lx rsp %#lx rbp %#lx (at its last kernel entry)\n", o->urip,
            o->ursp, o->urbp);
    for (uint32_t i = 0; i < o->nk; i++) {
        uint64_t offset;
        const char *name = ksym_lookup(o->kframes[i], &offset);
        if (name)
            kprintf("    kernel #%u %s+%#lx\n", i, name, offset);
        else
            kprintf("    kernel #%u %#lx\n", i, o->kframes[i]);
    }
    if (!as || o->state == T_RUNNING || o->on_cpu)
        return;   /* a running thread's stack moves under us: no walk */
    uint64_t frames[LOOK_UFRAMES];
    bool scanned[LOOK_UFRAMES];
    uint32_t n = user_backtrace(as, o->urip, o->ursp, o->urbp, frames, scanned, LOOK_UFRAMES);
    for (uint32_t i = 0; i < n; i++)
        kprintf("    user #%u %#018lx%s\n", i, frames[i],
                scanned[i] ? " (its record found by a scan)" : "");
    uint32_t off[LOOK_HITS];
    uint64_t val[LOOK_HITS];
    uint32_t m = user_stack_scan(as, o->ursp, words, off, val, LOOK_HITS);
    for (uint32_t i = 0; i < m; i++)
        kprintf("    stack +%#x %#018lx\n", off[i], val[i]);
}

/* A decimal number, all of s? */
static bool number(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > UINT64_MAX / 10 - 9)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

int64_t threads_report(struct job *scope, const char *which, uint32_t words)
{
    uint64_t koid;
    struct process *p = NULL;
    if (!words)
        words = LOOK_WORDS;
    if (words > LOOK_WORDS_MAX)
        words = LOOK_WORDS_MAX;
    if (scope && number(which, &koid))
        p = job_find_process_koid(scope, koid);
    if (scope && !p)
        p = job_find_process(scope, which, NULL);
    if (!p) {
        kprintf("threads: no process %s (ps's PID or a name)\n", which);
        return ERR_NOT_FOUND;
    }
    struct thread_look *looks = kmalloc(LOOK_THREADS * sizeof(*looks));
    if (!looks) {
        kobject_unref(process_kobject(p));
        return ERR_NO_MEMORY;
    }
    uint32_t n = process_look_threads(p, looks, LOOK_THREADS);
    struct aspace *as = process_aspace(p);
    kprintf("threads: process %lu \"%s\": %u thread%s\n", process_kobject(p)->koid,
            process_name(p), n, n == 1 ? "" : "s");
    for (uint32_t i = 0; i < n && i < LOOK_THREADS; i++)
        print_thread(as, &looks[i], words);
    if (n > LOOK_THREADS)
        kprintf("  (%u more not shown)\n", n - LOOK_THREADS);
    if (as)
        aspace_unref(as);
    kfree(looks);
    kobject_unref(process_kobject(p));
    return n;
}
