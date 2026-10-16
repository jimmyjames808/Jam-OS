/* The text of /proc's generated files (docs/CR-PLAN.md "PRC notes"), in
 * Linux's formats and field order (proc(5)), made when a file is read:
 *
 *   /proc/<pid>/stat, statm, status, maps   for our own pid and for each
 *                         child of ours not yet reaped
 *   /proc/meminfo, /proc/stat   the system's
 *   /proc/cpuinfo         cpuinfo.c
 *
 * Where the figures come from. A process: process_get_info (state, live
 * threads, CPU time, pid, creation time, name), our own process group,
 * session and parent from the hand-over (proc.c), a child's from the table
 * of children; its address space: process_get_maps, which lists the
 * mappings and the pages present in each (a child's through the handle we
 * hold of it). libc's own table of mappings (mem.c) says which of our
 * mappings are the break and which are shared or on a file's own pages;
 * the program's ELF headers say which are its image (whose name is the
 * path /proc/self/exe gives). The system: sys_public_info (memory, free
 * memory), the wall clock and the uptime (btime).
 *
 * Neutral values, where Jam OS has nothing to say (Linux's "none" for each):
 * stat: tty 0 and tpgid -1, flags PF_RANDOMIZE, all fault counts 0, stime 0
 * (the kernel keeps one CPU time: utime has all of it), cutime the CPU time
 * of the children we reaped, cstime 0, priority 20, nice 0, itrealvalue 0,
 * rsslim unlimited, startstack 0, the stack pointers 0, signal masks (ours
 * only; a child's are 0), wchan 0, exit_signal 17, processor 0, rt_priority
 * 0, policy 0, the data, brk, argument and environment addresses 0, and the
 * exit code of a zombie as wait would give it. A child's stat has no
 * startcode or endcode, and its comm is the name it was created with.
 * status: no tracer, uid and gid 1000, no groups, no capabilities, no
 * seccomp, VmLck, VmPin, VmLib, VmStk (the main stack isn't told apart from
 * other mappings: it is in VmData) and VmSwap 0, VmPTE 0, VmPeak and VmHWM
 * the largest VmSize and VmRSS this process has seen read here (a child's:
 * its current ones), all CPUs allowed, no context switches counted.
 * maps: device 00:00 and inode 0 on every line; the name of an image's
 * lines (ours only) and [heap]; no [stack], [vdso] or names for files
 * mapped with mmap; the offset only where the VMO is a file (an image, a
 * file's own pages). Anonymous memory and shared memory are not told apart
 * by name (all are blank).
 * meminfo: MemTotal and MemFree are the allocator's (free pages exclude
 * the kernel heap's cached objects); MemAvailable is MemFree (no page cache
 * is kept apart: files are the pager's pages, counted in no field); the
 * other fields 0 (swap, buffers, caches, huge pages), Hugepagesize 2048.
 * stat (system): the CPU times 0 (cpu_stat is root's), btime real, processes
 * our pid, procs_running 1, the rest 0. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "calls.h"
#include "files.h"
#include "proc.h"
#include "procinfo.h"
#include "sig.h"

#define PAGE      4096ull
#define TICK_NS   10000000ull   /* USER_HZ is 100: sysconf(_SC_CLK_TCK) */
#define ONE_USER  1000
#define COMM_MAX  15            /* a Linux comm: 15 bytes and a NUL */
#define PH_MAX    16u
#define NAME_COL  73            /* where maps' names start (Linux pads to here) */
#define PF_RANDOMIZE 0x400000u  /* stat's flags for an ordinary process */

extern const Elf64_Ehdr __ehdr_start __attribute__((visibility("hidden")));

/* ---- a growing text ---------------------------------------------------------------- */

void sb_put(struct sb *b, const char *fmt, ...)
{
    va_list ap;
    while (!b->bad) {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->bad = true;
        } else if ((size_t)n < room) {
            b->len += (size_t)n;
            return;
        } else {
            size_t cap = b->cap ? b->cap * 2 : 4096;
            while (cap - b->len <= (size_t)n)
                cap *= 2;
            char *q = realloc(b->p, cap);
            if (!q)
                b->bad = true;
            else {
                b->p = q;
                b->cap = cap;
            }
        }
    }
}

/* ---- what the mappings add up to ---------------------------------------------------- */

