/* top: live CPU use per CPU and per process, and memory, a frame every
 * -d seconds (default 1) until q or Ctrl+C (or -n frames). In a pipe the
 * frames follow each other without colours or clearing the screen. */
#include "sh.h"

struct sample {
    uint64_t          t;       /* when (uptime ns) */
    struct cpu_stat  *cpu;     /* SH_MAX_CPUS of them (malloc'd) */
    uint32_t          ncpu;    /* filled */
    struct proc_stat *proc;    /* SH_MAX_PROCS of them (malloc'd) */
    int               nproc;   /* filled */
};

static bool take(struct sample *s)
{
    s->t = now();
    int64_t r = jam_cpu_stat(sh_root(), 0, s->cpu, SH_MAX_CPUS);
    s->ncpu = r > 0 ? (uint32_t)r : 0;
    s->nproc = sh_procs(s->proc, "top");
    return r >= 0 && s->nproc >= 0;
}

static void bar(unsigned pm, char *out, unsigned width)
{
    unsigned fill = (pm * width + 500) / 1000;
    for (unsigned i = 0; i < width; i++)
        out[i] = i < fill ? '|' : ' ';
    out[width] = '\0';
}

/* Idle time CPU i gained from a to b. */
static uint64_t idle_delta(const struct sample *a, const struct sample *b, uint32_t i)
{
    return b->cpu[i].idle_ns > a->cpu[i].idle_ns ? b->cpu[i].idle_ns - a->cpu[i].idle_ns : 0;
}

static void header(const struct sample *a, const struct sample *b, const struct sys_info *si,
                   uint64_t dt)
{
    uint64_t idle_all = 0;
    for (uint32_t i = 0; i < b->ncpu && i < a->ncpu; i++)
        idle_all += idle_delta(a, b, i);
    unsigned busy = 1000 - sh_permille(idle_all, dt * (b->ncpu ? b->ncpu : 1));
    char up[40], used[24], total[24];
    sh_fmt_uptime(si->uptime_ns, up, sizeof(up));
    uint64_t tot = si->mem_total_pages * 4096, fr = si->mem_free_pages * 4096;
    sh_say("top - up %s, %u CPUs, %d processes, CPU %u.%u%%, memory %s used of %s\n", up, b->ncpu,
           b->nproc, busy / 10, busy % 10, sh_human(tot - fr, used, sizeof(used)),
           sh_human(tot, total, sizeof(total)));
}

/* Per-CPU bars, two columns. */
static void cpu_bars(const struct sample *a, const struct sample *b, uint64_t dt, bool screen)
{
    uint32_t rows = (b->ncpu + 1) / 2;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t col = 0; col < 2; col++) {
            uint32_t i = r + col * rows;
            if (i >= b->ncpu || i >= a->ncpu)
                continue;
            unsigned pm = 1000 - sh_permille(idle_delta(a, b, i), dt);
            char bb[24];
            bar(pm, bb, 20);
            const char *hi = !screen ? "" : pm >= 700 ? "\033[91m" : pm >= 300 ? "\033[93m"
                                                                              : "\033[92m";
            sh_say("%3u %s [%s%s%s] %3u%%%s", i, sh_cpu_type(b->cpu[i].type), hi, bb,
                   screen ? "\033[0m" : "", (pm + 5) / 10, col == 0 ? "    " : "");
        }
        sh_say("\n");
    }
}

