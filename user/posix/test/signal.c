/* posixtest's signal section (docs/M13-PLAN.md section 6): signals in one
 * process. The ones between processes are sigproc.c's.
 *
 *   - a handler runs on the thread the signal was routed to (the one
 *     thread not blocking it), and pthread_kill's on the thread named;
 *   - masks hold: a blocked signal stays pending (sigpending) and runs
 *     once when unblocked; sigprocmask's old mask; SA_RESETHAND;
 *     SIGKILL can't be caught; an ignored pending signal is dropped;
 *   - EINTR: a read and a poll blocked on a pipe ended by a handler
 *     without SA_RESTART; with SA_RESTART the read goes on and gets its
 *     bytes, the poll still ends with EINTR (signal(7));
 *   - no signal lost: two signals sent back to back to a thread blocked in
 *     a read (one wait, two interrupts) both run, 1,000 rounds; the same
 *     sent to the process;
 *   - sigsuspend, sigtimedwait (taken, and a timeout's EAGAIN), raise;
 *   - SIGPIPE: ignored, a write to a pipe with no reader is EPIPE; with a
 *     handler, the handler runs on the writing thread and EPIPE;
 *   - timers: setitimer every 10 ms (five SIGALRMs), alarm's time left, a
 *     POSIX timer with SIGEV_SIGNAL and a value, one with SIGEV_THREAD
 *     (its notifications all on one thread that knows its own id, though
 *     musl resets the thread's TLS after each);
 *   - pthread_cancel of a thread blocked in read;
 *   - a thread busy in a loop that makes no calls (docs/SIGNALS-PLAN.md
 *     section 2): pthread_kill(SIGPROF) runs its handler at once (most
 *     within 1 ms, each within 10 ms), on its alternate stack, with RIP
 *     inside the loop and RBP its frame (V8's profiler); 10,000 SIGPROF
 *     from a sampler thread, each once the last was handled, all handled;
 *     200 each to a thread making only quiet calls (mmap, munmap) and to
 *     one making plain calls too, all handled; tgkill(SIGURG) whose handler copies the stack from the ucontext's
 *     RSP and wakes the sender on a futex (Chromium's StackCopierSignal),
 *     on the interrupted stack; SIGINT from kill and from a timer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "pt.h"

#define ROUNDS 1000

/* Linux's futex operations (<linux/futex.h>, which musl does not ship). */
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* The busy threads' alternate stack: room for the kernel's frame, the
 * entry's copy of the vector state and a handler that makes calls. */
#define BUSY_ALT (64 << 10)

static volatile sig_atomic_t hits[65];       /* handler runs, by signal */
static volatile pthread_t    ran_on[65];     /* the thread the last one ran on */
static volatile int          last_code[65];  /* its si_code */
static volatile long         last_value[65]; /* its si_value */

static void count(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    hits[sig]++;
    ran_on[sig] = pthread_self();
    last_code[sig] = si->si_code;
    last_value[sig] = (long)si->si_value.sival_ptr;
}

/* Count sig with count(), with these flags. */
static void handle(int sig, int flags)
{
    struct sigaction sa = { .sa_sigaction = count, .sa_flags = SA_SIGINFO | flags };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(sig, &sa, NULL) == 0);
    hits[sig] = 0;
}

static void deflt(int sig)
{
    signal(sig, SIG_DFL);
}

static void block(int sig, int how)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    CHECK(pthread_sigmask(how, &s, NULL) == 0);
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

/* Wait (up to 10 s) until *v reaches n. */
static bool reaches(volatile sig_atomic_t *v, int n)
{
    for (int i = 0; i < 10000 && *v < n; i++)
        nap_ms(1);
    return *v >= n;
}

/* ---- threads and masks -------------------------------------------------------------- */

static volatile sig_atomic_t taker_go;

/* A thread that takes SIGUSR1 (all others block it) until told to stop. */
static void *taker(void *arg)
{
    (void)arg;
    block(SIGUSR1, SIG_UNBLOCK);
    while (!taker_go)
        pause();
    return NULL;
}

