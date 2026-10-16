/* posixtest's fault section (docs/SIGNALS-PLAN.md section 1, SG3): a
 * thread's own faults as Linux's signals, run on the faulting thread.
 *
 *   - SIGSEGV with si_addr the address, REG_TRAPNO 14, REG_CR2 and
 *     REG_ERR's bits: a load from a guard page (the handler moves REG_RIP
 *     past the instruction by its length and returns), a store to a
 *     read-only page (SEGV_ACCERR), a load where nothing is mapped
 *     (SEGV_MAPERR), a load from a PROT_NONE reservation (Linux says
 *     SEGV_ACCERR for it and for the guard page: a line says so if ours
 *     differs), a non-canonical address (SI_KERNEL, si_addr 0);
 *   - V8's trap handler's shape: RIP moved to a landing pad and R10 the
 *     faulting RIP, which the pad returns;
 *   - the vector registers: the handler sees the thread's xmm0 through
 *     fpregs (with Linux's fpx_sw_bytes and FP_XSTATE_MAGIC2 after the
 *     area) and a value it writes there is the thread's xmm0 after;
 *   - SIGFPE (FPE_INTDIV) on a division by zero, SIGILL (ILL_ILLOPN) on
 *     ud2, SIGTRAP (SI_KERNEL, Linux's) on int3, SIGBUS (BUS_ADRERR) past
 *     the end of a shrunk VMO (a file truncated under its mapping), each
 *     with Linux's si_addr;
 *   - masks: the signal and sa_mask blocked in the handler and not after;
 *     uc_sigmask edited by the handler is the mask after; SA_NODEFER runs
 *     a nested handler for a fault in the handler; siglongjmp out of a
 *     handler 1,000 times; SA_RESETHAND; a fault on a second thread is
 *     handled on that thread;
 *   - V8's "restore and return": a handler that installs another and
 *     returns re-runs the instruction into the other;
 *   - sent, not faults: raise's si_code <= 0; rt_tgsigqueueinfo to
 *     ourselves with SEGV_MAPERR and an address, given as sent;
 *   - children that must end: a fault with no handler (the kernel's kill
 *     as ever), with SIGSEGV default (SIGBUS handled: the entry set),
 *     ignored or blocked, a second fault after SA_RESETHAND, a fault in a
 *     handler without SA_NODEFER, a handler that resets to SIG_DFL and
 *     returns (Chromium's and V8's way out), and Chromium's stack dump
 *     (the registers printed, a sent SIGSEGV sent again with
 *     rt_tgsigqueueinfo). */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <jamos/libjam.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "pt.h"

#define PG      4096ul
#define SELF    "/boot/bin/posixtest"
#define JUMPS   1000
#define V8_RUNS 100
#define WILD    ((char *)0x10)                    /* nothing is ever mapped there */
#define NONCANON ((char *)0x8000000000001000ull)  /* not an address at all: #GP */

extern char **environ;

/* The faulting instructions: each a function returning rax, with labels
 * at the instruction and at the next one (where a handler moves REG_RIP). */
__asm__(".text\n"
        /* long fl_load(const void *p): the byte at p (3 bytes: movzbl) */
        "fl_load:\n"
        "fl_load_at: movzbl (%rdi), %eax\n"
        "fl_load_end: ret\n"
        /* void fl_store(void *p): 1 at p */
        "fl_store:\n"
        "fl_store_at: movb $1, (%rdi)\n"
        "fl_store_end: ret\n"
        /* long fl_div(long n): n / 0 */
        "fl_div: mov %rdi, %rax\n"
        "    xor %edx, %edx\n"
        "    xor %ecx, %ecx\n"
        "fl_div_at: div %rcx\n"
        "fl_div_end: ret\n"
        /* long fl_ud2(void): 7, after the ud2 */
        "fl_ud2:\n"
        "fl_ud2_at: ud2\n"
        "fl_ud2_end: mov $7, %eax\n"
        "    ret\n"
        /* long fl_int3(void): 9, after the int3 */
        "fl_int3: int3\n"
        "fl_int3_end: mov $9, %eax\n"
        "    ret\n"
        /* long fl_v8(const void *p): the load faults; the landing pad
         * returns R10 (where V8's trap handler puts the faulting RIP) */
        "fl_v8:\n"
        "fl_v8_at: movzbl (%rdi), %eax\n"
        "    ret\n"
        "fl_v8_pad: mov %r10, %rax\n"
        "    ret\n"
        /* void fl_xmm(const void *p, const void *in, void *out): xmm0 the
         * 16 bytes at in, a load from p, then xmm0 to out */
        "fl_xmm: movdqu (%rsi), %xmm0\n"
        "fl_xmm_at: movzbl (%rdi), %eax\n"
        "fl_xmm_end: movdqu %xmm0, (%rdx)\n"
        "    ret\n");