/* One walk of a process's mappings: the sums, and (maps) the lines. */
struct acc {
    uint64_t size;                           /* VmSize: bytes mapped */
    uint64_t rss, rss_anon, rss_file, rss_shmem;   /* pages present */
    uint64_t exe, data;                      /* VmExe, VmData: bytes */
    uint64_t text_lo, text_hi;               /* the image's executable mappings */
    bool     self;                           /* ours: libc's table and our image apply */
    const char *image_name;                  /* our image's path, or NULL */
    struct sb *lines;                        /* maps: where its lines go, or NULL */
    uint64_t lo[PH_MAX], hi[PH_MAX];         /* our image's segments, page-rounded */
    unsigned nseg;
};

static void image_segments(struct acc *a)
{
    const Elf64_Ehdr *eh = &__ehdr_start;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const uint8_t *)eh + eh->e_phoff);
    for (unsigned i = 0; i < eh->e_phnum && a->nseg < PH_MAX; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            a->lo[a->nseg] = ph[i].p_vaddr & ~(PAGE - 1);
            a->hi[a->nseg] = (ph[i].p_vaddr + ph[i].p_memsz + PAGE - 1) & ~(PAGE - 1);
            a->nseg++;
        }
}

static bool in_image(const struct acc *a, const struct map_info *m)
{
    for (unsigned i = 0; i < a->nseg; i++)
        if (m->base < a->hi[i] && m->base + m->len > a->lo[i])
            return true;
    return false;
}

/* One mapping m, counted and (maps) written. */
static void take(struct acc *a, const struct map_info *m)
{
    uint32_t kind = a->self ? mem_kind_at(m->base) : MEM_UNKNOWN;
    bool img = a->self && in_image(a, m);
    bool shared = kind != MEM_UNKNOWN && (kind & MEM_SHARED);
    bool file = img || (kind != MEM_UNKNOWN && (kind & MEM_FILE));
    bool heap = kind != MEM_UNKNOWN && (kind & MEM_BRK);
    a->size += m->len;
    a->rss += m->resident;
    if (shared)
        a->rss_shmem += m->resident;
    else if (file)
        a->rss_file += m->resident;
    else
        a->rss_anon += m->resident;
    if ((m->perms & VMAR_WRITE) && !shared)
        a->data += m->len;
    if (img && (m->perms & VMAR_EXEC)) {
        a->exe += m->len;
        if (!a->text_hi || m->base < a->text_lo)
            a->text_lo = m->base;
        if (m->base + m->len > a->text_hi)
            a->text_hi = m->base + m->len;
    }
    if (!a->lines)
        return;
    const char *name = heap ? "[heap]" : img ? a->image_name : NULL;
    bool offset = img || (kind != MEM_UNKNOWN && (kind & MEM_FILE));   /* a file's */
    char head[96];
    int n = snprintf(head, sizeof(head), "%08llx-%08llx %c%c%c%c %08llx 00:00 0",
                     (unsigned long long)m->base, (unsigned long long)(m->base + m->len),
                     m->perms & VMAR_READ ? 'r' : '-', m->perms & VMAR_WRITE ? 'w' : '-',
                     m->perms & VMAR_EXEC ? 'x' : '-', shared ? 's' : 'p',
                     (unsigned long long)(offset ? m->vmo_off : 0));
    if (name)
        sb_put(a->lines, "%s%*s%s\n", head, n < NAME_COL ? NAME_COL - n : 1, " ", name);
    else
        sb_put(a->lines, "%s\n", head);
}

/* Walk the mappings of pid (ours, or a child's) into a. 0, or -errno. */
static long walk_maps(int32_t pid, struct acc *a)
{
    struct map_info *buf = malloc(MAP_INFO_MAX * sizeof(*buf));
    if (!buf)
        return -ENOMEM;
    long r = 0;
    uint64_t from = 0;
    for (;;) {
        long n = a->self ? kp_process_maps(lx_self_process(), from, buf, MAP_INFO_MAX)
                         : lx_child_maps(pid, from, buf, MAP_INFO_MAX);
        if (n < 0) {
            r = n;
            break;
        }
        for (long i = 0; i < n; i++)
            take(a, &buf[i]);
        if (n < (long)MAP_INFO_MAX)
            break;
        from = buf[n - 1].base + buf[n - 1].len;
    }
    free(buf);
    return r;
}

/* ---- a process --------------------------------------------------------------------- */

struct view {
    int32_t             pid, ppid, pgid, sid;
    bool                self, zombie;
    struct process_info info;
    char                comm[COMM_MAX + 1];
    uint64_t            kids_ns;   /* CPU time of the children we reaped (ours only) */
    uint64_t            sig_pnd, sig_blk, sig_ign, sig_cgt;   /* signal sets (ours only) */
    struct acc          a;
};

static uint64_t peak_size, peak_rss;   /* the largest seen read here (ours); atomics */

