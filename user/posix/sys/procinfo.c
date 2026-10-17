/* The text of /proc's generated files (docs/CR-PLAN.md "PRC notes"), in
 * Linux's formats and field order (proc(5)), made when a file is read:
 *
 *   /proc/<pid>/stat, statm, status, maps, smaps_rollup   for our own pid and for each
 *                         child of ours not yet reaped; stat, status, statm
 *                         and maps of one of our threads too
 *                         (/proc/<pid>/task/<tid>/..., or /proc/<tid>/...)
 *   /proc/<pid>/cmdline   ours (the argument strings, start.c's run)
 *   /proc/meminfo, /proc/stat, /proc/version, /proc/sys/kernel/...   the system's
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
 * stat: tty 0 and tpgid -1, flags PF_RANDOMIZE, minflt the address space's
 * page faults (the kernel counts them: process_info.faults, a thread's file
 * has the process's), cminflt, majflt and cmajflt 0, stime 0
 * (the kernel keeps one CPU time: utime has all of it; a thread's utime is
 * its own), cutime the CPU time
 * of the children we reaped, cstime 0, priority 20, nice 0, itrealvalue 0,
 * rsslim unlimited, startstack 0, the stack pointers 0, signal masks (ours
 * only; a child's are 0; a thread's are the calling thread's), wchan 0,
 * exit_signal 17, processor 0, rt_priority
 * 0, policy 0, start_data, end_data and start_brk 0, and the
 * exit code of a zombie as wait would give it. Ours has the argument and
 * environment addresses (start.c's run of strings). A child's stat has no
 * startcode or endcode, and its comm is the name it was created with. A
 * thread's: its id first, state R for the thread reading and S for the
 * others (the kernel doesn't say which wait), the process's start time, and
 * the program's name (the calling thread's own, if it set one: other
 * threads' names are theirs alone).
 * status: no tracer, uid and gid 1000, no groups, no capabilities, no
 * seccomp, VmLck, VmPin, VmLib and VmSwap 0, VmStk the main stack's mapping
 * (the one holding procfs.c's constructor's address; VmData leaves it out),
 * VmPTE 0, VmPeak and VmHWM
 * the largest VmSize and VmRSS this process has seen read here (a child's:
 * its current ones), all CPUs allowed, no context switches counted.
 * maps: device 00:00 and inode 0 on every line; the name of an image's
 * lines (ours only), [heap] and [stack] (the main one); no [vdso] or names
 * for files mapped with mmap; the offset only where the VMO is a file (an
 * image, a file's own pages). Anonymous memory and shared memory are not
 * told apart by name (all are blank).
 * smaps_rollup: Rss and Pss are the pages present (no page is known to be
 * shared with another process, so Pss is Rss); a page is dirty if its
 * mapping can be written and shared if the mapping is MAP_SHARED (no
 * per-page bits); the rest 0 (put_smaps_rollup).
 * meminfo: MemTotal and MemFree are the allocator's (free pages exclude
 * the kernel heap's cached objects); MemAvailable is MemFree (no page cache
 * is kept apart: files are the pager's pages, counted in no field). Buffers,
 * Cached and the active and inactive figures have no source, and a program
 * (base's memory metrics) wants them non-zero and under MemTotal: they are
 * fractions of the memory in use (Buffers 1/64, Cached 1/8 split evenly
 * between active and inactive file, the rest anonymous, 3/4 active). The
 * other fields 0 (swap, huge pages), Hugepagesize 2048.
 * stat (system): the CPU times 0 (cpu_stat is root's), btime real, processes
 * our pid, procs_running 1, the rest 0. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <errno.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <time.h>
#include "calls.h"
#include "files.h"
#include "proc.h"
#include "procinfo.h"
#include "sig.h"
#include "threads.h"

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
    uint64_t private_dirty, private_clean, shared_dirty, shared_clean;   /* pages present, by kind
                                                                          * of mapping (smaps_rollup) */
    uint64_t first, last;                    /* the lowest mapping's start, the highest's end */
    uint64_t exe, data, stk;                 /* VmExe, VmData, VmStk: bytes */
    uint64_t text_lo, text_hi;               /* the image's executable mappings */
    bool     self;                           /* ours: libc's table and our image apply */
    uintptr_t stack_addr;                    /* ours: an address on the main stack, or 0 */
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
    bool stack = a->self && !img && a->stack_addr >= m->base && a->stack_addr - m->base < m->len;
    a->size += m->len;
    a->rss += m->resident;
    if (!a->last || m->base < a->first)
        a->first = m->base;
    if (m->base + m->len > a->last)
        a->last = m->base + m->len;
    bool dirty = m->perms & VMAR_WRITE;   /* the pages may have been written: no per-page bit */
    if (shared)
        *(dirty ? &a->shared_dirty : &a->shared_clean) += m->resident;
    else
        *(dirty ? &a->private_dirty : &a->private_clean) += m->resident;
    if (shared)
        a->rss_shmem += m->resident;
    else if (file)
        a->rss_file += m->resident;
    else
        a->rss_anon += m->resident;
    if (stack)
        a->stk += m->len;
    else if ((m->perms & VMAR_WRITE) && !shared)
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
    const char *name = heap ? "[heap]" : stack ? "[stack]" : img ? a->image_name : NULL;
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
    uint32_t            tid;       /* one of our threads: its id, else 0 (the process) */
    bool                self, zombie, running;   /* running: the process or the calling thread */
    struct process_info info;      /* a thread's file: cpu_ns is the thread's own */
    char                comm[COMM_MAX + 1];
    const char         *arg_start, *arg_end, *env_end;   /* the strings' run (ours only) */
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