long fl_load(const volatile void *p);
void fl_store(volatile void *p);
long fl_div(long n);
long fl_ud2(void);
long fl_int3(void);
long fl_v8(const volatile void *p);
void fl_xmm(const volatile void *p, const void *in, void *out);
extern char fl_load_at[], fl_load_end[], fl_store_at[], fl_store_end[], fl_div_at[],
    fl_div_end[], fl_ud2_at[], fl_ud2_end[], fl_int3_end[], fl_v8_at[], fl_v8_pad[],
    fl_xmm_end[];

static greg_t addr_of(const void *p)
{
    return (greg_t)(uintptr_t)p;
}

/* ---- in this process ---------------------------------------------------------------- */

static volatile int       runs, got_sig, got_code, got_blocked, depth, max_depth;
static volatile uintptr_t got_addr, got_rip, got_trapno, got_err, got_cr2;
static volatile pthread_t got_thread;
static volatile int       skip;        /* note() moves REG_RIP on by this many bytes, */
static char *volatile     resume_at;   /* or to here (neither: it returns unchanged) */

/* What a fault's handler saw; then on past it. */
static void note(int sig, siginfo_t *si, void *ucv)
{
    greg_t *g = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    sigset_t now;
    pthread_sigmask(SIG_BLOCK, NULL, &now);
    runs++;
    got_sig = sig;
    got_code = si->si_code;
    got_addr = (uintptr_t)si->si_addr;
    got_rip = (uintptr_t)g[REG_RIP];
    got_trapno = (uintptr_t)g[REG_TRAPNO];
    got_err = (uintptr_t)g[REG_ERR];
    got_cr2 = (uintptr_t)g[REG_CR2];
    got_blocked = sigismember(&now, sig);
    got_thread = pthread_self();
    if (skip)
        g[REG_RIP] += skip;
    else if (resume_at)
        g[REG_RIP] = addr_of(resume_at);
}

/* fn handles sig (SA_SIGINFO and flags); the count starts again. */
static void on(int sig, int flags, void (*fn)(int, siginfo_t *, void *))
{
    struct sigaction sa = { .sa_sigaction = fn, .sa_flags = SA_SIGINFO | flags };
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(sig, &sa, NULL) == 0);
    runs = 0;
    skip = 0;
    resume_at = NULL;
}

static bool blocked_now(int sig)
{
    sigset_t now;
    pthread_sigmask(SIG_BLOCK, NULL, &now);
    return sigismember(&now, sig);
}

static const char *segv_name(int code)
{
    return code == SEGV_MAPERR ? "SEGV_MAPERR" : code == SEGV_ACCERR ? "SEGV_ACCERR" : "another code";
}

/* The last SIGSEGV's code where Linux's is `linux`: a line if ours differs
 * (docs/SIGNALS-PLAN.md's *unsure*: what CL1's reservations give). */
static void segv_code(const char *what, int linux)
{
    CHECK(got_code == SEGV_MAPERR || got_code == SEGV_ACCERR);
    if (got_code != linux)
        printf("posixtest: fault: %s gives %s (Linux: %s)\n", what, segv_name(got_code),
               segv_name(linux));
}

