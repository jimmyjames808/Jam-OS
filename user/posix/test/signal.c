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
 *     POSIX timer with SIGEV_SIGNAL and a value, one with SIGEV_THREAD;
 *   - pthread_cancel of a thread blocked in read. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define ROUNDS 1000

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

static void note(union sigval v)
{
    if (v.sival_int == 7)
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
    pt_sigproc();
}