static void check_routing(void)
{
    handle(SIGUSR1, 0);
    handle(SIGUSR2, 0);
    block(SIGUSR1, SIG_BLOCK);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, taker, NULL) == 0);
    nap_ms(20);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(reaches(&hits[SIGUSR1], 1));
    CHECK(pthread_equal(ran_on[SIGUSR1], t));
    CHECK(last_code[SIGUSR1] == SI_USER);
    CHECK(pthread_kill(t, SIGUSR2) == 0);
    CHECK(reaches(&hits[SIGUSR2], 1));
    CHECK(pthread_equal(ran_on[SIGUSR2], t));
    CHECK(last_code[SIGUSR2] == SI_TKILL);
    taker_go = 1;
    CHECK(pthread_kill(t, SIGUSR1) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(pthread_kill(pthread_self(), 0) == 0);
    block(SIGUSR1, SIG_UNBLOCK);
}

static void check_masks(void)
{
    handle(SIGUSR1, 0);
    block(SIGUSR1, SIG_BLOCK);
    CHECK(raise(SIGUSR1) == 0);
    CHECK(raise(SIGUSR1) == 0);   /* a standard signal pending twice is one */
    CHECK(hits[SIGUSR1] == 0);
    sigset_t p, old;
    CHECK(sigpending(&p) == 0 && sigismember(&p, SIGUSR1));
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &old) == 0 && sigismember(&old, SIGUSR1));
    block(SIGUSR1, SIG_UNBLOCK);
    CHECK(hits[SIGUSR1] == 1);   /* at once, on the way out of the call that unblocked it */
    CHECK(sigpending(&p) == 0 && !sigismember(&p, SIGUSR1));
    /* An ignored pending signal is dropped. */
    block(SIGUSR2, SIG_BLOCK);
    handle(SIGUSR2, 0);
    CHECK(raise(SIGUSR2) == 0);
    signal(SIGUSR2, SIG_IGN);
    CHECK(sigpending(&p) == 0 && !sigismember(&p, SIGUSR2));
    block(SIGUSR2, SIG_UNBLOCK);
    CHECK(hits[SIGUSR2] == 0);
    /* SA_RESETHAND: the second goes to the default (ignored here: SIGURG). */
    handle(SIGURG, SA_RESETHAND);
    CHECK(raise(SIGURG) == 0 && raise(SIGURG) == 0);
    CHECK(hits[SIGURG] == 1);
    struct sigaction sa = { .sa_handler = SIG_IGN };
    errno = 0;
    CHECK(sigaction(SIGKILL, &sa, NULL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(kill(1, 0) == -1 && errno == ESRCH);   /* not ours to reach */
    CHECK(kill(getpid(), 0) == 0);
    deflt(SIGUSR1);
    deflt(SIGUSR2);
}

/* ---- EINTR and restart ------------------------------------------------------------- */

struct reader {
    int        fd;
    bool       use_poll;
    ssize_t    r;
    int        err;
    pthread_t  t;
};

static void *read_it(void *arg)
{
    struct reader *rd = arg;
    char b[8];
    errno = 0;
    if (rd->use_poll) {
        struct pollfd p = { .fd = rd->fd, .events = POLLIN };
        rd->r = poll(&p, 1, 10000);
    } else {
        rd->r = read(rd->fd, b, sizeof(b));
    }
    rd->err = errno;
    return NULL;
}

/* A thread blocked reading (or polling) p[0], sent SIGUSR1 (handled with
 * flags); then, if nothing came of it, a byte written. What it got. */
static struct reader interrupted(bool use_poll, int flags)
{
    int p[2];
    struct reader rd = { .use_poll = use_poll, .r = -2 };
    CHECK(pipe(p) == 0);
    rd.fd = p[0];
    handle(SIGUSR1, flags);
    CHECK(pthread_create(&rd.t, NULL, read_it, &rd) == 0);
    nap_ms(30);   /* it is asleep in the call */
    CHECK(pthread_kill(rd.t, SIGUSR1) == 0);
    CHECK(reaches(&hits[SIGUSR1], 1));
    nap_ms(30);
    CHECK(write(p[1], "x", 1) == 1);   /* a restarted read takes it */
    CHECK(pthread_join(rd.t, NULL) == 0);
    close(p[0]);
    close(p[1]);
    deflt(SIGUSR1);
    return rd;
}

static void check_eintr(void)
{
    struct reader rd = interrupted(false, 0);
    CHECK(rd.r == -1 && rd.err == EINTR);
    rd = interrupted(false, SA_RESTART);
    CHECK(rd.r == 1);
    rd = interrupted(true, 0);
    CHECK(rd.r == -1 && rd.err == EINTR);
    rd = interrupted(true, SA_RESTART);   /* poll is never restarted */
    CHECK(rd.r == -1 && rd.err == EINTR);
}

static struct timespec sleep_left;
static int             sleep_err;

static void *sleep_long(void *arg)
{
    (void)arg;
    struct timespec t = { .tv_sec = (time_t)1 << 62 };   /* longer than any uptime */
    errno = 0;
    sleep_err = nanosleep(&t, &sleep_left) ? errno : 0;
    return NULL;
}

/* A nanosleep far longer than the clock can count, ended by a signal:
 * EINTR, and the time left is what was asked less what was slept, never
 * negative (the deadline it slept to saturates). */
static void check_sleep_left(void)
{
    pthread_t t;
    handle(SIGUSR1, 0);
    CHECK(pthread_create(&t, NULL, sleep_long, NULL) == 0);
    nap_ms(30);
    CHECK(pthread_kill(t, SIGUSR1) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    deflt(SIGUSR1);
    CHECK(sleep_err == EINTR);
    CHECK(sleep_left.tv_sec > ((time_t)1 << 30) && sleep_left.tv_nsec >= 0 &&
          sleep_left.tv_nsec < 1000000000L);
}

/* Two signals at a thread blocked in one wait: both handlers run. */
static void check_two_in_one_wait(void)
{
    int p[2];
    struct reader rd = { .r = -2 };
    CHECK(pipe(p) == 0);
    rd.fd = p[0];
    handle(SIGUSR1, SA_RESTART);
    handle(SIGUSR2, SA_RESTART);
    CHECK(pthread_create(&rd.t, NULL, read_it, &rd) == 0);   /* it doesn't block them */
    block(SIGUSR1, SIG_BLOCK);
    block(SIGUSR2, SIG_BLOCK);   /* we do, so the process's go to the reader */
    nap_ms(20);
    bool lost = false;
    for (int i = 1; i <= ROUNDS && !lost; i++) {
        if (i % 2) {
            CHECK(pthread_kill(rd.t, SIGUSR1) == 0 && pthread_kill(rd.t, SIGUSR2) == 0);
        } else {
            CHECK(kill(getpid(), SIGUSR1) == 0 && kill(getpid(), SIGUSR2) == 0);
        }
        lost = !reaches(&hits[SIGUSR1], i) || !reaches(&hits[SIGUSR2], i);
    }
    CHECK(!lost && hits[SIGUSR1] == ROUNDS && hits[SIGUSR2] == ROUNDS);
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(pthread_join(rd.t, NULL) == 0 && rd.r == 1);
    close(p[0]);
    close(p[1]);
    block(SIGUSR1, SIG_UNBLOCK);
    block(SIGUSR2, SIG_UNBLOCK);
    deflt(SIGUSR1);
    deflt(SIGUSR2);
}

/* ---- waiting for signals ------------------------------------------------------------ */

static void check_waits(void)
{
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &s, &old) == 0);
    union sigval v = { .sival_int = 42 };
    CHECK(sigqueue(getpid(), SIGUSR1, v) == 0);
    siginfo_t si;
    CHECK(sigwaitinfo(&s, &si) == SIGUSR1 && si.si_code == SI_QUEUE && si.si_value.sival_int == 42);
    struct timespec ts = { 0, 100000000 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    CHECK(sigtimedwait(&s, &si, &ts) == -1 && errno == EAGAIN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 90);
    int sig = 0;
    CHECK(raise(SIGUSR1) == 0 && sigwait(&s, &sig) == 0 && sig == SIGUSR1);
    /* sigsuspend: unblocked for the wait, its handler run, the mask back. */
    handle(SIGUSR1, 0);
    CHECK(raise(SIGUSR1) == 0 && hits[SIGUSR1] == 0);
    errno = 0;
    CHECK(sigsuspend(&old) == -1 && errno == EINTR && hits[SIGUSR1] == 1);
    sigset_t now;
    CHECK(pthread_sigmask(SIG_SETMASK, &old, &now) == 0 && sigismember(&now, SIGUSR1));
    deflt(SIGUSR1);
}

/* ---- SIGPIPE ------------------------------------------------------------------------ */

static void check_sigpipe(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    close(p[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    CHECK(write(p[1], "x", 1) == -1 && errno == EPIPE);
    handle(SIGPIPE, 0);
    errno = 0;
    CHECK(write(p[1], "x", 1) == -1 && errno == EPIPE);
    CHECK(hits[SIGPIPE] == 1 && pthread_equal(ran_on[SIGPIPE], pthread_self()));
    close(p[1]);
    deflt(SIGPIPE);
}

/* ---- timers ------------------------------------------------------------------------- */

static volatile sig_atomic_t thread_notes;
static volatile pid_t        note_tid[3];   /* gettid in the first notifications */

static void note(union sigval v)
{
    if (v.sival_int != 7)
        return;
    if (thread_notes < 3)
        note_tid[thread_notes] = gettid();
    thread_notes++;
}

static void check_timers(void)
{
    handle(SIGALRM, SA_RESTART);
    struct itimerval it = { { 0, 10000 }, { 0, 10000 } }, left;
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    CHECK(reaches(&hits[SIGALRM], 5));
    struct itimerval off = { 0 };
    CHECK(setitimer(ITIMER_REAL, &off, &left) == 0 && left.it_interval.tv_usec == 10000);
    CHECK(alarm(100) == 0 && alarm(0) >= 99);
    deflt(SIGALRM);

    handle(SIGRTMIN + 1, 0);
    timer_t t;
    struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN + 1,
                           .sigev_value.sival_int = 99 };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    struct itimerspec at = { { 0, 0 }, { 0, 20000000 } }, cur;
    CHECK(timer_settime(t, 0, &at, NULL) == 0);
    CHECK(timer_gettime(t, &cur) == 0 && cur.it_value.tv_nsec > 0);
    CHECK(reaches(&hits[SIGRTMIN + 1], 1));
    CHECK(last_code[SIGRTMIN + 1] == SI_TIMER && last_value[SIGRTMIN + 1] == 99);
    CHECK(timer_getoverrun(t) == 0 && timer_delete(t) == 0);
    deflt(SIGRTMIN + 1);

    ev = (struct sigevent){ .sigev_notify = SIGEV_THREAD, .sigev_notify_function = note,
                            .sigev_value.sival_int = 7 };
    CHECK(timer_create(CLOCK_REALTIME, &ev, &t) == 0);
    at = (struct itimerspec){ { 0, 10000000 }, { 0, 10000000 } };
    CHECK(timer_settime(t, 0, &at, NULL) == 0);
    CHECK(reaches(&thread_notes, 3));
    CHECK(timer_delete(t) == 0);
    CHECK(note_tid[0] > 0 && note_tid[0] != gettid());
    CHECK(note_tid[1] == note_tid[0] && note_tid[2] == note_tid[0]);
}

/* ---- cancellation ------------------------------------------------------------------- */

static void *read_forever(void *arg)
{
    char b;
    (void)read(*(int *)arg, &b, 1);
    return (void *)1;   /* never: cancelled in the read */
}

static void check_cancel(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, read_forever, &p[0]) == 0);
    nap_ms(30);
    CHECK(pthread_cancel(t) == 0);
    void *res = NULL;
    CHECK(pthread_join(t, &res) == 0 && res == PTHREAD_CANCELED);
    close(p[0]);
    close(p[1]);
}