static void raise_to(uint64_t *peak, uint64_t v)
{
    uint64_t was = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (was < v && !__atomic_compare_exchange_n(peak, &was, v, true, __ATOMIC_RELAXED,
                                                    __ATOMIC_RELAXED))
        ;
}

/* pid's facts and its mappings' sums; lines (maps) gets the text of maps.
 * 0, or -errno. */
static long view_of(int32_t pid, struct sb *lines, struct view *v)
{
    memset(v, 0, sizeof(*v));
    v->pid = pid;
    v->self = pid == lx_pid();
    if (v->self) {
        if (jam_process_get_info(lx_self_process(), &v->info) != OK)
            return -ESRCH;
        v->ppid = (int32_t)lx_getppid(0, 0, 0, 0, 0, 0);
        v->pgid = lx_pgid();
        v->sid = lx_sid();
        v->kids_ns = lx_children_cpu_ns();
        v->sig_pnd = sig_pending_set();
        v->sig_blk = sig_mask_self();
        v->sig_ign = sig_ignored_set();
        for (int s = 1; s <= 64; s++) {
            struct sig_act_io io;
            if (!sig_action(s, NULL, &io) && io.handler > 1)   /* not SIG_DFL, not SIG_IGN */
                v->sig_cgt |= 1ull << (s - 1);
        }
        const char *name = program_invocation_short_name;
        snprintf(v->comm, sizeof(v->comm), "%s", name && *name ? name : v->info.name);
    } else {
        struct lx_child_facts f;
        long r = lx_child_facts(pid, &f);
        if (r)
            return r;
        v->info = f.info;
        v->ppid = lx_pid();
        v->pgid = f.pgid;
        v->sid = f.sid;
        v->zombie = f.dead;
        snprintf(v->comm, sizeof(v->comm), "%s", v->info.name);
    }
    if (v->info.state >= PROCESS_DYING)
        v->zombie = true;
    v->a.self = v->self;
    v->a.lines = lines;
    if (v->self) {
        image_segments(&v->a);
        if (lines)
            v->a.image_name = proc_exe();
    }
    long r = walk_maps(pid, &v->a);
    if (r == -ESRCH)
        return r;
    if (v->self) {
        raise_to(&peak_size, v->a.size);
        raise_to(&peak_rss, v->a.rss);
    }
    return 0;
}

static uint64_t peak_of(uint64_t *peak, uint64_t now_value, bool self)
{
    return self ? __atomic_load_n(peak, __ATOMIC_RELAXED) : now_value;
}

static char state_of(const struct view *v)
{
    return v->zombie ? 'Z' : v->self ? 'R' : 'S';
}

static const char *state_name(char c)
{
    return c == 'Z' ? "zombie" : c == 'R' ? "running" : "sleeping";
}

static void put_stat(struct sb *b, const struct view *v)
{
    unsigned long long utime = v->info.cpu_ns / TICK_NS, cutime = v->kids_ns / TICK_NS;
    int exit_code = !v->zombie ? 0 : v->info.killed ? 9 : (int)(v->info.exit_code & 0xff) << 8;
    sb_put(b,
           "%d (%s) %c %d %d %d 0 -1 %u "          /* pid comm state ppid pgrp session tty tpgid flags */
           "0 0 0 0 %llu 0 %llu 0 "                /* minflt cminflt majflt cmajflt utime stime cutime cstime */
           "20 0 %u 0 %llu %llu %llu "             /* priority nice threads itrealvalue start vsize rss */
           "18446744073709551615 %llu %llu 0 0 0 " /* rsslim startcode endcode startstack kstkesp kstkeip */
           "%llu %llu %llu %llu 0 0 0 "            /* signal blocked sigignore sigcatch wchan nswap cnswap */
           "17 0 0 0 0 0 0 "                       /* exit_signal processor rt_priority policy blkio guest cguest */
           "0 0 0 0 0 0 0 %d\n",                   /* start_data end_data start_brk arg_start..env_end exit_code */
           v->pid, v->comm, state_of(v), v->ppid, v->pgid, v->sid, PF_RANDOMIZE, utime, cutime,
           v->info.threads, (unsigned long long)(v->info.start_ns / TICK_NS),
           (unsigned long long)v->a.size, (unsigned long long)v->a.rss,
           (unsigned long long)v->a.text_lo, (unsigned long long)v->a.text_hi,
           (unsigned long long)v->sig_pnd, (unsigned long long)v->sig_blk,
           (unsigned long long)v->sig_ign, (unsigned long long)v->sig_cgt, exit_code);
}

/* A mask of cpus bits in Linux's form: 32-bit groups, the top one without
 * leading zeros. */