/* Processes by CPU use in this interval, the busiest 24. */
static void processes(const struct sample *a, const struct sample *b, uint64_t dt, bool screen)
{
    struct row {
        int      i;   /* index in b->proc */
        uint64_t d;   /* its CPU time in the interval, ns */
    } rows[SH_MAX_PROCS];
    int np = 0;
    for (int i = 0; i < b->nproc; i++) {
        uint64_t prev = 0;
        for (int j = 0; j < a->nproc; j++)
            if (a->proc[j].koid == b->proc[i].koid)
                prev = a->proc[j].cpu_ns;
        uint64_t d = b->proc[i].cpu_ns > prev ? b->proc[i].cpu_ns - prev : 0;
        int k = np++;
        while (k > 0 && rows[k - 1].d < d) {
            rows[k] = rows[k - 1];
            k--;
        }
        rows[k] = (struct row){ i, d };
    }
    sh_say("%s  PID  NAME                 CPU%%    CPU TIME  THR  JOB MEM%s\n",
           screen ? "\033[1m" : "", screen ? "\033[0m" : "");
    for (int k = 0; k < np && k < 24; k++) {
        const struct proc_stat *p = &b->proc[rows[k].i];
        unsigned pm = (unsigned)(rows[k].d * 1000 / dt);   /* may pass 100% (threads) */
        char t[24], m[24];
        sh_fmt_cpu_time(p->cpu_ns, t, sizeof(t));
        sh_say("%5lu  %-19s %3u.%u  %10s  %3u %8s\n", (unsigned long)p->koid, p->name, pm / 10,
               pm % 10, t, p->threads, sh_human(p->job_pages * 4096, m, sizeof(m)));
    }
}

static void frame(const struct sample *a, const struct sample *b, const struct sys_info *si,
                  bool screen)
{
    uint64_t dt = b->t > a->t ? b->t - a->t : 1;
    if (screen)
        sh_say("\033[2J\033[H");
    header(a, b, si, dt);
    cpu_bars(a, b, dt, screen);
    processes(a, b, dt, screen);
    if (screen)
        sh_say("q or Ctrl+C: quit\n");
}

/* -d's seconds ("0.5" allowed) -> ms, at least 100; false if not a number. */
static bool parse_delay(const char *s, uint64_t *delay_ms)
{
    uint64_t whole = 0, frac = 0, scale = 1000;
    const char *dot = strchr(s, '.');
    char w[16];
    size_t wl = dot ? (size_t)(dot - s) : strlen(s);
    if (wl >= sizeof(w))
        wl = sizeof(w) - 1;
    memcpy(w, s, wl);
    w[wl] = '\0';
    if (wl && !sh_parse_u64(w, &whole))
        return false;
    for (const char *p = dot ? dot + 1 : ""; *p && scale > 1; p++) {
        if (*p < '0' || *p > '9')
            return false;
        scale /= 10;
        frac += (uint64_t)(*p - '0') * scale;
    }
    *delay_ms = whole * 1000 + frac;
    if (*delay_ms < 100)
        *delay_ms = 100;
    return true;
}

/* Frames until q, Ctrl+C or `frames` of them (0: no limit). */
static void run_frames(struct sample *s, uint64_t delay_ms, uint64_t frames)
{
    bool screen = !sh_piped();
    for (uint64_t f = 0; !frames || f < frames; f++) {
        uint64_t deadline = now() + delay_ms * NS_PER_MS;
        bool quit = false;
        while (!quit && now() < deadline) {
            int key = sh_poll_key(deadline);
            quit = key == 'q' || key == 'Q' || key == 3;
        }
        struct sample *a = &s[f % 2], *b = &s[(f + 1) % 2];
        struct sys_info si;
        if (quit || !take(b) || jam_sys_info(sh_root(), &si) != OK)
            break;
        frame(a, b, &si, screen);
    }
}

SH_CMD(top)
{
    uint64_t delay_ms = 1000, frames = 0;   /* 0: until q */
    for (int i = 1; i < argc; i++) {
        uint64_t v;
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            if (!parse_delay(argv[++i], &delay_ms))
                goto usage;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc && sh_parse_u64(argv[i + 1], &v)) {
            frames = v;
            i++;
        } else {
            goto usage;
        }
    }
    struct sample s[2];
    for (int k = 0; k < 2; k++) {
        s[k].cpu = calloc(SH_MAX_CPUS, sizeof(struct cpu_stat));
        s[k].proc = calloc(SH_MAX_PROCS, sizeof(struct proc_stat));
    }
    int st = 0;
    if (!s[0].cpu || !s[0].proc || !s[1].cpu || !s[1].proc || !take(&s[0]))
        st = 1;
    else
        run_frames(s, delay_ms, frames);
    for (int k = 0; k < 2; k++) {
        free(s[k].cpu);
        free(s[k].proc);
    }
    return st;
usage:
    sh_tty("usage: top [-d seconds] [-n frames]\n");
    return 2;
}