/* ---- a thread busy in its own code (SG5) -------------------------------------------- */

/* The loop V8's profiler samples: no calls at all, a frame of its own
 * (RBP, written to *frame once it runs), and its end named so a handler
 * can tell its RIP is inside. Spins until *stop. */
void pt_sig_spin(volatile int *stop, void **frame);
extern const char pt_sig_spin_end[];
__asm__(".text\n"
        ".globl pt_sig_spin, pt_sig_spin_end\n"
        ".hidden pt_sig_spin, pt_sig_spin_end\n"
        ".type pt_sig_spin, @function\n"
        ".p2align 4\n"
        "pt_sig_spin:\n"
        "    push %rbp\n"
        "    mov %rsp, %rbp\n"
        "    mov %rbp, (%rsi)\n"
        "1:  pause\n"
        "    cmpl $0, (%rdi)\n"
        "    je 1b\n"
        "    pop %rbp\n"
        "    ret\n"
        "pt_sig_spin_end:\n"
        ".size pt_sig_spin, . - pt_sig_spin\n");

/* A busy thread taking SIGPROF, SIGURG and SIGINT, with an alternate stack
 * if alt is set: in pt_sig_spin, or making calls (busy_calling). */
struct busy {
    pthread_t      thread;
    volatile pid_t tid;
    volatile int   stop;
    void *volatile frame;   /* the loop's RBP, once it runs (busy_calling: itself) */
    stack_t        alt;
    bool           plain;   /* busy_calling: plain calls too */
};