static void put_mask(struct sb *b, unsigned cpus)
{
    unsigned words = (cpus + 31) / 32;
    for (unsigned w = words; w-- > 0;) {
        unsigned bits = cpus - w * 32 < 32 ? cpus - w * 32 : 32;
        uint32_t m = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
        sb_put(b, w == words - 1 ? "%x" : ",%08x", m);
    }
}

static void put_status(struct sb *b, const struct view *v)
{
    const struct acc *a = &v->a;
    struct kp_public pub;
    unsigned cpus = kp_public_info(&pub) == OK && pub.cpus ? pub.cpus : 1;
    char st = state_of(v);
    sb_put(b, "Name:\t%s\nUmask:\t%04o\nState:\t%c (%s)\n", v->comm, v->self ? lx_umask_now() : 022,
           st, state_name(st));
    sb_put(b, "Tgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n", v->pid, v->pid, v->ppid);
    sb_put(b, "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t64\nGroups:\t\n", ONE_USER,
           ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER);
    sb_put(b, "NStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n", v->pid, v->pid, v->pgid,
           v->sid);
    sb_put(b, "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\n",
           (unsigned long long)peak_of(&peak_size, a->size, v->self) >> 10,
           (unsigned long long)a->size >> 10, 0u, 0u);
    sb_put(b, "VmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\nRssAnon:\t%8llu kB\nRssFile:\t%8llu kB\n",
           (unsigned long long)peak_of(&peak_rss, a->rss, v->self) * (PAGE >> 10),
           (unsigned long long)a->rss * (PAGE >> 10), (unsigned long long)a->rss_anon * (PAGE >> 10),
           (unsigned long long)a->rss_file * (PAGE >> 10));
    sb_put(b, "RssShmem:\t%8llu kB\nVmData:\t%8llu kB\nVmStk:\t%8u kB\nVmExe:\t%8llu kB\n",
           (unsigned long long)a->rss_shmem * (PAGE >> 10), (unsigned long long)a->data >> 10,
           0u, (unsigned long long)a->exe >> 10);
    sb_put(b, "VmLib:\t%8u kB\nVmPTE:\t%8u kB\nVmSwap:\t%8u kB\nHugetlbPages:\t%8u kB\n", 0u, 0u,
           0u, 0u);
    sb_put(b, "CoreDumping:\t0\nTHP_enabled:\t0\nThreads:\t%u\nSigQ:\t0/63756\n",
           v->info.threads);
    sb_put(b, "SigPnd:\t%016llx\nShdPnd:\t%016llx\nSigBlk:\t%016llx\nSigIgn:\t%016llx\n",
           0ull, (unsigned long long)v->sig_pnd, (unsigned long long)v->sig_blk,
           (unsigned long long)v->sig_ign);
    sb_put(b, "SigCgt:\t%016llx\nCapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\n",
           (unsigned long long)v->sig_cgt, 0ull, 0ull, 0ull);
    sb_put(b, "CapBnd:\t000001ffffffffff\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\n"
              "Seccomp:\t0\nSeccomp_filters:\t0\nSpeculation_Store_Bypass:\tthread vulnerable\n");
    sb_put(b, "Cpus_allowed:\t");
    put_mask(b, cpus);
    sb_put(b, "\nCpus_allowed_list:\t0-%u\nMems_allowed:\t00000001\nMems_allowed_list:\t0\n",
           cpus - 1);
    sb_put(b, "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n");
}

static void put_statm(struct sb *b, const struct view *v)
{
    const struct acc *a = &v->a;
    sb_put(b, "%llu %llu %llu %llu 0 %llu 0\n", (unsigned long long)(a->size / PAGE),
           (unsigned long long)a->rss, (unsigned long long)(a->rss_file + a->rss_shmem),
           (unsigned long long)(a->exe / PAGE), (unsigned long long)(a->data / PAGE));
}

/* ---- the system ----------------------------------------------------------------------- */