/* pid's facts (and thread tid's, if not 0: one of ours) and its mappings'
 * sums; lines (maps) gets the text of maps. 0, or -errno. */
static long view_of(int32_t pid, uint32_t tid, struct sb *lines, struct view *v)
{
    memset(v, 0, sizeof(*v));
    v->pid = pid;
    v->tid = tid;
    v->self = pid == lx_pid();
    if (v->self) {
        if (jam_process_get_info(lx_self_process(), &v->info) != OK)
            return -ESRCH;
        v->running = true;
        if (tid) {   /* a thread: its own CPU time, and it is the one running if it is us */
            uint64_t ns;
            if (thread_cpu_ns(tid, &ns))
                return -ESRCH;
            v->info.cpu_ns = ns;
            v->running = tid == (uint32_t)lx_gettid(0, 0, 0, 0, 0, 0);
        }
        lx_arg_area(&v->arg_start, &v->arg_end, &v->env_end);
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
        char mine[COMM_MAX + 1];   /* the calling thread's own name, if it set one */
        if (tid && v->running && lx_prctl(PR_GET_NAME, (long)(uintptr_t)mine, 0, 0, 0, 0) == 0)
            snprintf(v->comm, sizeof(v->comm), "%s", mine);
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
        v->a.stack_addr = proc_stack_addr();
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
    return v->zombie ? 'Z' : v->running ? 'R' : 'S';
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
           "%llu 0 0 0 %llu 0 %llu 0 "             /* minflt cminflt majflt cmajflt utime stime cutime cstime */
           "20 0 %u 0 %llu %llu %llu "             /* priority nice threads itrealvalue start vsize rss */
           "18446744073709551615 %llu %llu 0 0 0 " /* rsslim startcode endcode startstack kstkesp kstkeip */
           "%llu %llu %llu %llu 0 0 0 "            /* signal blocked sigignore sigcatch wchan nswap cnswap */
           "17 0 0 0 0 0 0 "                       /* exit_signal processor rt_priority policy blkio guest cguest */
           "0 0 0 %llu %llu %llu %llu %d\n",       /* start_data end_data start_brk arg_start arg_end env_start env_end exit_code */
           v->tid ? (int)v->tid : v->pid, v->comm, state_of(v), v->ppid, v->pgid, v->sid,
           PF_RANDOMIZE, (unsigned long long)v->info.faults, utime, cutime,
           v->info.threads, (unsigned long long)(v->info.start_ns / TICK_NS),
           (unsigned long long)v->a.size, (unsigned long long)v->a.rss,
           (unsigned long long)v->a.text_lo, (unsigned long long)v->a.text_hi,
           (unsigned long long)v->sig_pnd, (unsigned long long)v->sig_blk,
           (unsigned long long)v->sig_ign, (unsigned long long)v->sig_cgt,
           (unsigned long long)(uintptr_t)v->arg_start, (unsigned long long)(uintptr_t)v->arg_end,
           (unsigned long long)(uintptr_t)v->arg_end, (unsigned long long)(uintptr_t)v->env_end,
           exit_code);
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
    int id = v->tid ? (int)v->tid : v->pid;   /* a thread's Pid is its own id, its Tgid ours */
    sb_put(b, "Tgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n", v->pid, id, v->ppid);
    sb_put(b, "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t64\nGroups:\t\n", ONE_USER,
           ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER, ONE_USER);
    sb_put(b, "NStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n", v->pid, id, v->pgid, v->sid);
    sb_put(b, "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\n",
           (unsigned long long)peak_of(&peak_size, a->size, v->self) >> 10,
           (unsigned long long)a->size >> 10, 0u, 0u);
    sb_put(b, "VmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\nRssAnon:\t%8llu kB\nRssFile:\t%8llu kB\n",
           (unsigned long long)peak_of(&peak_rss, a->rss, v->self) * (PAGE >> 10),
           (unsigned long long)a->rss * (PAGE >> 10), (unsigned long long)a->rss_anon * (PAGE >> 10),
           (unsigned long long)a->rss_file * (PAGE >> 10));
    sb_put(b, "RssShmem:\t%8llu kB\nVmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmExe:\t%8llu kB\n",
           (unsigned long long)a->rss_shmem * (PAGE >> 10), (unsigned long long)a->data >> 10,
           (unsigned long long)a->stk >> 10, (unsigned long long)a->exe >> 10);
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
           (unsigned long long)(a->exe / PAGE), (unsigned long long)((a->data + a->stk) / PAGE));
}