static struct busy *target;   /* the one the handlers below look at */

static void busy_begin(struct busy *b)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGPROF);
    sigaddset(&s, SIGURG);
    sigaddset(&s, SIGINT);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &s, NULL) == 0);
    if (b->alt.ss_sp)
        CHECK(sigaltstack(&b->alt, NULL) == 0);
    b->tid = gettid();
}

static void *busy_run(void *arg)
{
    struct busy *b = arg;
    busy_begin(b);
    pt_sig_spin(&b->stop, (void **)&b->frame);
    return NULL;
}

/* Quiet calls (mmap and munmap: no handler runs inside them, and a raise
 * that finds the thread there raises it again once it is out), plain
 * ones too if b->plain (the boundary delivers), and a spin between. */
static void *busy_calling(void *arg)
{
    struct busy *b = arg;
    busy_begin(b);
    b->frame = b;
    while (!b->stop) {
        if (b->plain)
            (void)getppid();
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            munmap(p, 4096);
        for (volatile int i = 0; i < 20000; i++)
            ;
    }
    return NULL;
}

static void busy_start_with(struct busy *b, size_t alt, void *(*fn)(void *), bool plain)
{
    *b = (struct busy){ .alt = { .ss_sp = alt ? malloc(alt) : NULL, .ss_size = alt },
                        .plain = plain };
    target = b;
    CHECK(pthread_create(&b->thread, NULL, fn, b) == 0);
    for (int i = 0; i < 10000 && !b->frame; i++)
        nap_ms(1);
    CHECK(b->frame);
}

