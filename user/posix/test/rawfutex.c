/* posixtest's raw section, futexes by raw syscall(), the way PartitionAlloc
 * and Mojo call them (musl's own locks use the plain wait and wake):
 *
 *   - FUTEX_WAIT_PRIVATE: a changed word is EAGAIN, a timeout (relative)
 *     is ETIMEDOUT after about that long, FUTEX_WAKE_PRIVATE with nobody
 *     waiting wakes 0;
 *   - FUTEX_WAIT_BITSET_PRIVATE with an absolute timeout on the monotonic
 *     clock, and with FUTEX_CLOCK_REALTIME on the wall clock: ETIMEDOUT
 *     after about that long; a time already past is ETIMEDOUT at once;
 *   - the masks: 0 is EINVAL, every bit is taken, any other wait mask is
 *     ENOSYS (and named once in the ENOSYS log); the clock flag on any
 *     other operation is ENOSYS; a bad timespec is EINVAL;
 *   - an operation Jam OS does not have (CMP_REQUEUE) is ENOSYS, logged
 *     once;
 *   - a wake reaches a bitset waiter and a plain waiter, by either wake
 *     call, and returns how many it woke. */
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "../sys/calls.h"
#include "pt.h"

/* Linux's futex operations (<linux/futex.h>, which musl does not ship). */
#define F_WAIT         0
#define F_WAKE         1
#define F_CMP_REQUEUE  4
#define F_WAIT_BITSET  9
#define F_WAKE_BITSET  10
#define F_PRIVATE      128
#define F_CLOCK_RT     256
#define ANY_BIT        0xffffffffu
#define TIMEOUT_MS     20
#define SLOW_MS        2000   /* far more than any wait here should take */

static long futex(uint32_t *a, int op, uint32_t val, const struct timespec *ts, uint32_t *a2,
                  uint32_t val3)
{
    errno = 0;
    return syscall(SYS_futex, a, op, val, ts, a2, val3);
}

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* now + ms on clock clk, as a timespec. */
static struct timespec at(clockid_t clk, long ms)
{
    struct timespec t;
    clock_gettime(clk, &t);
    t.tv_nsec += (ms % 1000) * 1000000L;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    return t;
}

/* A wait that times out: -1 ETIMEDOUT after at least ~TIMEOUT_MS and not
 * long. */
static void expect_timeout(uint32_t *w, int op, const struct timespec *ts)
{
    long t0 = mono_ms();
    CHECK(futex(w, op, *w, ts, NULL, ANY_BIT) == -1 && errno == ETIMEDOUT);
    long took = mono_ms() - t0;
    CHECK(took >= TIMEOUT_MS - 5 && took < SLOW_MS);
}

static void check_plain(void)
{
    uint32_t w = 1;
    CHECK(futex(&w, F_WAIT | F_PRIVATE, 0, NULL, NULL, 0) == -1 && errno == EAGAIN);   /* w != 0 */
    struct timespec rel = { 0, TIMEOUT_MS * 1000000L };
    long t0 = mono_ms();
    CHECK(futex(&w, F_WAIT | F_PRIVATE, 1, &rel, NULL, 0) == -1 && errno == ETIMEDOUT);
    long took = mono_ms() - t0;
    CHECK(took >= TIMEOUT_MS - 5 && took < SLOW_MS);
    CHECK(futex(&w, F_WAKE | F_PRIVATE, 1, NULL, NULL, 0) == 0);   /* nobody waits */
    CHECK(futex(&w, F_WAKE, 100, NULL, NULL, 0) == 0);
    struct timespec bad = { 0, 2000000000L };
    CHECK(futex(&w, F_WAIT | F_PRIVATE, 1, &bad, NULL, 0) == -1 && errno == EINVAL);
}

static void check_bitset_waits(void)
{
    uint32_t w = 1;
    struct timespec mono = at(CLOCK_MONOTONIC, TIMEOUT_MS);
    expect_timeout(&w, F_WAIT_BITSET | F_PRIVATE, &mono);                 /* absolute, monotonic */
    struct timespec real = at(CLOCK_REALTIME, TIMEOUT_MS);   /* made now: the first wait used its 20 ms */
    expect_timeout(&w, F_WAIT_BITSET | F_PRIVATE | F_CLOCK_RT, &real);    /* absolute, wall clock */

    /* A time already past is not a wait. */
    struct timespec past = at(CLOCK_REALTIME, 0);
    past.tv_sec -= 3600;
    long t0 = mono_ms();
    CHECK(futex(&w, F_WAIT_BITSET | F_PRIVATE | F_CLOCK_RT, 1, &past, NULL, ANY_BIT) == -1);
    CHECK(errno == ETIMEDOUT && mono_ms() - t0 < SLOW_MS);

    struct timespec later = at(CLOCK_MONOTONIC, SLOW_MS);
    CHECK(futex(&w, F_WAIT_BITSET | F_PRIVATE, 0, &later, NULL, ANY_BIT) == -1 && errno == EAGAIN);
    struct timespec bad = { 0, 2000000000L };
    CHECK(futex(&w, F_WAIT_BITSET | F_PRIVATE, 1, &bad, NULL, ANY_BIT) == -1 && errno == EINVAL);
}

