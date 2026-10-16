/* posixtest's execinfo section: backtrace(), backtrace_symbols() and
 * backtrace_symbols_fd() (user/posix/glibc/execinfo.c, docs/CR-PLAN.md
 * "BT notes"):
 *
 *   - four nested functions: the first frame is in the innermost and each
 *     next is the return address that function was called with (taken
 *     with __builtin_return_address), up to the section's own frame and on
 *     through main to the program's start; a short array gets the first
 *     frames only, and size 0 stores nothing;
 *   - the same in a second thread (its frames end in libc's thread start);
 *   - from a signal handler: the handler's frame, then libc's delivery
 *     (the layer's frames and musl's raise), then the function that raised
 *     the signal and its caller;
 *   - through musl: a qsort comparator's trace reaches the function that
 *     called qsort;
 *   - backtrace_symbols_fd() on a pipe, backtrace_symbols() in one block:
 *     "name() [0xaddr]" for an address in the program, "[0xaddr]" for one
 *     outside it. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "pt.h"

#define DEPTH 40
#define SPAN  512   /* every test function below is smaller than this */

#define NOINLINE __attribute__((noinline, noclone))
/* A call that is not the function's last act cannot be a tail call (a jump
 * that would drop the function from the trace). */
#define NOT_LAST() __asm__ volatile("" ::: "memory")

struct trace {
    void *buf[DEPTH];
    int   n;
    void *ra[4];   /* the return addresses level3, level2, level1 and caller were called with */
};

static bool inside(void *pc, void *fn)
{
    uintptr_t p = (uintptr_t)pc, f = (uintptr_t)fn;
    return p > f && p < f + SPAN;
}

static NOINLINE int level3(struct trace *t)
{
    t->ra[0] = __builtin_return_address(0);
    t->n = backtrace(t->buf, DEPTH);
    NOT_LAST();
    return t->n;
}

static NOINLINE int level2(struct trace *t)
{
    t->ra[1] = __builtin_return_address(0);
    int r = level3(t);
    NOT_LAST();
    return r;
}

static NOINLINE int level1(struct trace *t)
{
    t->ra[2] = __builtin_return_address(0);
    int r = level2(t);
    NOT_LAST();
    return r;
}

static NOINLINE int caller(struct trace *t)
{
    t->ra[3] = __builtin_return_address(0);
    int r = level1(t);
    NOT_LAST();
    return r;
}

/* t is a trace caller() made: the four frames are what they should be. */
static void check_nested(const struct trace *t, int min_n)
{
    CHECK(t->n >= min_n && t->n <= DEPTH);
    if (t->n < 5)
        return;
    CHECK(inside(t->buf[0], (void *)level3));
    CHECK(t->buf[1] == t->ra[0]);
    CHECK(t->buf[2] == t->ra[1]);
    CHECK(t->buf[3] == t->ra[2]);
    CHECK(t->buf[4] == t->ra[3]);
    CHECK(inside(t->buf[1], (void *)level2));
    CHECK(inside(t->buf[2], (void *)level1));
    CHECK(inside(t->buf[3], (void *)caller));
}

/* ---- nested, and short arrays ---------------------------------------------------------- */

static struct trace main_trace;

static void test_nested(void)
{
    memset(&main_trace, 0, sizeof main_trace);
    caller(&main_trace);
    /* caller's caller is this function, then pt_execinfo, main (main.c's
     * run is inlined or a frame of its own), and musl's start and the
     * layer's below (__libc_start_main, __jam_start, _start_c, _start). */
    check_nested(&main_trace, 7);
    CHECK(inside(main_trace.buf[4], (void *)test_nested));
    printf("posixtest: execinfo: nested %d frames\n", main_trace.n);

    void *few[4];
    memset(few, 0xaa, sizeof few);
    int n = backtrace(few, 1);
    CHECK(n == 1);
    CHECK(inside(few[0], (void *)test_nested));
    CHECK(few[1] == (void *)(uintptr_t)0xaaaaaaaaaaaaaaaaull);
    CHECK(backtrace(few, 0) == 0);

    /* A shorter array is the long one's first frames. */
    void *two[2];
    n = backtrace(two, 2);
    NOT_LAST();
    CHECK(n == 2);
    CHECK(inside(two[0], (void *)test_nested));
    CHECK(two[1] != NULL);
}

/* ---- a second thread ------------------------------------------------------------------- */

static struct trace thread_trace;

static void *thread_main(void *arg)
{
    caller(arg);
    NOT_LAST();
    return arg;
}

static void test_thread(void)
{
    memset(&thread_trace, 0, sizeof thread_trace);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, thread_main, &thread_trace) == 0);
    CHECK(pthread_join(th, NULL) == 0);
    /* caller's caller is thread_main, then libc's thread start. */
    check_nested(&thread_trace, 6);
    CHECK(inside(thread_trace.buf[4], (void *)thread_main));
}