static void check_segv(void)
{
    char *p = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    p[0] = p[2 * PG] = 1;
    CHECK(mprotect(p + PG, PG, PROT_NONE) == 0);   /* a guard page */
    on(SIGSEGV, 0, note);

    skip = 3;   /* movzbl (%rdi), %eax */
    fl_load(p + PG + 5);
    CHECK(runs == 1 && got_sig == SIGSEGV && got_addr == (uintptr_t)(p + PG + 5));
    CHECK(got_rip == (uintptr_t)fl_load_at && got_trapno == 14 && got_cr2 == got_addr);
    CHECK(!(got_err & 2) && (got_err & 4));   /* a read, in user mode */
    CHECK(got_blocked && !blocked_now(SIGSEGV));
    segv_code("a PROT_NONE guard page", SEGV_ACCERR);

    skip = 0;
    resume_at = fl_store_end;
    CHECK(mprotect(p, PG, PROT_READ) == 0);
    fl_store(p + 7);
    CHECK(runs == 2 && got_code == SEGV_ACCERR && got_addr == (uintptr_t)(p + 7));
    CHECK(got_rip == (uintptr_t)fl_store_at && (got_err & 7) == 7 && p[7] == 0);   /* present, write, user */

    resume_at = fl_load_end;
    CHECK(munmap(p, 3 * PG) == 0);
    fl_load(p + PG + 9);
    CHECK(runs == 3 && got_code == SEGV_MAPERR && got_addr == (uintptr_t)(p + PG + 9));

    char *r = mmap(NULL, 64 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(r != MAP_FAILED);
    if (r != MAP_FAILED) {
        fl_load(r + 33 * PG + 1);   /* V8's guard regions, Wasm's memories */
        CHECK(runs == 4 && got_sig == SIGSEGV && got_addr == (uintptr_t)(r + 33 * PG + 1));
        segv_code("a PROT_NONE reservation", SEGV_ACCERR);
        CHECK(munmap(r, 64 * PG) == 0);
    }

    fl_load(NONCANON);   /* a general protection fault: no address (Linux's) */
    CHECK(runs == 5 && got_sig == SIGSEGV && got_code == SI_KERNEL && got_addr == 0);
    CHECK(got_trapno == 13 && got_rip == (uintptr_t)fl_load_at);
    signal(SIGSEGV, SIG_DFL);
}

/* V8's TryHandleSignal, cut down: a kernel's SIGSEGV at a protected
 * instruction lands on the pad with the faulting RIP in R10. */
static void v8_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    greg_t *g = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    runs++;
    if (si->si_code <= 0 || g[REG_RIP] != addr_of(fl_v8_at))
        return;
    g[REG_R10] = g[REG_RIP];
    g[REG_RIP] = addr_of(fl_v8_pad);
}

static void check_v8(void)
{
    char *r = mmap(NULL, 16 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(r != MAP_FAILED);
    if (r == MAP_FAILED)
        return;
    on(SIGSEGV, 0, v8_handler);
    int right = 0;
    for (int i = 0; i < V8_RUNS; i++)
        right += fl_v8(r + (unsigned)i * 97) == (long)(uintptr_t)fl_v8_at;
    CHECK(right == V8_RUNS && runs == V8_RUNS);
    signal(SIGSEGV, SIG_DFL);
    CHECK(munmap(r, 16 * PG) == 0);
}

#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u

static uint32_t     xmm_saw[4];
static volatile int xmm_layout_ok;

/* Read xmm0 through fpregs, check Linux's layout of the extended state,
 * write a new xmm0 there, go on past the load. */
static void xmm_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ucv;
    fpregset_t fp = uc->uc_mcontext.fpregs;
    runs++;
    memcpy(xmm_saw, fp->_xmm[0].element, sizeof(xmm_saw));
    if (uc->uc_flags & 1) {   /* UC_FP_XSTATE */
        const uint8_t *b = (const uint8_t *)fp;
        uint32_t sw[5], magic2;
        memcpy(sw, b + 464, sizeof(sw));   /* fpx_sw_bytes: magic1, extended_size, xfeatures, xstate_size */
        memcpy(&magic2, b + sw[1] - 4, sizeof(magic2));
        xmm_layout_ok = sw[0] == FP_XSTATE_MAGIC1 && sw[1] == sw[4] + 4 && (sw[2] & 3) == 3 &&
                        sw[4] >= 576 && magic2 == FP_XSTATE_MAGIC2 && ((uintptr_t)fp & 63) == 0;
    } else {
        xmm_layout_ok = ((uintptr_t)fp & 15) == 0;   /* FXSAVE's area alone */
    }
    for (unsigned i = 0; i < 4; i++)
        fp->_xmm[0].element[i] = 0x5a5a0000u + i;
    uc->uc_mcontext.gregs[REG_RIP] = addr_of(fl_xmm_end);
}

static void check_xmm(void)
{
    on(SIGSEGV, 0, xmm_handler);
    uint32_t in[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 }, out[4] = { 0 };
    fl_xmm(WILD, in, out);
    CHECK(runs == 1 && !memcmp(xmm_saw, in, sizeof(in)) && xmm_layout_ok);
    CHECK(out[0] == 0x5a5a0000u && out[3] == 0x5a5a0003u);
    signal(SIGSEGV, SIG_DFL);
}