static void check_masks(void)
{
    uint32_t w = 1;
    struct timespec t = at(CLOCK_MONOTONIC, TIMEOUT_MS);
    CHECK(futex(&w, F_WAIT_BITSET | F_PRIVATE, 1, &t, NULL, 0) == -1 && errno == EINVAL);
    CHECK(futex(&w, F_WAKE_BITSET | F_PRIVATE, 1, NULL, NULL, 0) == -1 && errno == EINVAL);
    CHECK(futex(&w, F_WAIT_BITSET | F_PRIVATE, 1, &t, NULL, 0x1) == -1 && errno == ENOSYS);
    CHECK(futex(&w, F_WAKE_BITSET | F_PRIVATE, 1, NULL, NULL, 0x1) == 0);   /* some bit: a wake */
    /* The clock flag belongs to the bitset wait. */
    CHECK(futex(&w, F_WAIT | F_CLOCK_RT, 1, NULL, NULL, 0) == -1 && errno == ENOSYS);
    CHECK(futex(&w, F_WAKE | F_CLOCK_RT, 1, NULL, NULL, 0) == -1 && errno == ENOSYS);
    CHECK(futex(&w, F_WAKE_BITSET | F_CLOCK_RT, 1, NULL, NULL, ANY_BIT) == -1 && errno == ENOSYS);

    /* An operation we do not have, named once in the log. */
    lx_enosys_set(1);
    unsigned n0 = lx_enosys_lines();
    CHECK(futex(&w, F_CMP_REQUEUE | F_PRIVATE, 1, NULL, &w, 1) == -1 && errno == ENOSYS);
    CHECK(futex(&w, F_CMP_REQUEUE | F_PRIVATE, 1, NULL, &w, 1) == -1 && errno == ENOSYS);
    CHECK(lx_enosys_lines() == n0 + 1);
    char last[256];
    lx_enosys_last(last, sizeof(last));
    CHECK(strstr(last, ": futex operation 4\n"));
    futex(&w, F_WAIT_BITSET | F_PRIVATE, 1, &t, NULL, 0x1);   /* the mask refused: logged too */
    CHECK(lx_enosys_lines() == n0 + 2);
    lx_enosys_last(last, sizeof(last));
    CHECK(strstr(last, ": futex wait bitset 1\n"));
    futex(&w, F_WAIT | F_PRIVATE, 1, &(struct timespec){ 0, 1000000 }, NULL, 0);
    CHECK(lx_enosys_lines() == n0 + 2);   /* the answered ones are no gap */
    lx_enosys_set(0);
}

/* ---- a wake reaches both kinds of waiter -------------------------------------------- */

struct waiter {
    uint32_t     *word;
    int           op;        /* F_WAIT_BITSET or F_WAIT, with F_PRIVATE */
    volatile int  up;        /* about to wait */
    volatile long result;    /* the call's, -errno */
};

static void *waiter_main(void *arg)
{
    struct waiter *w = arg;
    __atomic_store_n(&w->up, 1, __ATOMIC_RELEASE);
    long r = futex(w->word, w->op, 0, NULL, NULL, ANY_BIT);
    w->result = r == -1 ? -errno : r;
    return NULL;
}

static void nap_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void check_wake(int wake_op)
{
    uint32_t word = 0;
    struct waiter ws[2] = {
        { &word, F_WAIT_BITSET | F_PRIVATE, 0, 99 },
        { &word, F_WAIT | F_PRIVATE, 0, 99 },
    };
    pthread_t t[2];
    for (int i = 0; i < 2; i++) {
        CHECK(pthread_create(&t[i], NULL, waiter_main, &ws[i]) == 0);
        while (!__atomic_load_n(&ws[i].up, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    nap_ms(50);   /* both in the wait, as far as a test can tell */
    __atomic_store_n(&word, 1, __ATOMIC_RELEASE);
    long woke = 0;
    for (long start = mono_ms(); mono_ms() - start < SLOW_MS;) {
        long n = futex(&word, wake_op | F_PRIVATE, 2147483647, NULL, NULL, ANY_BIT);
        CHECK(n >= 0);
        woke += n > 0 ? n : 0;
        if (ws[0].result != 99 && ws[1].result != 99)
            break;
        nap_ms(2);
    }
    for (int i = 0; i < 2; i++) {
        CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(ws[i].result == 0 || ws[i].result == -EAGAIN);   /* woken, or it came late */
    }
    CHECK(woke <= 2);
    if (ws[0].result == 0 || ws[1].result == 0)
        CHECK(woke >= 1);   /* a waiter that slept was woken by a wake that said so */
}

void pt_raw_futex(void)
{
    check_plain();
    check_bitset_waits();
    check_masks();
    check_wake(F_WAKE);
    check_wake(F_WAKE_BITSET);
}