/* ---- a signal handler ------------------------------------------------------------------ */

static void *sig_buf[DEPTH];
static volatile int sig_n;
static void *sig_raiser_ra;

static NOINLINE void on_usr1(int sig)
{
    (void)sig;
    sig_n = backtrace(sig_buf, DEPTH);
    NOT_LAST();
}

static NOINLINE void raiser(void)
{
    sig_raiser_ra = __builtin_return_address(0);
    raise(SIGUSR1);
    NOT_LAST();
}

static void test_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    sig_n = 0;
    raiser();
    NOT_LAST();
    signal(SIGUSR1, SIG_DFL);

    CHECK(sig_n > 2);
    CHECK(inside(sig_buf[0], (void *)on_usr1));
    /* libc's frames lie between the handler and the function that raised
     * the signal: the delivery (run_handler, deliver, sig_call, the call
     * layer's entry) and musl's raise. */
    int k = 0;
    for (int i = 1; i < sig_n; i++)
        if (inside(sig_buf[i], (void *)raiser)) {
            k = i;
            break;
        }
    CHECK(k > 1 && k <= 12);
    CHECK(k > 0 && k + 1 < sig_n && sig_buf[k + 1] == sig_raiser_ra);
    printf("posixtest: execinfo: handler %d frames, raiser at %d\n", sig_n, k);
}

/* ---- through musl ---------------------------------------------------------------------- */

static void *qs_buf[DEPTH];
static int qs_n;

static NOINLINE int by_value(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (!qs_n)
        qs_n = backtrace(qs_buf, DEPTH);
    return x < y ? -1 : x > y;
}

static NOINLINE void sorter(void)
{
    int v[16];
    for (int i = 0; i < 16; i++)
        v[i] = (i * 7) % 16;
    qsort(v, 16, sizeof v[0], by_value);
    NOT_LAST();
    for (int i = 1; i < 16; i++)
        CHECK(v[i - 1] <= v[i]);
}

static void test_qsort(void)
{
    qs_n = 0;
    sorter();
    CHECK(qs_n > 2);
    CHECK(inside(qs_buf[0], (void *)by_value));
    int k = 0;
    for (int i = 1; i < qs_n; i++)
        if (inside(qs_buf[i], (void *)sorter)) {
            k = i;
            break;
        }
    CHECK(k > 1 && k <= 10);   /* musl's qsort frames between */
    printf("posixtest: execinfo: qsort comparator %d frames, sorter at %d\n", qs_n, k);
}

/* ---- the lines ------------------------------------------------------------------------- */

/* The line for pc as the test spells it: in_program says which form. */
static void expect_line(char *out, size_t size, void *pc, bool in_program)
{
    unsigned long a = (unsigned long)(uintptr_t)pc;
    if (in_program)
        snprintf(out, size, "%s() [0x%lx]", program_invocation_name, a);
    else
        snprintf(out, size, "[0x%lx]", a);
}

static void test_lines(void)
{
    /* The frames of a trace (all code of the program), then a stack
     * address and a heap one, which are not. */
    int local = 0;
    char *heap = malloc(16);
    void *pcs[DEPTH + 2];
    int n = main_trace.n < 8 ? main_trace.n : 8;
    for (int i = 0; i < n; i++)
        pcs[i] = main_trace.buf[i];
    pcs[n] = &local;
    pcs[n + 1] = heap;
    int total = n + 2;

    char want[DEPTH + 2][160];
    for (int i = 0; i < total; i++)
        expect_line(want[i], sizeof want[i], pcs[i], i < n);

    char **s = backtrace_symbols(pcs, total);
    CHECK(s != NULL);
    if (s) {
        /* One block: the pointers, then the strings, as glibc's. */
        CHECK((char *)s[0] == (char *)(s + total));
        for (int i = 0; i < total; i++)
            CHECK(!strcmp(s[i], want[i]));
        free(s);
    }
    s = backtrace_symbols(pcs, 0);
    CHECK(s != NULL);
    free(s);

    /* The same lines on a descriptor, each ended by a newline. */
    int p[2];
    CHECK(pipe(p) == 0);
    backtrace_symbols_fd(pcs, total, p[1]);
    close(p[1]);
    char text[DEPTH * 160];
    size_t len = 0;
    for (;;) {
        ssize_t r = read(p[0], text + len, sizeof text - len - 1);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    close(p[0]);
    text[len] = '\0';
    char *at = text;
    for (int i = 0; i < total; i++) {
        char *nl = strchr(at, '\n');
        CHECK(nl != NULL);
        if (!nl)
            break;
        *nl = '\0';
        CHECK(!strcmp(at, want[i]));
        at = nl + 1;
    }
    CHECK(*at == '\0');
    free(heap);
}

void pt_execinfo(char **argv)
{
    (void)argv;
    test_nested();
    test_thread();
    test_handler();
    test_qsort();
    test_lines();
}
