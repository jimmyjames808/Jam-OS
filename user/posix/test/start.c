/* posixtest's start section: what a POSIX program starts with.
 *
 *   - this run's own auxiliary vector (the page size, the program headers
 *     with PT_TLS among them, 16 random bytes, the ids, AT_SECURE 0) and
 *     the TZ the start added (the starter gave none);
 *   - a child started with libjam's spawn, given an argv and an
 *     environment with an empty string, spaces, '=' and a long string:
 *     it gets them exactly (and keeps the TZ it was given);
 *   - a child started with a hand-over message (a pipe as descriptor 1,
 *     the null device as 0, nothing at 2): it writes through 1 to us,
 *     reads the end of the file at once from 0, and 2 is EBADF;
 *   - the clocks, a sleep, getrandom and uname. */
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <errno.h>
#include <jamos/libjam.h>
#include <posixfds.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/random.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define LONG_ARG 1000   /* bytes of the long argument */
#define CHILD_WAIT_NS (30 * NS_PER_S)

static const char *const child_env[] = { "A=1", "EMPTY=", "SPACE=a b", "TZ=UTC0", NULL };

static void long_arg(char *buf)
{
    memset(buf, 'L', LONG_ARG);
    buf[LONG_ARG] = '\0';
}

uint64_t pt_job_pages(void)
{
    struct job_info ji;
    return jam_job_get_info(startup_handle(SR_JOB), &ji) == OK ? ji.used[JOB_LIMIT_PAGES] : 0;
}

int pt_child_start(int argc, char **argv)
{
    char big[LONG_ARG + 1];
    long_arg(big);
    const char *want[] = { "posixtest", "--child-start", "", "two words", "x=y", big };
    CHECK(argc == 6);
    for (int i = 0; i < 6 && i < argc; i++)
        CHECK(!strcmp(argv[i], want[i]));
    CHECK(argv[argc] == NULL);
    extern char **environ;
    int n = 0;
    for (; environ[n]; n++)
        CHECK(n < 4 && !strcmp(environ[n], child_env[n]));
    CHECK(n == 4);
    CHECK(!strcmp(getenv("TZ"), "UTC0"));
    CHECK(getauxval(AT_PAGESZ) == 4096);
    return pt_failures() ? 1 : 0;
}

int pt_child_handover(void)
{
    char c;
    CHECK(read(0, &c, 1) == 0);   /* the null device */
    CHECK(write(1, "hello through fd 1", 18) == 18);
    errno = 0;
    CHECK(write(2, "x", 1) == -1 && errno == EBADF);
    CHECK(write(3, "x", 1) == -1 && errno == EBADF);
    return pt_failures() ? 1 : 0;
}

/* Start this program with argv; its exit code, or -1. */
static int run_child(const char *const *argv, int argc, const struct posixfds_out *ho)
{
    struct spawn_args a = {
        .path = "bin/posixtest", .argc = argc, .argv = argv,
        .job = startup_handle(SR_JOB), .envp = child_env, .handover = ho,
    };
    handle_t proc;
    status_t st = spawn(&a, &proc);
    CHECK(st == OK);
    if (st != OK)
        return -1;
    struct process_info info;
    st = spawn_wait(proc, CHILD_WAIT_NS, &info);
    CHECK(st == OK);
    jam_handle_close(proc);
    return st == OK && !info.killed ? (int)info.exit_code : -1;
}

static void check_child_args(void)
{
    char big[LONG_ARG + 1];
    long_arg(big);
    const char *const argv[] = { "posixtest", "--child-start", "", "two words", "x=y", big, NULL };
    CHECK(run_child(argv, 6, NULL) == 0);
}

