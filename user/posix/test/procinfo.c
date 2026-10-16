/* posixtest's procinfo section (docs/CR-PLAN.md, "PRC notes"): the files of
 * libc's /proc that Chromium's base reads, against Linux's formats
 * (proc(5)) and against what the process really does.
 *
 *   - /proc/self/stat: 52 fields, parsed as base does (the first "(" and the
 *     last ")", then spaces); pid, comm, state, ppid, pgrp and session are
 *     ours; num_threads follows threads made and ended; utime follows CPU
 *     burned; starttime and /proc/stat's btime give a creation time between
 *     the boot and now; vsize and rss follow memory mapped and touched;
 *     startcode and endcode hold our code; the neutral fields are Linux's;
 *   - /proc/self/status: the keys base reads (Name, State, Tgid, Pid, PPid,
 *     TracerPid, Uid, Threads, VmPeak ... VmSwap, the signal masks) and the
 *     sums that must agree (RssAnon + RssFile + RssShmem is VmRSS); statm
 *     beside it;
 *   - /proc/self/maps: base's line format, in order; our code, a private
 *     mapping, a protect that splits one, a guard, MAP_SHARED, the break and
 *     an unmap each show as they should;
 *   - /proc/cpuinfo, /proc/meminfo (against sysconf, sysinfo and real
 *     allocation), /proc/stat;
 *   - the passes of a file: a read from the start makes it afresh, pread at
 *     an offset reads that pass; the file's kind (read only, size 0,
 *     /proc/self/fd/N's link);
 *   - a child's stat and status (live: S; ended and not yet reaped: Z with
 *     its exit code; reaped: gone) and the listing of /proc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pt.h"

#define PG 4096ull
#define TICK_NS 10000000ull   /* USER_HZ: sysconf(_SC_CLK_TCK) is 100 */

extern char **environ;

/* ---- reading and parsing ------------------------------------------------------------ */

/* A whole file as a NUL-terminated string (malloc'd), or NULL. */
static char *slurp(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 1 >= cap)
            buf = realloc(buf, cap *= 2);
        ssize_t n = buf ? read(fd, buf + len, cap - len - 1) : -1;
        if (n < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (!n)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(fd);
    return buf;
}

#define STAT_FIELDS 52   /* proc(5): pid (1) to exit_code (52) */

/* /proc/<pid>/stat as base's ParseProcStats reads it: the vector holds the
 * pid, the name and the fields after it, so field N of proc(5) is at N - 1. */
struct stat_f {
    char *text;
    char *f[64];
    int   n;
};

static bool stat_of(const char *path, struct stat_f *s)
{
    memset(s, 0, sizeof(*s));
    s->text = slurp(path);
    if (!s->text)
        return false;
    char *open_p = strchr(s->text, '('), *close_p = strrchr(s->text, ')');
    if (!open_p || !close_p || close_p < open_p)
        return false;
    s->f[s->n++] = s->text;                  /* the pid: up to the first space */
    *strchr(s->text, ' ') = '\0';
    *close_p = '\0';
    s->f[s->n++] = open_p + 1;               /* the name */
    for (char *t = strtok(close_p + 1, " \n"); t && s->n < 64; t = strtok(NULL, " \n"))
        s->f[s->n++] = t;
    return true;
}

static unsigned long long fnum(const struct stat_f *s, int field)
{
    return field - 1 < s->n ? strtoull(s->f[field - 1], NULL, 10) : ~0ull;
}

static void stat_free(struct stat_f *s)
{
    free(s->text);
}

/* The value of `key:` in status text, as a number (kB where it has them), or -1. */
static long long status_num(const char *text, const char *key)
{
    size_t kl = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == ':')
            return strtoll(l + kl + 1, NULL, 10);
    return -1;
}

/* The text after `key:` and its tab, up to the end of the line (in out). */
static bool status_str(const char *text, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, kl) && l[kl] == ':') {
            const char *v = l + kl + 1;
            while (*v == '\t' || *v == ' ')
                v++;
            size_t n = strcspn(v, "\n");
            snprintf(out, cap, "%.*s", (int)(n < cap - 1 ? n : cap - 1), v);
            return true;
        }
    return false;
}

static unsigned long long uptime_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return ((unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec) /
           TICK_NS;
}

/* Our own stat, as the process now is. */
static bool self_stat(struct stat_f *s)
{
    return stat_of("/proc/self/stat", s);
}

/* ---- /proc/self/stat --------------------------------------------------------------- */

static void *idle_thread(void *arg)
{
    int *go = arg;
    while (!__atomic_load_n(go, __ATOMIC_ACQUIRE))
        usleep(2000);
    return NULL;
}

extern void pt_procinfo(char **argv);   /* a function of ours: stat's code range holds it */