/* smaps_rollup: the sums smaps would give over every mapping, one pseudo-
 * mapping of the whole range first. Linux counts the pages by who else maps
 * them and whether each was written; we know neither, so a page counts whole
 * (Pss is Rss), is dirty if its mapping can be written and shared if it is
 * MAP_SHARED. Nothing is swapped, locked, lazily freed or huge. */
static void put_smaps_rollup(struct sb *b, const struct view *v)
{
    const struct acc *a = &v->a;
    char head[96];
    int n = snprintf(head, sizeof(head), "%08llx-%08llx ---p 00000000 00:00 0",
                     (unsigned long long)a->first, (unsigned long long)a->last);
    sb_put(b, "%s%*s[rollup]\n", head, n < NAME_COL ? NAME_COL - n : 1, " ");
    const struct { const char *name; uint64_t pages; } fields[] = {
        { "Rss", a->rss }, { "Pss", a->rss },
        { "Pss_Dirty", a->private_dirty + a->shared_dirty },
        { "Pss_Anon", a->rss_anon }, { "Pss_File", a->rss_file }, { "Pss_Shmem", a->rss_shmem },
        { "Shared_Clean", a->shared_clean }, { "Shared_Dirty", a->shared_dirty },
        { "Private_Clean", a->private_clean }, { "Private_Dirty", a->private_dirty },
        { "Referenced", a->rss }, { "Anonymous", a->rss_anon }, { "LazyFree", 0 },
        { "AnonHugePages", 0 }, { "ShmemPmdMapped", 0 }, { "FilePmdMapped", 0 },
        { "Shared_Hugetlb", 0 }, { "Private_Hugetlb", 0 }, { "Swap", 0 }, { "SwapPss", 0 },
        { "Locked", 0 },
    };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char key[24];
        snprintf(key, sizeof(key), "%s:", fields[i].name);
        sb_put(b, "%-15s %8llu kB\n", key, (unsigned long long)(fields[i].pages * (PAGE / 1024)));
    }
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
    /* The memory in use, split as the header says (no source: fractions of
     * it, so each is non-zero and under MemTotal, as base's metrics want). */
    unsigned long long used = total > free_kb ? total - free_kb : 0;
    unsigned long long buffers = used / 64, cached = used / 8;
    unsigned long long anon = used - buffers - cached;
    unsigned long long act_anon = anon * 3 / 4, inact_anon = anon - act_anon;
    unsigned long long act_file = cached / 2, inact_file = cached - act_file;
    enum { ZERO, TOTAL, FREE, HALF, VMALLOC, HUGE_SIZE, BUFFERS, CACHED, ACTIVE, INACTIVE,
           ACT_ANON, INACT_ANON, ACT_FILE, INACT_FILE, ANON };
    /* Linux's lines in its order; the figures we have, 0 for the rest. */
    static const struct { const char *key; int kind; } rows[] = {
        { "MemTotal", TOTAL }, { "MemFree", FREE }, { "MemAvailable", FREE },
        { "Buffers", BUFFERS }, { "Cached", CACHED }, { "SwapCached", ZERO },
        { "Active", ACTIVE }, { "Inactive", INACTIVE },
        { "Active(anon)", ACT_ANON }, { "Inactive(anon)", INACT_ANON },
        { "Active(file)", ACT_FILE }, { "Inactive(file)", INACT_FILE },
        { "Unevictable", ZERO }, { "Mlocked", ZERO },
        { "SwapTotal", ZERO }, { "SwapFree", ZERO }, { "Dirty", ZERO }, { "Writeback", ZERO },
        { "AnonPages", ANON }, { "Mapped", ZERO }, { "Shmem", ZERO }, { "KReclaimable", ZERO },
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
                               kind == HUGE_SIZE ? 2048 : kind == BUFFERS ? buffers :
                               kind == CACHED ? cached : kind == ACTIVE ? act_anon + act_file :
                               kind == INACTIVE ? inact_anon + inact_file :
                               kind == ACT_ANON ? act_anon : kind == INACT_ANON ? inact_anon :
                               kind == ACT_FILE ? act_file : kind == INACT_FILE ? inact_file :
                               kind == ANON ? anon : 0;
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

/* Raw bytes (a NUL among them) onto b. */
static void sb_bytes(struct sb *b, const void *p, size_t n)
{
    if (b->bad)
        return;
    if (b->cap - b->len < n + 1) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap - b->len < n + 1)
            cap *= 2;
        char *q = realloc(b->p, cap);
        if (!q) {
            b->bad = true;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

/* ---- cmdline, version and sys/kernel ---------------------------------------------------- */

/* /proc/self/cmdline as Linux's kernel reads it from the process: the
 * argument strings, NUL-ended one after the other (start.c's run). A
 * program that overwrote the run's last byte has changed its title
 * (setproctitle): the text is then the run from its start to the first
 * NUL (that included), a page at most, which is what Linux's does
 * (get_mm_proctitle). */
static void put_cmdline(struct sb *b)
{
    const char *as, *ae, *ee;
    lx_arg_area(&as, &ae, &ee);
    if (!as || ae <= as)
        return;
    if (ae[-1]) {
        size_t room = (size_t)(ee - as) < PAGE ? (size_t)(ee - as) : PAGE;
        size_t l = strnlen(as, room);
        sb_bytes(b, as, l < room ? l + 1 : l);
    } else {
        sb_bytes(b, as, (size_t)(ae - as));
    }
}

/* One of uname's fields, and a line end. */
static void put_uname(struct sb *b, enum pfile f)
{
    struct utsname u;
    lx_uname_fields(&u);
    switch (f) {
    case PF_VERSION:   /* Linux's shape: "<name> version <release> (<who>) <version>" */
        sb_put(b, "%s version %s (%s@%s) %s\n", u.sysname, u.release, "jamos", u.nodename,
               u.version);
        break;
    case PF_OSRELEASE: sb_put(b, "%s\n", u.release); break;
    case PF_OSTYPE:    sb_put(b, "%s\n", u.sysname); break;
    case PF_KVERSION:  sb_put(b, "%s\n", u.version); break;
    case PF_HOSTNAME:  sb_put(b, "%s\n", u.nodename); break;
    default:           break;
    }
}

/* ---- the entry ------------------------------------------------------------------------- */

long procinfo_text(enum pfile f, int32_t pid, uint32_t tid, char **text, size_t *len)
{
    struct sb b = { 0 };
    struct view *v = NULL;
    if (f == PF_STAT || f == PF_STATM || f == PF_STATUS || f == PF_MAPS || f == PF_SMAPS_ROLLUP) {
        v = malloc(sizeof(*v));
        if (!v)
            return -ENOMEM;
        long r = view_of(pid, tid, f == PF_MAPS ? &b : NULL, v);
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
    case PF_SMAPS_ROLLUP: put_smaps_rollup(&b, v); break;
    case PF_CMDLINE: put_cmdline(&b); break;
    case PF_CPUINFO: cpuinfo_text(&b); break;
    case PF_MEMINFO: put_meminfo(&b); break;
    case PF_SYSSTAT: put_sysstat(&b); break;
    case PF_VERSION: case PF_OSRELEASE: case PF_OSTYPE: case PF_KVERSION: case PF_HOSTNAME:
        put_uname(&b, f);
        break;
    case PF_MEM:     break;   /* not text: procfs.c reads the memory */
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