static void put_meminfo(struct sb *b)
{
    struct kp_public pub;
    unsigned long long total = 0, free_kb = 0;
    if (kp_public_info(&pub) == OK) {
        total = pub.pages * (pub.page_size >> 10);
        free_kb = pub.free_pages * (pub.page_size >> 10);
    }
    enum { ZERO, TOTAL, FREE, HALF, VMALLOC, HUGE_SIZE };
    /* Linux's lines in its order; the figures we have, 0 for the rest. */
    static const struct { const char *key; int kind; } rows[] = {
        { "MemTotal", TOTAL }, { "MemFree", FREE }, { "MemAvailable", FREE }, { "Buffers", ZERO },
        { "Cached", ZERO }, { "SwapCached", ZERO }, { "Active", ZERO }, { "Inactive", ZERO },
        { "Active(anon)", ZERO }, { "Inactive(anon)", ZERO }, { "Active(file)", ZERO },
        { "Inactive(file)", ZERO }, { "Unevictable", ZERO }, { "Mlocked", ZERO },
        { "SwapTotal", ZERO }, { "SwapFree", ZERO }, { "Dirty", ZERO }, { "Writeback", ZERO },
        { "AnonPages", ZERO }, { "Mapped", ZERO }, { "Shmem", ZERO }, { "KReclaimable", ZERO },
        { "Slab", ZERO }, { "SReclaimable", ZERO }, { "SUnreclaim", ZERO },
        { "KernelStack", ZERO }, { "PageTables", ZERO }, { "NFS_Unstable", ZERO },
        { "Bounce", ZERO }, { "WritebackTmp", ZERO }, { "CommitLimit", HALF },
        { "Committed_AS", ZERO }, { "VmallocTotal", VMALLOC }, { "VmallocUsed", ZERO },
        { "VmallocChunk", ZERO }, { "Percpu", ZERO }, { "HardwareCorrupted", ZERO },
        { "AnonHugePages", ZERO }, { "ShmemHugePages", ZERO }, { "ShmemPmdMapped", ZERO },
        { "FileHugePages", ZERO }, { "FilePmdMapped", ZERO }, { "HugePages_Total", -1 },
        { "HugePages_Free", -1 }, { "HugePages_Rsvd", -1 },
        { "HugePages_Surp", -1 }, { "Hugepagesize", HUGE_SIZE }, { "Hugetlb", ZERO },
        { "DirectMap4k", TOTAL }, { "DirectMap2M", ZERO }, { "DirectMap1G", ZERO },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char key[32];
        snprintf(key, sizeof(key), "%s:", rows[i].key);
        int kind = rows[i].kind;
        unsigned long long v = kind == TOTAL ? total : kind == FREE ? free_kb :
                               kind == HALF ? total / 2 : kind == VMALLOC ? 34359738367ull :
                               kind == HUGE_SIZE ? 2048 : 0;
        if (kind < 0)   /* a count of pages: no unit */
            sb_put(b, "%-16s%8llu\n", key, v);
        else
            sb_put(b, "%-16s%8llu kB\n", key, v);
    }
}

static void put_sysstat(struct sb *b)
{
    struct kp_public pub;
    unsigned cpus = kp_public_info(&pub) == OK && pub.cpus ? pub.cpus : 1;
    struct timespec wall = { 0 }, up = { 0 };
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_BOOTTIME, &up);
    long long btime = (long long)wall.tv_sec - up.tv_sec - (wall.tv_nsec < up.tv_nsec ? 1 : 0);
    sb_put(b, "cpu  0 0 0 0 0 0 0 0 0 0\n");
    for (unsigned i = 0; i < cpus; i++)
        sb_put(b, "cpu%u 0 0 0 0 0 0 0 0 0 0\n", i);
    sb_put(b, "intr 0\nctxt 0\nbtime %lld\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n"
              "softirq 0 0 0 0 0 0 0 0 0 0 0\n", btime < 0 ? 0 : btime, lx_pid());
}

/* ---- the entry ------------------------------------------------------------------------- */

long procinfo_text(enum pfile f, int32_t pid, char **text, size_t *len)
{
    struct sb b = { 0 };
    struct view *v = NULL;
    if (f == PF_STAT || f == PF_STATM || f == PF_STATUS || f == PF_MAPS) {
        v = malloc(sizeof(*v));
        if (!v)
            return -ENOMEM;
        long r = view_of(pid, f == PF_MAPS ? &b : NULL, v);
        if (r) {
            free(v);
            free(b.p);
            return r;
        }
    }
    switch (f) {
    case PF_STAT:    put_stat(&b, v); break;
    case PF_STATM:   put_statm(&b, v); break;
    case PF_STATUS:  put_status(&b, v); break;
    case PF_MAPS:    break;   /* the walk wrote it */
    case PF_CPUINFO: cpuinfo_text(&b); break;
    case PF_MEMINFO: put_meminfo(&b); break;
    case PF_SYSSTAT: put_sysstat(&b); break;
    }
    free(v);
    if (b.bad) {
        free(b.p);
        return -ENOMEM;
    }
    if (!b.p) {   /* a zombie's maps: no mappings, no text */
        b.p = malloc(1);
        if (!b.p)
            return -ENOMEM;
    }
    *text = b.p;
    *len = b.len;
    return 0;
}