static void check_stat(void)
{
    struct stat_f s;
    CHECK(self_stat(&s));
    CHECK(s.n == STAT_FIELDS);
    CHECK(fnum(&s, 1) == (unsigned long long)getpid());
    CHECK(!strncmp(s.f[1], program_invocation_short_name, 15) && strlen(s.f[1]) <= 15);
    CHECK(!strcmp(s.f[2], "R"));   /* we are running as we read it */
    CHECK(fnum(&s, 4) == (unsigned long long)getppid());
    CHECK(fnum(&s, 5) == (unsigned long long)getpgrp());
    CHECK(fnum(&s, 6) == (unsigned long long)getsid(0));
    CHECK(!strcmp(s.f[6], "0") && !strcmp(s.f[7], "-1"));   /* tty_nr, tpgid: no terminal */
    CHECK(fnum(&s, 9) == 0x400000u);                           /* flags: PF_RANDOMIZE */
    CHECK(fnum(&s, 10) + fnum(&s, 11) + fnum(&s, 12) + fnum(&s, 13) == 0);   /* no fault counts */
    CHECK(fnum(&s, 15) == 0);                                   /* stime: all in utime */
    CHECK(fnum(&s, 18) == 20 && fnum(&s, 19) == 0 && fnum(&s, 21) == 0);   /* priority nice itreal */
    CHECK(fnum(&s, 20) >= 1);                                   /* num_threads */
    CHECK(!strcmp(s.f[24], "18446744073709551615"));            /* rsslim: none */
    CHECK(fnum(&s, 38) == 17);                                  /* exit_signal: SIGCHLD */
    CHECK(fnum(&s, 52) == 0);                                   /* exit_code of a live process */
    /* startstack, kstkesp, kstkeip, wchan, nswap, cnswap: none; the signal sets are numbers. */
    CHECK(fnum(&s, 28) + fnum(&s, 29) + fnum(&s, 30) + fnum(&s, 35) + fnum(&s, 36) + fnum(&s, 37) == 0);
    CHECK(fnum(&s, 31) != ~0ull && fnum(&s, 32) != ~0ull && fnum(&s, 33) != ~0ull &&
          fnum(&s, 34) != ~0ull);
    CHECK(fnum(&s, 39) == 0 && fnum(&s, 40) == 0 && fnum(&s, 41) == 0);   /* cpu, rt_priority, policy */

    /* startcode and endcode hold our code. */
    unsigned long long here = (unsigned long long)(uintptr_t)&pt_procinfo;
    CHECK(fnum(&s, 26) <= here && here < fnum(&s, 27));

    /* vsize is bytes mapped and rss pages present; mapping and touching 8 MiB moves both. */
    unsigned long long v0 = fnum(&s, 23), r0 = fnum(&s, 24);
    CHECK(v0 > 0 && r0 > 0 && v0 >= r0 * PG);
    stat_free(&s);
    size_t len = 8u << 20;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        struct stat_f t;
        CHECK(self_stat(&t));
        CHECK(fnum(&t, 23) + 65536 >= v0 + len);                /* mapped, not touched */
        CHECK(fnum(&t, 24) < r0 + 256);
        stat_free(&t);
        for (size_t i = 0; i < len; i += PG)
            m[i] = 1;
        CHECK(self_stat(&t));
        CHECK(fnum(&t, 24) >= r0 + 1800);                       /* 2048 pages touched */
        stat_free(&t);
        munmap(m, len);
        CHECK(self_stat(&t));
        CHECK(fnum(&t, 23) < v0 + len);
        CHECK(fnum(&t, 24) < r0 + 256);
        stat_free(&t);
    }

    /* CPU time: burn 60 ms of it and utime moves by about six ticks. */
    CHECK(self_stat(&s));
    unsigned long long u0 = fnum(&s, 14);
    stat_free(&s);
    struct timespec c0, c1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
    do {
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
    } while ((c1.tv_sec - c0.tv_sec) * 1000000000ll + (c1.tv_nsec - c0.tv_nsec) < 60000000ll);
    CHECK(self_stat(&s));
    CHECK(fnum(&s, 14) >= u0 + 4 && fnum(&s, 14) <= u0 + 40);
    stat_free(&s);

    /* Threads: three more while they live, back when they have gone. */
    CHECK(self_stat(&s));
    unsigned long long th0 = fnum(&s, 20);
    stat_free(&s);
    int go = 0;
    pthread_t t[3];
    for (int i = 0; i < 3; i++)
        CHECK(pthread_create(&t[i], NULL, idle_thread, &go) == 0);
    CHECK(self_stat(&s));
    CHECK(fnum(&s, 20) == th0 + 3);
    stat_free(&s);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    unsigned long long th1 = 0;
    for (int tries = 0; tries < 200; tries++) {
        CHECK(self_stat(&s));
        th1 = fnum(&s, 20);
        stat_free(&s);
        if (th1 == th0)
            break;
        usleep(10000);
    }
    CHECK(th1 == th0);
}

/* The creation time base computes: btime (from /proc/stat) plus starttime in
 * ticks lies between the boot and now, and agrees with the uptime. */
static void check_start_time(void)
{
    char *sys = slurp("/proc/stat");
    CHECK(sys != NULL);
    long long btime = -1;
    if (sys) {
        const char *b = strstr(sys, "\nbtime ");
        CHECK(b != NULL);
        if (b)
            btime = strtoll(b + 7, NULL, 10);
    }
    struct stat_f s;
    CHECK(self_stat(&s));
    unsigned long long start = fnum(&s, 22);
    stat_free(&s);
    CHECK(start > 0 && start <= uptime_ticks());
    struct timespec now, up;
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_BOOTTIME, &up);
    long long created = btime + (long long)(start / 100);
    CHECK(created <= now.tv_sec);
    CHECK(created >= now.tv_sec - up.tv_sec - 2);   /* the boot, give or take a second */
    CHECK(btime >= now.tv_sec - up.tv_sec - 2 && btime <= now.tv_sec - up.tv_sec + 1);
    free(sys);
}