static void check_traps(void)
{
    on(SIGFPE, 0, note);
    resume_at = fl_div_end;
    fl_div(7);
    CHECK(runs == 1 && got_sig == SIGFPE && got_code == FPE_INTDIV);
    CHECK(got_addr == (uintptr_t)fl_div_at && got_rip == got_addr && got_trapno == 0);
    signal(SIGFPE, SIG_DFL);

    on(SIGILL, 0, note);
    resume_at = fl_ud2_end;
    CHECK(fl_ud2() == 7);
    CHECK(runs == 1 && got_sig == SIGILL && got_code == ILL_ILLOPN);
    CHECK(got_addr == (uintptr_t)fl_ud2_at && got_rip == got_addr && got_trapno == 6);
    signal(SIGILL, SIG_DFL);

    on(SIGTRAP, 0, note);   /* returns unchanged: RIP is after the int3 */
    CHECK(fl_int3() == 9);
    CHECK(runs == 1 && got_sig == SIGTRAP && got_code == SI_KERNEL);
    CHECK(got_rip == (uintptr_t)fl_int3_end && got_trapno == 3);
    signal(SIGTRAP, SIG_DFL);
}

/* SIGBUS: a VMO mapped in a hole of a mapping of libc's (as mem.c's), then
 * shrunk under it: the page past its end is Linux's truncated file. */
static void check_bus(void)
{
    char *p = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED)
        return;
    CHECK(munmap(p + PG, 2 * PG) == 0);
    handle_t v;
    uint64_t at = (uint64_t)(uintptr_t)(p + PG);
    CHECK(jam_vmo_create(2 * PG, 0, &v) == OK);
    CHECK(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, 2 * PG,
                       VMAR_READ | VMAR_WRITE | VMAR_FIXED, &at) == OK);
    p[PG] = 5;
    p[2 * PG] = 6;
    CHECK(jam_vmo_set_size(v, PG) == OK);
    on(SIGBUS, 0, note);
    resume_at = fl_load_end;
    fl_load(p + 2 * PG + 3);
    CHECK(runs == 1 && got_sig == SIGBUS && got_code == BUS_ADRERR);
    CHECK(got_addr == (uintptr_t)(p + 2 * PG + 3) && got_trapno == 14 && p[PG] == 5);
    signal(SIGBUS, SIG_DFL);
    CHECK(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), at, 2 * PG) == OK);
    CHECK(jam_handle_close(v) == OK);
    CHECK(munmap(p, 4 * PG) == 0);
}

/* The mask in the handler (its signal and sa_mask), and the one it leaves
 * in uc_sigmask is the thread's after. */
static void mask_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ucv;
    runs++;
    got_blocked = blocked_now(SIGSEGV) && blocked_now(SIGUSR1) &&
                  !sigismember(&uc->uc_sigmask, SIGSEGV) && !sigismember(&uc->uc_sigmask, SIGUSR1);
    sigaddset(&uc->uc_sigmask, SIGUSR2);
    uc->uc_mcontext.gregs[REG_RIP] = addr_of(fl_load_end);
}

/* A fault in the handler itself: SA_NODEFER lets a nested handler run. */
static void nested_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    greg_t *g = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    int d = ++depth;
    if (d > max_depth)
        max_depth = d;
    runs++;
    if (d == 1)
        fl_load(WILD);
    g[REG_RIP] = addr_of(fl_load_end);
    depth--;
}

static sigjmp_buf jump;

static void jump_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    (void)uc;
    runs++;
    siglongjmp(jump, 1);
}

static void check_masks(void)
{
    struct sigaction sa = { .sa_sigaction = mask_handler, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);
    runs = got_blocked = 0;
    fl_load(WILD);
    CHECK(runs == 1 && got_blocked);
    CHECK(blocked_now(SIGUSR2) && !blocked_now(SIGSEGV) && !blocked_now(SIGUSR1));
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &s, NULL) == 0);

    on(SIGSEGV, SA_NODEFER, nested_handler);
    depth = max_depth = 0;
    fl_load(WILD);
    CHECK(runs == 2 && max_depth == 2 && depth == 0);

    on(SIGSEGV, 0, jump_handler);
    volatile int back = 0;
    for (volatile int i = 0; i < JUMPS; i++) {
        if (sigsetjmp(jump, 1) == 0)
            fl_load(WILD);
        else
            back++;
    }
    CHECK(back == JUMPS && runs == JUMPS && !blocked_now(SIGSEGV));

    on(SIGSEGV, SA_RESETHAND, note);
    resume_at = fl_load_end;
    fl_load(WILD);
    struct sigaction old;
    CHECK(sigaction(SIGSEGV, NULL, &old) == 0);
    CHECK(runs == 1 && old.sa_handler == SIG_DFL);
}