static void busy_start(struct busy *b, size_t alt)
{
    busy_start_with(b, alt, busy_run, false);
}

static void busy_stop(struct busy *b)
{
    b->stop = 1;
    CHECK(pthread_join(b->thread, NULL) == 0);
    free(b->alt.ss_sp);
}

/* The registers a handler was given are the loop's: RIP inside it, RBP
 * and RSP its frame. */
static bool in_loop(const ucontext_t *uc)
{
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t frame = (uintptr_t)target->frame;
    return rip >= (uintptr_t)pt_sig_spin && rip < (uintptr_t)pt_sig_spin_end &&
           (uintptr_t)uc->uc_mcontext.gregs[REG_RBP] == frame &&
           (uintptr_t)uc->uc_mcontext.gregs[REG_RSP] == frame;
}

/* addr is on the target's alternate stack. */
static bool on_target_alt(uintptr_t addr)
{
    uintptr_t base = (uintptr_t)target->alt.ss_sp;
    return base && addr >= base && addr - base < target->alt.ss_size;
}

static long us_since(const struct timespec *t0, const struct timespec *t)
{
    return (t->tv_sec - t0->tv_sec) * 1000000L + (t->tv_nsec - t0->tv_nsec) / 1000;
}

/* Wait (up to ms, yielding) until *v reaches n. */
static bool reaches_soon(volatile sig_atomic_t *v, int n, long ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (*v < n) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (us_since(&t0, &t) > ms * 1000)
            return false;
        sched_yield();
    }
    return true;
}

static volatile sig_atomic_t busy_hits[65];    /* sample()'s runs, by signal */
static volatile sig_atomic_t busy_inloop[65];  /* of them, with the loop's registers */
static volatile sig_atomic_t busy_onalt[65];   /* of them, on the target's alternate stack */
static struct timespec       busy_at;          /* when the last one ran */

/* V8's profiler's handler, in short: where was the thread? */
static void sample(int sig, siginfo_t *si, void *uc)
{
    (void)si;
    if (in_loop(uc))
        busy_inloop[sig]++;
    if (on_target_alt((uintptr_t)__builtin_frame_address(0)))
        busy_onalt[sig]++;
    ran_on[sig] = pthread_self();
    clock_gettime(CLOCK_MONOTONIC, &busy_at);
    busy_hits[sig]++;
}