/* ---- /proc/self/status and statm ----------------------------------------------------- */

static void noop_handler(int sig)
{
    (void)sig;
}

static void check_status(void)
{
    char *t = slurp("/proc/self/status");
    CHECK(t != NULL);
    if (!t)
        return;
    char buf[128], name[64];
    struct stat_f s;
    CHECK(self_stat(&s));
    CHECK(status_str(t, "Name", buf, sizeof(buf)) && !strcmp(buf, s.f[1]));
    CHECK(status_str(t, "State", buf, sizeof(buf)) && !strncmp(buf, "R (running)", 11));
    CHECK(status_num(t, "Tgid") == getpid() && status_num(t, "Pid") == getpid());
    CHECK(status_num(t, "PPid") == getppid());
    CHECK(status_num(t, "TracerPid") == 0);   /* base's BeingDebugged reads this */
    CHECK(status_str(t, "Uid", buf, sizeof(buf)));
    snprintf(name, sizeof(name), "%u\t%u\t%u\t%u", getuid(), geteuid(), getuid(), geteuid());
    CHECK(!strcmp(buf, name));
    CHECK(status_num(t, "Threads") == (long long)fnum(&s, 20));
    CHECK(status_num(t, "Seccomp") == 0 && status_num(t, "NoNewPrivs") == 0);
    stat_free(&s);

    long long size = status_num(t, "VmSize"), peak = status_num(t, "VmPeak");
    long long rss = status_num(t, "VmRSS"), hwm = status_num(t, "VmHWM");
    CHECK(size > 0 && rss > 0 && size >= rss);
    CHECK(peak >= size && hwm >= rss);
    CHECK(status_num(t, "RssAnon") + status_num(t, "RssFile") + status_num(t, "RssShmem") == rss);
    CHECK(status_num(t, "RssFile") > 0);      /* the code we run from */
    CHECK(status_num(t, "VmData") > 0 && status_num(t, "VmExe") > 0);
    CHECK(status_num(t, "VmSwap") == 0 && status_num(t, "VmLck") == 0);
    CHECK(status_num(t, "VmPTE") >= 0 && status_num(t, "VmStk") >= 0 && status_num(t, "VmLib") >= 0);
    CHECK(status_str(t, "Cpus_allowed_list", buf, sizeof(buf)) && buf[0] == '0');
    CHECK(status_str(t, "Cpus_allowed", buf, sizeof(buf)) && buf[0]);
    free(t);

    /* Peaks never fall: map 8 MiB, touch it, give it back. */
    size_t len = 8u << 20;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        for (size_t i = 0; i < len; i += PG)
            m[i] = 1;
        t = slurp("/proc/self/status");
        long long big_size = t ? status_num(t, "VmSize") : -1, big_rss = t ? status_num(t, "VmRSS") : -1;
        free(t);
        CHECK(big_size + 64 >= size + 8192 && big_rss >= rss + 7000);
        munmap(m, len);
        t = slurp("/proc/self/status");
        CHECK(t && status_num(t, "VmSize") < big_size && status_num(t, "VmRSS") < big_rss);
        CHECK(t && status_num(t, "VmPeak") >= big_size && status_num(t, "VmHWM") >= big_rss);
        free(t);
    }

    /* The signal masks are ours: blocked, ignored and caught. */
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &one, &old);
    struct sigaction ign = { .sa_handler = SIG_IGN }, cgt = { .sa_handler = noop_handler };
    struct sigaction ign_old, cgt_old;
    sigaction(SIGUSR2, &ign, &ign_old);
    sigaction(SIGWINCH, &cgt, &cgt_old);
    t = slurp("/proc/self/status");
    CHECK(t != NULL);
    if (t) {
        char mask[40];
        unsigned long long m1;
        CHECK(status_str(t, "SigBlk", mask, sizeof(mask)));
        m1 = strtoull(mask, NULL, 16);
        CHECK(m1 & (1ull << (SIGUSR1 - 1)));
        CHECK(status_str(t, "SigIgn", mask, sizeof(mask)));
        m1 = strtoull(mask, NULL, 16);
        CHECK(m1 & (1ull << (SIGUSR2 - 1)));
        CHECK(status_str(t, "SigCgt", mask, sizeof(mask)));
        m1 = strtoull(mask, NULL, 16);
        CHECK(m1 & (1ull << (SIGWINCH - 1)));
        CHECK(!(m1 & (1ull << (SIGUSR2 - 1))));
        free(t);
    }
    sigaction(SIGWINCH, &cgt_old, NULL);
    sigaction(SIGUSR2, &ign_old, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void check_statm(void)
{
    char *m = slurp("/proc/self/statm"), *st = slurp("/proc/self/status");
    CHECK(m && st);
    if (m && st) {
        unsigned long long size, res, shared, text, lib, data, dt;
        CHECK(sscanf(m, "%llu %llu %llu %llu %llu %llu %llu", &size, &res, &shared, &text, &lib,
                     &data, &dt) == 7);
        CHECK(size > 0 && res > 0 && res <= size && lib == 0 && dt == 0);
        CHECK(shared <= res && text > 0);
        long long kb = status_num(st, "VmSize");
        CHECK(size * 4 + 128 >= (unsigned long long)kb && size * 4 <= (unsigned long long)kb + 128);
        kb = status_num(st, "VmRSS");
        CHECK(res * 4 + 512 >= (unsigned long long)kb && res * 4 <= (unsigned long long)kb + 512);
    }
    free(m);
    free(st);
}

/* ---- /proc/self/maps ---------------------------------------------------------------- */

struct line {
    uintptr_t start, end;
    char      perms[5];
    unsigned long long off;
    unsigned  dev_major, dev_minor;
    long long inode;
    char      name[256];
};

/* base's ParseProcMaps line: "%x-%x %4s %llx %hhx:%hhx %lld %n", then the name. */
static int parse_maps(const char *text, struct line *out, int cap)
{
    int n = 0;
    for (const char *l = text; *l && n < cap;) {
        struct line *o = &out[n];
        int used = 0;
        unsigned char maj = 0, min = 0;
        memset(o, 0, sizeof(*o));
        int got = sscanf(l, "%lx-%lx %4s %llx %hhx:%hhx %lld %n", &o->start, &o->end, o->perms,
                         &o->off, &maj, &min, &o->inode, &used);
        if (got < 7)
            return -1;
        o->dev_major = maj;
        o->dev_minor = min;
        const char *e = strchr(l, '\n');
        if (!e)
            return -1;
        if (l + used < e)
            snprintf(o->name, sizeof(o->name), "%.*s", (int)(e - l - used), l + used);
        n++;
        l = e + 1;
    }
    return n;
}

static const struct line *line_at(const struct line *ls, int n, uintptr_t addr)
{
    for (int i = 0; i < n; i++)
        if (ls[i].start <= addr && addr < ls[i].end)
            return &ls[i];
    return NULL;
}

/* Is there a line for exactly [start, end)? */
static bool has_exact(const struct line *ls, int n, uintptr_t start, uintptr_t end)
{
    for (int i = 0; i < n; i++)
        if (ls[i].start == start && ls[i].end == end)
            return true;
    return false;
}

static int read_maps(struct line *ls, int cap)
{
    char *t = slurp("/proc/self/maps");
    int n = t ? parse_maps(t, ls, cap) : -1;
    free(t);
    return n;
}

#define MAXL 2048

static void check_maps(void)
{
    static struct line ls[MAXL];
    int n = read_maps(ls, MAXL);
    CHECK(n > 3);
    if (n <= 0)
        return;
    for (int i = 0; i < n; i++) {
        CHECK(ls[i].start < ls[i].end && !(ls[i].start & (PG - 1)) && !(ls[i].end & (PG - 1)));
        CHECK(i == 0 || ls[i].start >= ls[i - 1].end);
        CHECK(strlen(ls[i].perms) == 4 && (ls[i].perms[3] == 'p' || ls[i].perms[3] == 's'));
        CHECK(ls[i].dev_major == 0 && ls[i].dev_minor == 0 && ls[i].inode == 0);
    }

    /* Our code: r-xp, named by the file /proc/self/exe names; our stack: read-write. */
    char exe[256];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    CHECK(el > 0);
    exe[el > 0 ? el : 0] = '\0';
    const struct line *code = line_at(ls, n, (uintptr_t)&pt_procinfo);
    CHECK(code && !strcmp(code->perms, "r-xp") && !strcmp(code->name, exe));
    int local = 0;
    const struct line *stk = line_at(ls, n, (uintptr_t)&local);
    CHECK(stk && stk->perms[0] == 'r' && stk->perms[1] == 'w' && stk->perms[2] == '-');
    static int data_word;
    const struct line *data = line_at(ls, n, (uintptr_t)&data_word);
    CHECK(data && data->perms[0] == 'r' && data->perms[1] == 'w');

    /* A private mapping of ours: one line, exactly its size, rw-p, no name, offset 0. */
    size_t len = 16 * PG;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m == MAP_FAILED)
        return;
    n = read_maps(ls, MAXL);
    const struct line *l = line_at(ls, n, (uintptr_t)m);
    CHECK(l && l->start == (uintptr_t)m && l->end == (uintptr_t)m + len);
    CHECK(l && !strcmp(l->perms, "rw-p") && !l->name[0] && l->off == 0);

    /* A protect in the middle splits it three ways; no access shows as ---p. */
    CHECK(mprotect(m + 4 * PG, 4 * PG, PROT_READ) == 0);
    CHECK(mprotect(m + 12 * PG, 2 * PG, PROT_NONE) == 0);
    n = read_maps(ls, MAXL);
    const struct line *a = line_at(ls, n, (uintptr_t)m), *b = line_at(ls, n, (uintptr_t)m + 4 * PG);
    const struct line *c = line_at(ls, n, (uintptr_t)m + 8 * PG), *g = line_at(ls, n, (uintptr_t)m + 12 * PG);
    CHECK(a && !strcmp(a->perms, "rw-p") && a->end == (uintptr_t)m + 4 * PG);
    CHECK(b && !strcmp(b->perms, "r--p") && b->start == (uintptr_t)m + 4 * PG &&
          b->end == (uintptr_t)m + 8 * PG);
    CHECK(c && !strcmp(c->perms, "rw-p") && c->start == (uintptr_t)m + 8 * PG &&
          c->end == (uintptr_t)m + 12 * PG);
    CHECK(g && !strcmp(g->perms, "---p") && g->start == (uintptr_t)m + 12 * PG &&
          g->end == (uintptr_t)m + 14 * PG);
    CHECK(munmap(m + 2 * PG, 2 * PG) == 0);   /* a hole in the first piece */
    n = read_maps(ls, MAXL);
    const struct line *x = line_at(ls, n, (uintptr_t)m + PG);
    CHECK(x && x->end == (uintptr_t)m + 2 * PG);   /* the piece now ends at the hole */
    CHECK(!has_exact(ls, n, (uintptr_t)m, (uintptr_t)m + 4 * PG));
    munmap(m, len);
    n = read_maps(ls, MAXL);
    static const unsigned pieces[][2] = { { 0, 2 }, { 4, 8 }, { 8, 12 }, { 12, 14 }, { 14, 16 } };
    for (unsigned i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++)
        CHECK(!has_exact(ls, n, (uintptr_t)m + pieces[i][0] * PG, (uintptr_t)m + pieces[i][1] * PG));

    /* MAP_SHARED shows s. */
    int fd = memfd_create("ptmaps", 0);
    CHECK(fd >= 0 && ftruncate(fd, 2 * PG) == 0);
    char *sh = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(sh != MAP_FAILED);
    if (sh != MAP_FAILED) {
        sh[0] = 1;
        n = read_maps(ls, MAXL);
        l = line_at(ls, n, (uintptr_t)sh);
        CHECK(l && !strcmp(l->perms, "rw-s") && l->end - l->start == 2 * PG);
        munmap(sh, 2 * PG);
    }
    close(fd);

    /* The break: a line named [heap] holds it. */
    long brk0 = syscall(SYS_brk, 0);
    n = read_maps(ls, MAXL);
    l = brk0 ? line_at(ls, n, (uintptr_t)brk0) : NULL;
    CHECK(brk0 == 0 || (l && !strcmp(l->name, "[heap]")));
}