static void *fault_thread(void *arg)
{
    fl_load(arg);
    return NULL;
}

static volatile int first_runs, second_runs;

static void second_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    second_runs++;
    ((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP] = addr_of(fl_load_end);
}

/* V8's way when a fault isn't its own: the old action back, then return,
 * so the instruction faults again into it. */
static void first_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    (void)ucv;
    first_runs++;
    struct sigaction sa = { .sa_sigaction = second_handler, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

static void check_threads_and_again(void)
{
    on(SIGSEGV, 0, note);
    resume_at = fl_load_end;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, fault_thread, WILD) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(runs == 1 && pthread_equal(got_thread, t) && got_addr == (uintptr_t)WILD);

    on(SIGSEGV, 0, first_handler);
    first_runs = second_runs = 0;
    fl_load(WILD);
    CHECK(first_runs == 1 && second_runs == 1);
    signal(SIGSEGV, SIG_DFL);
}

static void check_sent(void)
{
    on(SIGSEGV, 0, note);
    got_code = 99;
    CHECK(raise(SIGSEGV) == 0);
    CHECK(runs == 1 && got_sig == SIGSEGV && got_code <= 0);

    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGSEGV;
    si.si_code = SEGV_MAPERR;
    si.si_addr = (void *)0x1234;
    CHECK(syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), SIGSEGV, &si) == 0);
    CHECK(runs == 2 && got_code == SEGV_MAPERR && got_addr == 0x1234);
    signal(SIGSEGV, SIG_DFL);
}

/* ---- children that must end --------------------------------------------------------- */

/* Start `posixtest --child-fault what`, its 1 the write end of a pipe
 * whose read end is *out. */