static void handle_busy(int sig, void (*h)(int, siginfo_t *, void *), int flags)
{
    struct sigaction sa = { .sa_sigaction = h, .sa_flags = SA_SIGINFO | flags };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(sig, &sa, NULL) == 0);
    busy_hits[sig] = busy_inloop[sig] = busy_onalt[sig] = 0;
}

#define PROF_ROUNDS  20
#define PROF_SAMPLES 10000

/* 10,000 SIGPROF, each sent once the last was handled (V8's sampler
 * thread). */
static void *sampler(void *arg)
{
    (void)arg;
    for (int i = 0; i < PROF_SAMPLES; i++) {
        int n = busy_hits[SIGPROF];
        if (pthread_kill(target->thread, SIGPROF) || !reaches_soon(&busy_hits[SIGPROF], n + 1, 2000))
            break;
    }
    return NULL;
}

/* V8's CPU profiler: pthread_kill(SIGPROF) to a thread running a loop
 * that makes no calls runs its handler at once, on its alternate stack
 * (SA_ONSTACK, V8's flags), with the registers where it was. Most within
 * a millisecond (the kernel interrupts the thread's CPU for it), each
 * within 10 ms (a tick); then 10,000 in a row from a sampler thread. */
static void check_busy_profiler(void)
{
    handle_busy(SIGPROF, sample, SA_RESTART | SA_ONSTACK);
    struct busy b;
    busy_start(&b, BUSY_ALT);
    long us[PROF_ROUNDS];
    int fast = 0, slow = 0;
    for (int i = 0; i < PROF_ROUNDS; i++) {
        nap_ms(1);   /* back in its loop: a raise inside a call waits for a tick */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(pthread_kill(b.thread, SIGPROF) == 0);
        CHECK(reaches_soon(&busy_hits[SIGPROF], i + 1, 2000));
        us[i] = us_since(&t0, &busy_at);
        fast += us[i] < 1000;
        slow += us[i] >= 10000;
    }
    CHECK(busy_inloop[SIGPROF] == PROF_ROUNDS && busy_onalt[SIGPROF] == PROF_ROUNDS);
    CHECK(pthread_equal(ran_on[SIGPROF], b.thread));
    CHECK(fast > PROF_ROUNDS / 2 && slow == 0);
    long worst = 0;
    for (int i = 0; i < PROF_ROUNDS; i++)
        worst = us[i] > worst ? us[i] : worst;
    printf("posixtest: signal: SIGPROF to a busy thread: %d of %d within 1 ms, the slowest %ld us\n",
           fast, PROF_ROUNDS, worst);

    handle_busy(SIGPROF, sample, SA_RESTART | SA_ONSTACK);
    pthread_t s;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(pthread_create(&s, NULL, sampler, NULL) == 0);
    CHECK(pthread_join(s, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(busy_hits[SIGPROF] == PROF_SAMPLES && busy_inloop[SIGPROF] == PROF_SAMPLES);
    printf("posixtest: signal: %d SIGPROF from a sampler thread, all handled, %ld us each\n",
           busy_hits[SIGPROF], us_since(&t0, &t1) / PROF_SAMPLES);
    busy_stop(&b);
    deflt(SIGPROF);
}

#define CALL_SAMPLES 200

/* A thread that makes calls gets every signal too: in quiet calls only (a
 * raise inside one is raised again once out, or the signal would wait for
 * a call that never comes), and with plain calls among them. */
static void check_busy_calls(void)
{
    for (int plain = 0; plain < 2; plain++) {
        handle_busy(SIGPROF, sample, SA_RESTART);
        struct busy b;
        busy_start_with(&b, 0, busy_calling, plain);
        int i = 0;
        while (i < CALL_SAMPLES && !pthread_kill(b.thread, SIGPROF) &&
               reaches_soon(&busy_hits[SIGPROF], i + 1, 5000))
            i++;
        CHECK(i == CALL_SAMPLES);
        busy_stop(&b);
    }
    deflt(SIGPROF);
}

/* Chromium's StackCopierSignal: tgkill(SIGURG), the handler copies the
 * thread's stack from the RSP its ucontext gives up to the stack's top,
 * then wakes the sender on a futex. No SA_ONSTACK, so the handler runs on
 * the interrupted stack (below that RSP) though the thread has an
 * alternate one. */
static struct {
    volatile uint32_t done;   /* the futex word: 1 once the copy is made */
    uintptr_t top;            /* the stack's top */
    uint8_t  *buf;
    size_t    cap, len;
    uintptr_t rsp, here;      /* the ucontext's RSP, the handler's frame */
    bool      inloop;
} copy;

static void copy_stack(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ucv;
    copy.inloop = in_loop(uc);
    copy.rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    copy.here = (uintptr_t)__builtin_frame_address(0);
    copy.len = 0;
    if (copy.rsp < copy.top && copy.top - copy.rsp <= copy.cap) {
        copy.len = copy.top - copy.rsp;
        memcpy(copy.buf, (void *)copy.rsp, copy.len);
    }
    __atomic_store_n(&copy.done, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &copy.done, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

#define COPY_ROUNDS 200

static void check_busy_stack_copy(void)
{
    handle_busy(SIGURG, copy_stack, SA_RESTART);
    struct busy b;
    busy_start(&b, BUSY_ALT);
    pthread_attr_t attr;
    void *base;
    size_t size;
    CHECK(pthread_getattr_np(b.thread, &attr) == 0);
    CHECK(pthread_attr_getstack(&attr, &base, &size) == 0);
    pthread_attr_destroy(&attr);
    copy.top = (uintptr_t)base + size;
    copy.cap = size;
    copy.buf = malloc(size);
    uintptr_t frame = (uintptr_t)b.frame;
    int good = 0;
    for (int i = 0; i < COPY_ROUNDS; i++) {
        __atomic_store_n(&copy.done, 0, __ATOMIC_RELAXED);
        CHECK(syscall(SYS_tgkill, getpid(), b.tid, SIGURG) == 0);
        struct timespec second = { 1, 0 };
        for (int w = 0; w < 5 && !__atomic_load_n(&copy.done, __ATOMIC_ACQUIRE); w++)
            syscall(SYS_futex, &copy.done, FUTEX_WAIT_PRIVATE, 0, &second, NULL, 0);
        if (!__atomic_load_n(&copy.done, __ATOMIC_ACQUIRE))
            break;
        /* the loop's frame in the copy as it is on the stack: its saved
         * RBP and return address; the handler below the RSP, not on the
         * alternate stack */
        size_t at = frame - copy.rsp;
        good += copy.inloop && copy.len && at + 16 <= copy.len &&
                !memcmp(copy.buf + at, (void *)frame, 16) && copy.here < copy.rsp &&
                !on_target_alt(copy.here);
    }
    CHECK(good == COPY_ROUNDS);
    busy_stop(&b);
    free(copy.buf);
    deflt(SIGURG);
}

/* A SIGINT handler runs on a thread in a busy loop, the one thread that
 * takes it: sent by kill (routed by the sender) and by a timer (routed by
 * the signal thread). */
static void check_busy_sigint(void)
{
    block(SIGINT, SIG_BLOCK);
    handle_busy(SIGINT, sample, 0);
    struct busy b;
    busy_start(&b, 0);
    CHECK(kill(getpid(), SIGINT) == 0);
    CHECK(reaches_soon(&busy_hits[SIGINT], 1, 2000));
    timer_t t;
    struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGINT };
    CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
    struct itimerspec at = { { 0, 0 }, { 0, 10000000 } };
    CHECK(timer_settime(t, 0, &at, NULL) == 0);
    CHECK(reaches_soon(&busy_hits[SIGINT], 2, 2000));
    CHECK(timer_delete(t) == 0);
    CHECK(busy_inloop[SIGINT] == 2 && pthread_equal(ran_on[SIGINT], b.thread));
    busy_stop(&b);
    deflt(SIGINT);
    block(SIGINT, SIG_UNBLOCK);
}

void pt_signal(char **argv)
{
    (void)argv;
    check_routing();
    check_masks();
    check_eintr();
    check_sleep_left();
    check_two_in_one_wait();
    check_waits();
    check_sigpipe();
    check_timers();
    check_cancel();
    check_busy_profiler();
    check_busy_calls();
    check_busy_stack_copy();
    check_busy_sigint();
    pt_sigproc();
}