static void check_child_handover(void)
{
    handle_t r, w;
    CHECK(jam_channel_create(&r, &w) == OK);
    const struct posixfds_desc fds[] = {
        { .fd = 0, .kind = POSIXFDS_DEV, .status = 0, .data = "null", .data_len = 5 },
        { .fd = 1, .kind = POSIXFDS_PIPE, .status = 1, .handles = &w },
    };
    const struct posixfds_out ho = { .state = { .umask = 022 }, .cwd = "/", .fds = fds, .nfds = 2 };
    const char *const argv[] = { "posixtest", "--child-handover", NULL };
    CHECK(run_child(argv, 2, &ho) == 0);
    char buf[64] = { 0 };
    uint32_t got = 0, nh = 0;
    struct channel_read_args a = {
        .h = r, .rd_cap = sizeof(buf) - 1, .rd_bytes = (uint64_t)(uintptr_t)buf,
        .rd_actual = (uint64_t)(uintptr_t)&got, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK(jam_channel_read(&a) == OK && !strcmp(buf, "hello through fd 1"));
    jam_handle_close(r);
}

static void check_aux(char **argv)
{
    CHECK(argv[0] != NULL);
    CHECK(getauxval(AT_PAGESZ) == 4096);
    CHECK(getauxval(AT_SECURE) == 0);
    CHECK(getauxval(AT_UID) == 1000 && getauxval(AT_GID) == 1000);
    CHECK(getauxval(AT_HWCAP) & (1u << 26));   /* SSE2: every x86-64 has it */
    const unsigned char *rnd = (const unsigned char *)getauxval(AT_RANDOM);
    unsigned or = 0;
    for (int i = 0; rnd && i < 16; i++)
        or |= rnd[i];
    CHECK(rnd && or);
    const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
    unsigned long n = getauxval(AT_PHNUM);
    CHECK(ph && n && getauxval(AT_PHENT) == sizeof(*ph));
    bool tls = false, load = false;
    for (unsigned long i = 0; ph && i < n; i++) {
        tls |= ph[i].p_type == PT_TLS;
        load |= ph[i].p_type == PT_LOAD && ph[i].p_offset == 0;   /* the headers' own */
    }
    CHECK(tls && load);
    CHECK(getenv("TZ") && getenv("TZ")[0]);   /* added: init gives none */
}

static void check_time(void)
{
    struct timespec a, b, real;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(clock_gettime(CLOCK_REALTIME, &real) == 0 && real.tv_sec > 0);
    struct timespec nap = { 0, 20 * 1000 * 1000 };
    CHECK(nanosleep(&nap, NULL) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    int64_t ns = (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    CHECK(ns >= 20 * 1000 * 1000);
    struct timespec res;
    CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1);
    CHECK(clock_settime(CLOCK_REALTIME, &real) == -1 && errno == EPERM);
    struct timespec bad = { 0, 1000000000L };
    CHECK(nanosleep(&bad, NULL) == -1 && errno == EINVAL);
    struct timespec cpu;   /* the kernel's CPU times: some, after the sleep's setup */
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0 && (cpu.tv_sec || cpu.tv_nsec));
    CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0 && (cpu.tv_sec || cpu.tv_nsec));
    CHECK(clock() > 0);
    CHECK(gettid() > 0 && gettid() < (1 << 22));   /* the kernel's small id */
    time_t t = time(NULL);
    struct tm tm;
    CHECK(localtime_r(&t, &tm) != NULL);
}

static void check_machine(void)
{
    unsigned char x[64], y[64];
    CHECK(getrandom(x, sizeof(x), 0) == sizeof(x));
    CHECK(getrandom(y, sizeof(y), GRND_NONBLOCK) == sizeof(y));
    CHECK(memcmp(x, y, sizeof(x)) != 0);
    CHECK(getrandom(x, 1, 0x80) == -1 && errno == EINVAL);
    struct utsname u;
    CHECK(uname(&u) == 0 && !strcmp(u.sysname, "JamOS") && !strcmp(u.machine, "x86_64"));
    CHECK(sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    CHECK(sysconf(_SC_PAGESIZE) == 4096);
}

void pt_start(char **argv)
{
    check_aux(argv);
    check_time();
    check_machine();
    check_child_args();
    check_child_handover();
}