static pid_t start(const char *what, int *out)
{
    char *argv[] = { "posixtest", "--child-fault", (char *)what, NULL };
    int p[2] = { -1, -1 };
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    pid_t pid = -1;
    CHECK(posix_spawn(&pid, SELF, &fa, NULL, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    *out = p[0];
    return pid;
}

/* What the child wrote until it ended, NUL-ended; the pipe closed. */
static void drain(int fd, char *buf, size_t size)
{
    size_t n = 0;
    ssize_t r;
    while (n + 1 < size && (r = read(fd, buf + n, size - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = 0;
    close(fd);
}

/* The child's end, by sig: the kernel's kill (waitpid says SIGKILL for it
 * until SG4 gives a fault's kill its signal) or libc's end with sig. */
static int end_of(pid_t pid)
{
    int st = -1;
    CHECK(waitpid(pid, &st, 0) == pid);
    return st;
}

static bool ended_by(int st, int sig)
{
    return WIFSIGNALED(st) && (WTERMSIG(st) == sig || WTERMSIG(st) == SIGKILL);
}

/* Run child `what`: it ends by its fault, having written `wrote`. */
static void child_ends(const char *what, const char *wrote)
{
    int fd;
    pid_t pid = start(what, &fd);
    char buf[64];
    drain(fd, buf, sizeof(buf));
    int st = end_of(pid);
    if (!ended_by(st, SIGSEGV) || strcmp(buf, wrote)) {
        printf("posixtest: fault: child %s: status %#x, wrote \"%s\"\n", what, st, buf);
        CHECK(!"the child ended by its fault");
    }
}

/* Chromium's stack dump: the lines, the registers' (rip the load's),
 * the end. A real fault ends by the kernel's kill; a sent SIGSEGV, sent
 * again by rt_tgsigqueueinfo, by libc's with SIGSEGV itself. */
static void check_dump(void)
{
    int fd;
    pid_t pid = start("dump", &fd);
    char buf[512], want[128];
    drain(fd, buf, sizeof(buf));
    int st = end_of(pid);
    CHECK(ended_by(st, SIGSEGV));
    snprintf(want, sizeof(want), "Received signal 11 SEGV_MAPERR %lx\n", (unsigned long)(uintptr_t)WILD);
    CHECK(strstr(buf, want) != NULL);
    snprintf(want, sizeof(want), " rip: %016lx ", (unsigned long)(uintptr_t)fl_load_at);
    CHECK(strstr(buf, want) != NULL && strstr(buf, " trp: 000000000000000e ") != NULL);
    CHECK(strstr(buf, "[end of stack trace]\n") != NULL);

    pid = start("dump-sent", &fd);
    drain(fd, buf, sizeof(buf));
    st = end_of(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    CHECK(strstr(buf, "Received signal 11 SI_USER") != NULL);
    CHECK(strstr(buf, "[end of stack trace]\n") != NULL);
}

void pt_fault(char **argv)
{
    (void)argv;
    check_segv();
    check_v8();
    check_xmm();
    check_traps();
    check_bus();
    check_masks();
    check_threads_and_again();
    check_sent();
    child_ends("none", "");
    child_ends("default", "");
    child_ends("ignored", "");
    child_ends("blocked", "");
    child_ends("resethand", "h");
    child_ends("nested", "h");
    child_ends("reset-return", "h");
    check_dump();
}

/* ---- the children ------------------------------------------------------------------- */

static void say(const char *s)
{
    (void)write(1, s, strlen(s));
}

static void child_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    (void)ucv;
    say("h");
}

static void child_nested(int sig, siginfo_t *si, void *ucv)
{
    child_handler(sig, si, ucv);
    fl_load(WILD);   /* its signal blocked: the kernel's kill */
}

static void child_reset(int sig, siginfo_t *si, void *ucv)
{
    child_handler(sig, si, ucv);
    signal(sig, SIG_DFL);
}

/* Chromium's StackDumpSignalHandler, in short: its first line, the
 * registers from the ucontext, then the default action back and, for a
 * signal that was sent (si_code <= 0), the same signal sent again to this
 * thread; return. */
static void child_dump(int sig, siginfo_t *si, void *ucv)
{
    static const char *const names[] = { "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                                         "di", "si", "bp", "bx", "dx", "ax", "cx", "sp",
                                         "rip", "efl", "cgf", "erf", "trp", "msk", "cr2" };
    const greg_t *g = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    char line[160];
    const char *code = si->si_code == SEGV_MAPERR ? "SEGV_MAPERR"
                       : si->si_code == SI_USER   ? "SI_USER"
                                                  : "?";
    snprintf(line, sizeof(line), "Received signal %d %s %lx\n", sig, code,
             (unsigned long)(uintptr_t)si->si_addr);
    say(line);
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(line, sizeof(line), " %3s: %016lx%s", names[i], (unsigned long)g[i],
                 i % 4 == 3 ? " \n" : " ");
        say(line);
    }
    say("\n[end of stack trace]\n");
    signal(sig, SIG_DFL);
    if (si->si_code <= 0 &&
        syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), sig, si) != 0)
        raise(sig);
}

static void handle(int sig, int flags, void (*fn)(int, siginfo_t *, void *))
{
    struct sigaction sa = { .sa_sigaction = fn, .sa_flags = SA_SIGINFO | flags };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

int pt_child_fault(int argc, char **argv)
{
    const char *what = argc > 2 ? argv[2] : "";
    if (!strcmp(what, "default") || !strcmp(what, "ignored"))
        handle(SIGBUS, 0, child_handler);   /* the entry is set; SIGSEGV isn't handled */
    if (!strcmp(what, "ignored"))
        signal(SIGSEGV, SIG_IGN);
    if (!strcmp(what, "blocked")) {
        handle(SIGSEGV, 0, child_handler);
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGSEGV);
        sigprocmask(SIG_BLOCK, &s, NULL);
    }
    if (!strcmp(what, "resethand"))
        handle(SIGSEGV, SA_RESETHAND, child_handler);
    if (!strcmp(what, "nested"))
        handle(SIGSEGV, 0, child_nested);
    if (!strcmp(what, "reset-return"))
        handle(SIGSEGV, 0, child_reset);
    if (!strcmp(what, "dump") || !strcmp(what, "dump-sent"))
        handle(SIGSEGV, SA_RESETHAND, child_dump);   /* as Chromium installs it */
    if (!strcmp(what, "dump-sent"))
        kill(getpid(), SIGSEGV);
    else
        fl_load(WILD);
    say("survived");
    return 3;
}
