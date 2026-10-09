/* posixtest's thread section: musl's pthreads on the layer.
 *
 *   - TLS in 64 threads: each sees the TLS image's initial values, writes
 *     its own, meets the others at a barrier and finds its own unchanged;
 *   - errno per thread: threads make different calls fail at once, and
 *     each sees its own errno (and the first thread its own);
 *   - a mutex counted up by 16 threads, exact;
 *   - pthread_join of 1,000 threads, each joined as soon as it returns, so
 *     its stack is unmapped at once (the kernel clears the tid word only
 *     after the thread has left it), and 100 detached threads, which unmap
 *     their own stacks; the job's pages come back. */
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pt.h"

#define TLS_THREADS   64
#define ERRNO_THREADS 8
#define COUNT_THREADS 16
#define COUNT_EACH    20000
#define JOINS         1000
#define DETACHED      100
#define SLACK_PAGES   2048   /* 8 MiB: malloc's and the thread table's own */

static __thread int tls_int = 1234;               /* .tdata */
static __thread char tls_buf[256] = "image";      /* .tdata */
static __thread long tls_zero[64];                /* .tbss */

static pthread_barrier_t barrier;
static pid_t main_tid;   /* the first thread's id */

static void *tls_worker(void *arg)
{
    long me = (long)arg;
    bool ok = tls_int == 1234 && !strcmp(tls_buf, "image") && !tls_zero[63] &&
              gettid() > 0 && gettid() != main_tid;
    tls_int = (int)me;
    snprintf(tls_buf, sizeof(tls_buf), "thread %ld", me);
    tls_zero[63] = me;
    pthread_barrier_wait(&barrier);
    char want[32];
    snprintf(want, sizeof(want), "thread %ld", me);
    ok = ok && tls_int == (int)me && !strcmp(tls_buf, want) && tls_zero[63] == me;
    return ok ? arg : NULL;
}

static void check_tls(void)
{
    pthread_t t[TLS_THREADS];
    main_tid = gettid();
    CHECK(pthread_barrier_init(&barrier, NULL, TLS_THREADS) == 0);
    for (long i = 0; i < TLS_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, tls_worker, (void *)(i + 1)) == 0);
    for (long i = 0; i < TLS_THREADS; i++) {
        void *r = NULL;
        CHECK(pthread_join(t[i], &r) == 0 && r == (void *)(i + 1));
    }
    pthread_barrier_destroy(&barrier);
    CHECK(tls_int == 1234 && !strcmp(tls_buf, "image"));   /* ours untouched */
}

static void *errno_worker(void *arg)
{
    long me = (long)arg;
    if (me % 2)
        (void)close(1000 + (int)me);   /* EBADF */
    else
        (void)mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* EINVAL */
    int mine = errno;
    pthread_barrier_wait(&barrier);
    return (void *)(long)(mine == (me % 2 ? EBADF : EINVAL) && errno == mine);
}

static void check_errno(void)
{
    pthread_t t[ERRNO_THREADS];
    CHECK(pthread_barrier_init(&barrier, NULL, ERRNO_THREADS + 1) == 0);
    for (long i = 0; i < ERRNO_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, errno_worker, (void *)i) == 0);
    errno = 0;
    CHECK(nanosleep(&(struct timespec){ 0, -1 }, NULL) == -1);   /* ours: EINVAL */
    pthread_barrier_wait(&barrier);
    CHECK(errno == EINVAL);
    for (long i = 0; i < ERRNO_THREADS; i++) {
        void *r = NULL;
        CHECK(pthread_join(t[i], &r) == 0 && r == (void *)1);
    }
    pthread_barrier_destroy(&barrier);
}

static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static long counted;

static void *count_worker(void *arg)
{
    for (int i = 0; i < COUNT_EACH; i++) {
        pthread_mutex_lock(&count_lock);
        counted++;
        pthread_mutex_unlock(&count_lock);
    }
    return arg;
}

static void check_mutex(void)
{
    pthread_t t[COUNT_THREADS];
    for (long i = 0; i < COUNT_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, count_worker, NULL) == 0);
    for (long i = 0; i < COUNT_THREADS; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(counted == (long)COUNT_THREADS * COUNT_EACH);
}

static void *quick(void *arg)
{
    return arg;
}

static volatile int detached_done;

static void *detached(void *arg)
{
    __atomic_add_fetch(&detached_done, 1, __ATOMIC_RELEASE);
    return arg;
}

static void check_joins(void)
{
    uint64_t before = pt_job_pages();
    unsigned joined = 0;
    for (long i = 0; i < JOINS; i++) {
        pthread_t t;
        void *r = NULL;
        if (pthread_create(&t, NULL, quick, (void *)i) == 0 && pthread_join(t, &r) == 0 &&
            r == (void *)i)
            joined++;
    }
    CHECK(joined == JOINS);
    static pthread_t live[JOINS];   /* all at once, then joined in turn */
    unsigned made = 0;
    while (made < JOINS && pthread_create(&live[made], NULL, quick, (void *)(long)made) == 0)
        made++;
    CHECK(made == JOINS);
    joined = 0;
    for (unsigned i = 0; i < made; i++) {
        void *r = NULL;
        joined += pthread_join(live[i], &r) == 0 && r == (void *)(long)i;
    }
    CHECK(joined == made);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < DETACHED; i++) {
        pthread_t t;
        CHECK(pthread_create(&t, &a, detached, NULL) == 0);
    }
    pthread_attr_destroy(&a);
    for (int i = 0; i < 500 && __atomic_load_n(&detached_done, __ATOMIC_ACQUIRE) < DETACHED; i++)
        usleep(10 * 1000);
    CHECK(detached_done == DETACHED);
    usleep(100 * 1000);   /* the last of them leave after counting */
    CHECK(pt_job_pages() <= before + SLACK_PAGES);
}

void pt_thread(char **argv __attribute__((unused)))
{
    check_tls();
    check_errno();
    check_mutex();
    check_joins();
}