/* A file is read in passes: from the start it is made afresh; at an offset
 * after that it is the pass's. */
static void check_passes(void)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    static char a[1 << 18], b[1 << 18];
    ssize_t na = pread(fd, a, sizeof(a) - 1, 0);
    CHECK(na > 0);
    /* A new mapping between two passes shows in the second. */
    char *m = mmap(NULL, 37 * PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    char want[48];
    snprintf(want, sizeof(want), "%08lx-%08lx r--p", (unsigned long)(uintptr_t)m,
             (unsigned long)((uintptr_t)m + 37 * PG));
    a[na > 0 ? na : 0] = '\0';
    CHECK(!strstr(a, want));
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    ssize_t nb = read(fd, b, sizeof(b) - 1);
    CHECK(nb > 0);
    b[nb > 0 ? nb : 0] = '\0';
    CHECK(strstr(b, want));
    /* Reads after the first of a pass are one text: the second half is the first's tail. */
    char half[64];
    ssize_t nh = pread(fd, half, sizeof(half), nb > 100 ? nb - 50 : 0);
    CHECK(nh > 0 && !memcmp(half, b + (nb - 50), (size_t)(nh < 50 ? nh : 50)));
    CHECK(pread(fd, half, 8, nb) == 0);   /* the end */
    CHECK(lseek(fd, 0, SEEK_END) > 0);
    /* The link of the descriptor names the file. */
    char link[64], path[64], expect[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t ln = readlink(path, link, sizeof(link) - 1);
    snprintf(expect, sizeof(expect), "/proc/%d/maps", (int)getpid());
    CHECK(ln == (ssize_t)strlen(expect) && !memcmp(link, expect, (size_t)ln));
    munmap(m, 37 * PG);
    close(fd);
}

/* ---- the files as files ------------------------------------------------------------- */

static void check_kinds(void)
{
    static const char *const files[] = {
        "/proc/self/stat", "/proc/self/statm", "/proc/self/status", "/proc/self/maps",
        "/proc/cpuinfo", "/proc/meminfo", "/proc/stat",
    };
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/stat", (int)getpid());
    struct stat st;
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const char *f = files[i];
        CHECK(stat(f, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0444 &&
              st.st_size == 0 && st.st_nlink == 1);
        struct stat ls;
        CHECK(lstat(f, &ls) == 0 && S_ISREG(ls.st_mode) && ls.st_ino == st.st_ino);
        CHECK(access(f, R_OK) == 0 && access(f, F_OK) == 0);
        errno = 0;
        CHECK(access(f, W_OK) == -1 && errno == EACCES);
        errno = 0;
        CHECK(open(f, O_WRONLY) == -1 && errno == EACCES);
        errno = 0;
        CHECK(open(f, O_RDWR) == -1 && errno == EACCES);
        errno = 0;
        CHECK(open(f, O_RDONLY | O_TRUNC) == -1 && errno == EACCES);
        errno = 0;
        CHECK(open(f, O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
        errno = 0;
        CHECK(opendir(f) == NULL && errno == ENOTDIR);
        char buf[8];
        errno = 0;
        CHECK(readlink(f, buf, sizeof(buf)) == -1 && errno == EINVAL);
        char sub[96];
        snprintf(sub, sizeof(sub), "%s/x", f);
        errno = 0;
        CHECK(stat(sub, &st) == -1 && errno == ENOTDIR);
        int fd = open(f, O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0);
        errno = 0;
        CHECK(fd >= 0 && write(fd, "x", 1) == -1 && errno == EBADF);
        char big[64];
        CHECK(fd >= 0 && read(fd, big, sizeof(big)) > 0);
        if (fd >= 0)
            close(fd);
    }
    /* Nothing is made under /proc. */
    errno = 0;
    CHECK(open("/proc/cpuinfo2", O_CREAT | O_WRONLY, 0600) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(unlink("/proc/cpuinfo") == -1 && errno == EACCES);
    /* The listing: /proc has the files, the process directory has its own. */
    bool seen[3] = { false, false, false };
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        seen[0] |= !strcmp(e->d_name, "cpuinfo") && e->d_type == DT_REG;
        seen[1] |= !strcmp(e->d_name, "meminfo") && e->d_type == DT_REG;
        seen[2] |= !strcmp(e->d_name, "stat") && e->d_type == DT_REG;
    }
    if (d)
        closedir(d);
    CHECK(seen[0] && seen[1] && seen[2]);
    int found = 0;
    d = opendir("/proc/self");
    while (d && (e = readdir(d)))
        found += !strcmp(e->d_name, "maps") + !strcmp(e->d_name, "stat") +
                 !strcmp(e->d_name, "statm") + !strcmp(e->d_name, "status") +
                 !strcmp(e->d_name, "exe") + !strcmp(e->d_name, "fd");
    if (d)
        closedir(d);
    CHECK(found == 6);
}

/* What Linux has that we don't is still ENOENT: user namespaces (base's
 * check of /proc/self/ns/user then says there are none), the rest of the
 * process's files. */
static void check_gone(void)
{
    static const char *const gone[] = {
        "/proc/self/ns/user", "/proc/self/ns", "/proc/uptime", "/proc/loadavg", "/proc/version",
        "/proc/self/smaps", "/proc/self/cmdline", "/proc/self/environ", "/proc/self/limits",
        "/proc/self/cwd", "/proc/self/task", "/proc/self/oom_score_adj", "/proc/diskstats",
        "/proc/sys/kernel/osrelease", "/proc/self/auxv",
    };
    struct stat st;
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        errno = 0;
        CHECK(open(gone[i], O_RDONLY) == -1 && errno == ENOENT);
        errno = 0;
        CHECK(stat(gone[i], &st) == -1 && errno == ENOENT);
    }
}

/* ---- /proc/cpuinfo, /proc/meminfo, /proc/stat ---------------------------------------- */

static void check_cpuinfo(void)
{
    char *t = slurp("/proc/cpuinfo");
    CHECK(t != NULL);
    if (!t)
        return;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int blocks = 0;
    for (const char *p = t; (p = strstr(p, "processor\t: ")); p++)
        blocks++;
    CHECK(cpus >= 1 && blocks == cpus);
    CHECK(strncmp(t, "processor\t: 0\n", 14) == 0);
    /* base's CPUModelName: the first "model name" line. */
    const char *mn = strstr(t, "\nmodel name\t: ");
    CHECK(mn != NULL);
    if (mn)
        CHECK(mn[14] != '\n' && mn[14] != '\0');
    uint32_t a, bx, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(bx), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    char vendor[13];
    memcpy(vendor, &bx, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    char want[40];
    snprintf(want, sizeof(want), "\nvendor_id\t: %s\n", vendor);
    CHECK(strstr(t, want) != NULL);
    __asm__ volatile("cpuid" : "=a"(a), "=b"(bx), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    unsigned family = (a >> 8) & 0xf, step = a & 0xf;
    if (family == 0xf)
        family += (a >> 20) & 0xff;
    snprintf(want, sizeof(want), "\ncpu family\t: %u\n", family);
    CHECK(strstr(t, want) != NULL);
    snprintf(want, sizeof(want), "\nstepping\t: %u\n", step);
    CHECK(strstr(t, want) != NULL);
    const char *fl = strstr(t, "\nflags\t\t: ");
    CHECK(fl != NULL);
    if (fl) {
        size_t n = strcspn(fl + 1, "\n");
        const char *list = fl + 10;   /* after "flags\t\t: " */
        size_t cl = strcspn(list, "\n");
        char *flags = malloc(cl + 3);
        (void)n;
        if (flags) {
            flags[0] = ' ';
            memcpy(flags + 1, list, cl);
            flags[cl + 1] = ' ';
            flags[cl + 2] = '\0';
            CHECK(strstr(flags, " fpu ") && strstr(flags, " sse2 ") && strstr(flags, " lm "));
            CHECK(((d >> 26) & 1) == (strstr(flags, " sse2 ") != NULL));   /* leaf 1's bits */
            CHECK(((c >> 28) & 1) == (strstr(flags, " avx ") != NULL));
            free(flags);
        }
    }
    const char *mhz = strstr(t, "\ncpu MHz\t\t: ");
    CHECK(mhz && atof(mhz + 13) > 100.0);   /* the TSC's rate */
    const char *bogo = strstr(t, "\nbogomips\t: ");
    CHECK(bogo && atof(bogo + 12) > 200.0);
    CHECK(strstr(t, "\ncache size\t: ") != NULL);
    CHECK(strstr(t, "\naddress sizes\t: ") != NULL);
    CHECK(strstr(t, "\nclflush size\t: 64\n") != NULL);
    free(t);
}

static void check_meminfo(void)
{
    char *t = slurp("/proc/meminfo");
    CHECK(t != NULL);
    if (!t)
        return;
    CHECK(!strncmp(t, "MemTotal:", 9));
    /* base's ParseProcMeminfo: every line has "Key:", a number and maybe "kB". */
    int lines = 0;
    for (const char *l = t; *l; l = strchr(l, '\n') + 1) {
        char key[40], unit[8] = "", one[96];
        unsigned long long v;
        snprintf(one, sizeof(one), "%.*s", (int)strcspn(l, "\n"), l);
        int got = sscanf(one, "%39s %llu %7s", key, &v, unit);
        CHECK((got == 3 && !strcmp(unit, "kB")) || (got == 2 && !strncmp(key, "HugePages_", 10)));
        CHECK(key[strlen(key) - 1] == ':');
        lines++;
    }
    CHECK(lines >= 40);
    long long total = status_num(t, "MemTotal"), free_kb = status_num(t, "MemFree");
    long long avail = status_num(t, "MemAvailable");
    CHECK(total == sysconf(_SC_PHYS_PAGES) * (sysconf(_SC_PAGESIZE) / 1024));
    CHECK(free_kb > 0 && free_kb <= total && avail >= free_kb && avail <= total);
    CHECK(status_num(t, "SwapTotal") == 0 && status_num(t, "Hugepagesize") == 2048);
    free(t);

    /* sysinfo agrees (within what moves meanwhile), and real allocation shows in MemFree. */
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0 && si.freeram > 0 && si.freeram <= si.totalram);
    CHECK((long long)(si.totalram / 1024) == total);
    size_t len = 16u << 20;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    if (m != MAP_FAILED) {
        for (size_t i = 0; i < len; i += PG)
            m[i] = 1;
        t = slurp("/proc/meminfo");
        long long during = t ? status_num(t, "MemFree") : -1;
        free(t);
        CHECK(during >= 0 && during + 8192 <= free_kb);   /* at least half of it */
        munmap(m, len);
        t = slurp("/proc/meminfo");
        CHECK(t && status_num(t, "MemFree") >= during + 8192);
        free(t);
    }
}

static void check_sysstat(void)
{
    char *t = slurp("/proc/stat");
    CHECK(t != NULL);
    if (!t)
        return;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long long v[10];
    CHECK(!strncmp(t, "cpu  ", 5) &&
          sscanf(t, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                 &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) == 10);
    int per = 0;
    for (const char *p = t; (p = strstr(p, "\ncpu")); p++)
        per += p[4] >= '0' && p[4] <= '9';
    CHECK(per == cpus);
    CHECK(strstr(t, "\nintr ") && strstr(t, "\nctxt ") && strstr(t, "\nbtime ") &&
          strstr(t, "\nprocesses ") && strstr(t, "\nprocs_running ") &&
          strstr(t, "\nprocs_blocked "));
    free(t);
}

/* ---- children ---------------------------------------------------------------------- */

/* posixtest --child-procfs block: say so on descriptor 1, then wait for the
 * pipe on descriptor 0 to close and exit 5. We wait for its word, so it has
 * run (it has pages present and a thread) when we look. */
static pid_t spawn_blocker(int *wr)
{
    int p[2], q[2];
    CHECK(pipe(p) == 0 && pipe(q) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[0], 0);
    posix_spawn_file_actions_adddup2(&fa, q[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    posix_spawn_file_actions_addclose(&fa, q[0]);
    char *argv[] = { "posixtest", "--child-procfs", "block", NULL };
    pid_t pid = -1;
    int e = posix_spawn(&pid, "/bin/posixtest", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    CHECK(e == 0);
    close(p[0]);
    close(q[1]);
    char up = 0;
    CHECK(e != 0 || read(q[0], &up, 1) == 1);   /* it has started */
    close(q[0]);
    *wr = p[1];
    return e == 0 ? pid : -1;
}

static bool listed_in_proc(pid_t pid)
{
    char name[16];
    snprintf(name, sizeof(name), "%d", (int)pid);
    bool seen = false;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)))
        seen |= !strcmp(e->d_name, name) && e->d_type == DT_DIR;
    if (d)
        closedir(d);
    return seen;
}

static void check_child(bool kill_it)
{
    unsigned long long t0 = uptime_ticks();
    int wr = -1;
    pid_t pid = spawn_blocker(&wr);
    CHECK(pid > 0);
    if (pid <= 0)
        return;
    unsigned long long t1 = uptime_ticks();
    char path[48];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    struct stat_f s;
    CHECK(stat_of(path, &s));
    CHECK(s.n == STAT_FIELDS && fnum(&s, 1) == (unsigned long long)pid);
    CHECK(s.f[1][0] && strlen(s.f[1]) <= 15);
    CHECK(!strcmp(s.f[2], "S"));   /* alive: not running as far as we can tell, not a zombie */
    CHECK(fnum(&s, 4) == (unsigned long long)getpid());           /* we are its parent */
    CHECK(fnum(&s, 5) == (unsigned long long)getpgid(pid));
    CHECK(fnum(&s, 20) >= 1);
    CHECK(fnum(&s, 22) + 1 >= t0 && fnum(&s, 22) <= t1 + 1);        /* started between the two */
    CHECK(fnum(&s, 23) > 0 && fnum(&s, 24) > 0);                    /* mapped and present */
    char name[32];
    snprintf(name, sizeof(name), "%s", s.f[1]);
    stat_free(&s);
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    char *t = slurp(path), buf[64];
    CHECK(t && status_str(t, "Name", buf, sizeof(buf)) && !strcmp(buf, name));
    CHECK(t && status_num(t, "PPid") == getpid() && status_num(t, "Pid") == pid);
    CHECK(t && status_str(t, "State", buf, sizeof(buf)) && buf[0] == 'S');
    CHECK(t && status_num(t, "VmRSS") > 0 && status_num(t, "Threads") >= 1);
    free(t);
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    t = slurp(path);
    static struct line ls[MAXL];
    int n = t ? parse_maps(t, ls, MAXL) : -1;
    free(t);
    bool code = false;
    for (int i = 0; i < n; i++)
        code |= !strcmp(ls[i].perms, "r-xp");
    CHECK(n > 3 && code);
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    t = slurp(path);
    CHECK(t && strlen(t) > 6);
    free(t);
    /* Its directory has these four and not our own exe and fd; /proc lists it. */
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    int found = 0;
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)))
        found += !strcmp(e->d_name, "maps") + !strcmp(e->d_name, "stat") +
                 !strcmp(e->d_name, "statm") + !strcmp(e->d_name, "status") +
                 100 * (!strcmp(e->d_name, "exe") + !strcmp(e->d_name, "fd"));
    if (d)
        closedir(d);
    CHECK(found == 4);
    CHECK(listed_in_proc(pid));
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    errno = 0;
    CHECK(lstat(path, &st) == -1 && errno == ENOENT);
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    errno = 0;
    CHECK(stat(path, &st) == -1 && errno == ENOENT);

    /* It ends, and until we wait it is a zombie with its code. */
    int status = 0;
    if (kill_it)
        CHECK(kill(pid, SIGKILL) == 0);
    else
        close(wr);
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    CHECK(waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == pid);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    CHECK(stat_of(path, &s));
    CHECK(s.n == STAT_FIELDS && !strcmp(s.f[2], "Z"));
    CHECK(fnum(&s, 52) == (kill_it ? 9u : 5u << 8));
    stat_free(&s);
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    t = slurp(path);
    CHECK(t && status_str(t, "State", buf, sizeof(buf)) && buf[0] == 'Z');
    free(t);
    CHECK(listed_in_proc(pid));
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(kill_it ? (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                  : (WIFEXITED(status) && WEXITSTATUS(status) == 5));
    if (kill_it)
        close(wr);
    /* Reaped: gone, from the listing and for every call. */
    errno = 0;
    CHECK(open(path, O_RDONLY) == -1 && errno == ENOENT);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    errno = 0;
    CHECK(stat(path, &st) == -1 && errno == ENOENT);
    CHECK(!listed_in_proc(pid));
}

/* A file opened while the child lived reads ESRCH once it is reaped, as Linux. */
static void check_child_reaped_file(void)
{
    int wr = -1;
    pid_t pid = spawn_blocker(&wr);
    CHECK(pid > 0);
    if (pid <= 0)
        return;
    char path[48], buf[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    close(wr);
    CHECK(waitpid(pid, NULL, 0) == pid);
    errno = 0;
    CHECK(fd >= 0 && pread(fd, buf, sizeof(buf), 0) == -1 && errno == ESRCH);
    if (fd >= 0)
        close(fd);
}

/* Only we and our children are here. */
static void check_others(void)
{
    char path[32];
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)getppid());
    errno = 0;
    CHECK(stat(path, &st) == -1 && errno == ENOENT);   /* our parent's */
    errno = 0;
    CHECK(stat("/proc/0/stat", &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(stat("/proc/00/stat", &st) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(stat("/proc/-1/stat", &st) == -1 && errno == ENOENT);
}

int pt_child_block(void)
{
    char c = 'u';
    if (write(1, &c, 1) != 1)
        return 3;
    while (read(0, &c, 1) > 0)
        ;
    return 5;
}

void pt_procinfo(char **argv __attribute__((unused)))
{
    check_stat();
    check_start_time();
    check_status();
    check_statm();
    check_maps();
    check_passes();
    check_kinds();
    check_gone();
    check_cpuinfo();
    check_meminfo();
    check_sysstat();
    check_child(false);
    check_child(true);
    check_child_reaped_file();
    check_others();
}
