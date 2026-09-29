/* Information commands: uname version uptime date lscpu free ps top whoami
 * hostname dmesg history. The numbers come from syscalls 130-133
 * (sys_info, cpu_stat, proc_list, rtc_read; RIGHT_READ on the root). */
#include "sh.h"

#define MAX_CPUS_SH  256
#define MAX_PROCS_SH 512

static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static bool get_sysinfo(struct sys_info *s, const char *who)
{
    status_t st = jam_sys_info(sh_root(), s);
    if (st != OK)
        sh_tty("%s: %s\n", who, status_str(st));
    return st == OK;
}

/* "3 days, 4:05", "1:02:03", "5 min 3 s" */
static void fmt_uptime(uint64_t ns, char *buf, size_t cap)
{
    uint64_t s = ns / SH_S, d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d)
        snprintf(buf, cap, "%lu day%s, %lu:%02lu", (unsigned long)d, d == 1 ? "" : "s",
                 (unsigned long)h, (unsigned long)m);
    else if (h)
        snprintf(buf, cap, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m,
                 (unsigned long)(s % 60));
    else
        snprintf(buf, cap, "%lu min %lu s", (unsigned long)m, (unsigned long)(s % 60));
}

/* ---- calendar and time zones ------------------------------------------------------ */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

struct civil {
    int64_t  year;
    unsigned month, day, hour, minute, second, wday;
};

static void civil_from_secs(int64_t t, struct civil *c)
{
    int64_t z = (t >= 0 ? t : t - 86399) / 86400;
    int64_t secs = t - z * 86400;
    c->wday = (unsigned)(((z % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = (int64_t)yoe + era * 400 + (c->month <= 2);
    c->hour = (unsigned)(secs / 3600);
    c->minute = (unsigned)(secs / 60 % 60);
    c->second = (unsigned)(secs % 60);
}

struct tz {
    bool sydney;       /* Australia/Sydney: AEST +10, AEDT +11 (Oct..Apr) */
    int  off_min;      /* fixed zones */
    char name[24];
};

/* TZ: Australia/Sydney (also Sydney, AEST, AEDT, local), UTC/GMT, or
 * [UTC|GMT]+H[:MM] / -H[:MM]. false if not understood. */
static bool parse_tz(const char *s, struct tz *tz)
{
    memset(tz, 0, sizeof(*tz));
    if (!s || !*s || !strcmp(s, "Australia/Sydney") || !strcmp(s, "Sydney") ||
        !strcmp(s, "AEST") || !strcmp(s, "AEDT") || !strcmp(s, "local") ||
        !strcmp(s, "Australia/Melbourne") || !strcmp(s, "Australia/Canberra")) {
        tz->sydney = true;
        return true;
    }
    const char *p = s;
    if (!strncmp(p, "UTC", 3) || !strncmp(p, "GMT", 3))
        p += 3;
    else if (*p == 'Z' && !p[1])
        p++;
    if (!*p) {
        memcpy(tz->name, "UTC", 4);
        return true;
    }
    if (*p != '+' && *p != '-')
        return false;
    int sign = *p++ == '-' ? -1 : 1;
    unsigned h = 0, m = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) {
        h = h * 10 + (unsigned)(*p++ - '0');
        digits++;
    }
    if (!digits || h > 14)
        return false;
    if (*p == ':') {
        p++;
        if (!(p[0] >= '0' && p[0] <= '5' && p[1] >= '0' && p[1] <= '9'))
            return false;
        m = (unsigned)(p[0] - '0') * 10 + (unsigned)(p[1] - '0');
        p += 2;
    }
    if (*p)
        return false;
    tz->off_min = sign * (int)(h * 60 + m);
    snprintf(tz->name, sizeof(tz->name), "UTC%c%02u:%02u", sign < 0 ? '-' : '+', h, m);
    return true;
}

/* 02:00 AEST on the first Sunday of `month` in year y, as UTC seconds,
 * with `local_hour` and offset hours: the Sydney switch moments. */
static int64_t first_sunday_utc(int64_t y, unsigned month, int local_hour, int off_h)
{
    int64_t d = days_from_civil(y, month, 1);
    unsigned wd = (unsigned)(((d % 7) + 11) % 7);
    d += (7 - wd) % 7;
    return d * 86400 + (int64_t)(local_hour - off_h) * 3600;
}

/* Sydney: daylight time from the first Sunday of October 02:00 AEST to the
 * first Sunday of April 03:00 AEDT (since 2008). */
static bool sydney_dst(int64_t utc)
{
    struct civil c;
    civil_from_secs(utc + 10 * 3600, &c);
    int64_t end = first_sunday_utc(c.year, 4, 3, 11), start = first_sunday_utc(c.year, 10, 2, 10);
    return utc < end || utc >= start;
}

/* The offset (minutes) and zone name at UTC time t. */
static int tz_at(const struct tz *tz, int64_t t, const char **name)
{
    if (tz->sydney) {
        bool dst = sydney_dst(t);
        *name = dst ? "AEDT" : "AEST";
        return dst ? 660 : 600;
    }
    *name = tz->name;
    return tz->off_min;
}

/* Local wall time `local` (seconds, as if UTC) in tz -> UTC. */
static int64_t tz_local_to_utc(const struct tz *tz, int64_t local)
{
    const char *n;
    if (!tz->sydney)
        return local - (int64_t)tz->off_min * 60;
    int64_t t = local - 600 * 60;   /* standard time first */
    if (tz_at(tz, t, &n) == 660)
        t = local - 660 * 60;
    return t;
}

/* The time now (UTC seconds) from the RTC, and the raw reading. */
static bool clock_now(int64_t *utc, struct rtc_time *raw, const char *who)
{
    status_t st = jam_rtc_read(sh_root(), raw);
    if (st != OK) {
        sh_tty("%s: the real-time clock: %s\n", who, status_str(st));
        return false;
    }
    int64_t local = days_from_civil(raw->year, raw->month, raw->day) * 86400 +
                    raw->hour * 3600 + raw->minute * 60 + raw->second;
    const char *rtc = sh_getvar("RTC");
    struct tz tz;
    if (rtc && (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC")))
        *utc = local;
    else if (parse_tz(rtc, &tz))   /* "local" = Australia/Sydney */
        *utc = tz_local_to_utc(&tz, local);
    else
        *utc = local;
    return true;
}

static void fmt_time(int64_t utc, const struct tz *tz, char *buf, size_t cap, bool with_zone)
{
    const char *name;
    int off = tz_at(tz, utc, &name);
    struct civil c;
    civil_from_secs(utc + (int64_t)off * 60, &c);
    int a = off < 0 ? -off : off;
    if (with_zone)
        snprintf(buf, cap, "%s %u %s %ld %02u:%02u:%02u %s (UTC%c%02d:%02d)", wdays[c.wday], c.day,
                 months[c.month - 1], (long)c.year, c.hour, c.minute, c.second, name,
                 off < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(buf, cap, "%02u:%02u:%02u", c.hour, c.minute, c.second);
}

static bool local_tz(struct tz *tz, const char *who)
{
    const char *s = sh_getvar("TZ");
    if (parse_tz(s, tz))
        return true;
    sh_tty("%s: TZ=%s not understood (Australia/Sydney, UTC, +10, -5:30): using UTC\n", who, s);
    parse_tz("UTC", tz);
    return false;
}

SH_CMD(date)
{
    bool utc_only = false, raw = false, given = false;
    int64_t utc = 0;
    for (int i = 1; i < argc; i++) {
        uint64_t v;
        if (!strcmp(argv[i], "-u")) {
            utc_only = true;
        } else if (!strcmp(argv[i], "-r")) {
            raw = true;
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc && argv[i + 1][0] == '@' &&
                   sh_parse_u64(argv[i + 1] + 1, &v)) {
            utc = (int64_t)v;
            given = true;
            i++;
        } else {
            sh_tty("usage: date [-u] [-r] [-d @unix-seconds]\n");
            return 2;
        }
    }
    struct rtc_time r;
    if (!given && !clock_now(&utc, &r, "date"))
        return 1;
    struct tz tz;
    if (utc_only)
        parse_tz("UTC", &tz);
    else
        local_tz(&tz, "date");
    char buf[80];
    fmt_time(utc, &tz, buf, sizeof(buf), true);
    sh_say("%s\n", buf);
    if (raw && !given) {
        const char *rtc = sh_getvar("RTC");
        bool rtc_utc = rtc && (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC"));
        sh_say("RTC %04u-%02u-%02u %02u:%02u:%02u (register B %#x: %s, %s), read as %s time "
               "(RTC=%s; set RTC=utc if the clock keeps UTC, RTC=local if Sydney time)\n",
               r.year, r.month, r.day, r.hour, r.minute, r.second, r.status_b,
               r.status_b & 4 ? "binary" : "BCD", r.status_b & 2 ? "24-hour" : "12-hour",
               rtc_utc ? "UTC" : "Sydney", rtc ? rtc : "local");
        sh_say("Unix time %ld\n", (long)utc);
    }
    return 0;
}

/* ---- uname, version, whoami, hostname, uptime ---------------------------------------------- */

SH_CMD(uname)
{
    struct sys_info s;
    if (argc > 1 && strcmp(argv[1], "-a") && strcmp(argv[1], "-r") && strcmp(argv[1], "-s")) {
        sh_tty("usage: uname [-a|-r|-s]\n");
        return 2;
    }
    if (argc == 1 || !strcmp(argv[1], "-s")) {
        sh_say("Jam OS\n");
        return 0;
    }
    if (!get_sysinfo(&s, "uname"))
        return 1;
    if (!strcmp(argv[1], "-r")) {
        sh_say("%s\n", s.version);
        return 0;
    }
    const char *host = sh_getvar("HOSTNAME");
    sh_say("Jam OS %s %s x86_64 %s\n", host ? host : "jamos", s.version, s.cpu_brand);
    return 0;
}

SH_CMD(version)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    if (!get_sysinfo(&s, "version"))
        return 1;
    sh_say("Jam OS %s%s\n", s.version, s.flags & SYSINFO_KTESTS ? " (kernel tests built in)" : "");
    return 0;
}

SH_CMD(whoami)
{
    (void)argc;
    (void)argv;
    const char *u = sh_getvar("USER");
    sh_say("%s\n", u ? u : "jam");
    return 0;
}

SH_CMD(hostname)
{
    (void)argc;
    (void)argv;
    const char *h = sh_getvar("HOSTNAME");
    sh_say("%s\n", h ? h : "jamos");
    return 0;
}

static bool get_cpus(struct cpu_stat *c, uint32_t *n, const char *who)
{
    int64_t r = jam_cpu_stat(sh_root(), 0, c, MAX_CPUS_SH);
    if (r < 0) {
        sh_tty("%s: %s\n", who, status_str((status_t)r));
        return false;
    }
    *n = (uint32_t)r;
    return true;
}

/* Tenths of a percent, clamped to 0..1000. */
static unsigned permille(uint64_t part, uint64_t whole)
{
    if (!whole)
        return 0;
    uint64_t v = part * 1000 / whole;
    return v > 1000 ? 1000 : (unsigned)v;
}

SH_CMD(uptime)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    struct cpu_stat *c = calloc(MAX_CPUS_SH, sizeof(*c));
    uint32_t n = 0;
    if (!c || !get_sysinfo(&s, "uptime") || !get_cpus(c, &n, "uptime")) {
        free(c);
        return 1;
    }
    uint64_t idle = 0;
    uint32_t online = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (c[i].online) {
            online++;
            idle += c[i].idle_ns;
        }
    }
    free(c);
    uint64_t total = s.uptime_ns * online;
    unsigned busy = 1000 - permille(idle, total);
    char up[40], now[16] = "";
    fmt_uptime(s.uptime_ns, up, sizeof(up));
    int64_t utc;
    struct rtc_time r;
    struct tz tz;
    if (clock_now(&utc, &r, "uptime")) {
        local_tz(&tz, "uptime");
        fmt_time(utc, &tz, now, sizeof(now), false);
    }
    sh_say("%s up %s, 1 user, %u CPUs, %u.%u%% busy since boot\n", now, up, s.cpu_count, busy / 10,
           busy % 10);
    return 0;
}

/* ---- lscpu, free ------------------------------------------------------------------------------ */

static const char *type_name(uint32_t t)
{
    return t == CPU_TYPE_PERFORMANCE ? "P" : t == CPU_TYPE_EFFICIENCY ? "E" : "-";
}

struct core_count {
    uint32_t cores[3], threads[3];   /* by CPU_TYPE_* */
    uint32_t max_smt[3];
};

static void count_cores(const struct cpu_stat *c, uint32_t n, struct core_count *k)
{
    memset(k, 0, sizeof(*k));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = c[i].type < 3 ? c[i].type : 0;
        k->threads[t]++;
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; j++)
            seen = c[j].core_id == c[i].core_id;
        if (!seen)
            k->cores[t]++;
    }
    for (int t = 0; t < 3; t++)
        k->max_smt[t] = k->cores[t] ? (k->threads[t] + k->cores[t] - 1) / k->cores[t] : 0;
}

SH_CMD(lscpu)
{
    bool each = argc > 1 && !strcmp(argv[1], "-e");
    struct sys_info s;
    struct cpu_stat *c = calloc(MAX_CPUS_SH, sizeof(*c));
    uint32_t n = 0;
    if (!c || !get_sysinfo(&s, "lscpu") || !get_cpus(c, &n, "lscpu")) {
        free(c);
        return 1;
    }
    if (each) {
        sh_say("CPU  APIC  TYPE  CORE  SMT  IDLE%%  SWITCHES\n");
        for (uint32_t i = 0; i < n; i++) {
            unsigned idle = permille(c[i].idle_ns, s.uptime_ns);
            sh_say("%3u  %4u  %-4s  %4u  %3u  %3u.%u  %lu\n", c[i].index, c[i].apic_id,
                   type_name(c[i].type), c[i].core_id, c[i].smt_id, idle / 10, idle % 10,
                   (unsigned long)c[i].switches);
        }
        free(c);
        return 0;
    }
    struct core_count k;
    count_cores(c, n, &k);
    uint32_t cores = k.cores[0] + k.cores[1] + k.cores[2];
    sh_say("Model:      %s\n", s.cpu_brand);
    sh_say("Vendor:     %s\n", s.cpu_vendor);
    sh_say("CPUs:       %u online (logical processors)\n", s.cpu_count);
    if (s.flags & SYSINFO_HYBRID) {
        sh_say("Cores:      %u = %u P-cores (%u thread%s each) + %u E-cores (%u thread%s each)\n",
               cores, k.cores[1], k.max_smt[1], k.max_smt[1] == 1 ? "" : "s", k.cores[2],
               k.max_smt[2], k.max_smt[2] == 1 ? "" : "s");
        sh_say("Hybrid:     yes: CPUs");
        for (int t = 1; t <= 2; t++) {
            uint32_t lo = UINT32_MAX, hi = 0;
            for (uint32_t i = 0; i < n; i++)
                if (c[i].type == (uint32_t)t) {
                    lo = c[i].index < lo ? c[i].index : lo;
                    hi = c[i].index > hi ? c[i].index : hi;
                }
            if (lo != UINT32_MAX)
                sh_say(" %u-%u %s", lo, hi, t == 1 ? "P," : "E");
        }
        sh_say("\n");
    } else {
        sh_say("Cores:      %u (%u thread%s each)\n", cores, k.max_smt[0] ? k.max_smt[0] : 1,
               k.max_smt[0] == 1 ? "" : "s");
    }
    sh_say("TSC:        %lu.%03lu MHz\n", (unsigned long)(s.tsc_hz / 1000000),
           (unsigned long)(s.tsc_hz / 1000 % 1000));
    free(c);
    return 0;
}

SH_CMD(free)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    if (!get_sysinfo(&s, "free"))
        return 1;
    char t[24], u[24], f[24], c[24];
    uint64_t total = s.mem_total_pages * 4096, fr = s.mem_free_pages * 4096;
    sh_say("            total        used        free\n");
    sh_say("Mem:   %10s  %10s  %10s\n", sh_human(total, t, sizeof(t)),
           sh_human(total - fr, u, sizeof(u)), sh_human(fr, f, sizeof(f)));
    sh_say("(free includes %s of cached thread stacks)\n",
           sh_human(s.stack_cache_pages * 4096, c, sizeof(c)));
    return 0;
}

/* ---- ps, top --------------------------------------------------------------------------------- */

static const char *state_name(uint32_t s)
{
    static const char *const names[] = { "new", "run", "dying", "dead" };
    return s < 4 ? names[s] : "?";
}

static void fmt_cpu_time(uint64_t ns, char *buf, size_t cap)
{
    uint64_t cs = ns / 10000000;   /* hundredths */
    snprintf(buf, cap, "%lu:%02lu.%02lu", (unsigned long)(cs / 6000), (unsigned long)(cs / 100 % 60),
             (unsigned long)(cs % 100));
}

static int get_procs(struct proc_stat *p, const char *who)
{
    int64_t r = jam_proc_list(sh_root(), p, MAX_PROCS_SH);
    if (r < 0)
        sh_tty("%s: %s\n", who, status_str((status_t)r));
    return (int)r;
}

SH_CMD(ps)
{
    if (argc > 1 && !strcmp(argv[1], "-k")) {
        char line[] = "ps";
        sh_main_command(line);   /* main.c's: the kernel's listing, into the log */
        return 0;
    }
    struct proc_stat *p = calloc(MAX_PROCS_SH, sizeof(*p));
    if (!p)
        return 1;
    int n = get_procs(p, "ps");
    if (n < 0) {
        free(p);
        return 1;
    }
    sh_say("  PID   JOB  THR  STATE    CPU TIME  JOB MEM  NAME\n");
    for (int i = 0; i < n; i++) {
        char t[24], m[24];
        fmt_cpu_time(p[i].cpu_ns, t, sizeof(t));
        sh_say("%5lu %5lu  %3u  %-5s  %10s %8s  %*s%s\n", (unsigned long)p[i].koid,
               (unsigned long)p[i].job_koid, p[i].threads, state_name(p[i].state), t,
               sh_human(p[i].job_pages * 4096, m, sizeof(m)), (int)(p[i].depth * 2), "",
               p[i].name);
    }
    sh_say("%d process%s\n", n, n == 1 ? "" : "es");
    free(p);
    return 0;
}

struct sample {
    uint64_t          t;
    struct cpu_stat  *cpu;
    uint32_t          ncpu;
    struct proc_stat *proc;
    int               nproc;
};

static bool take(struct sample *s)
{
    s->t = (uint64_t)jam_clock_get();
    int64_t r = jam_cpu_stat(sh_root(), 0, s->cpu, MAX_CPUS_SH);
    s->ncpu = r > 0 ? (uint32_t)r : 0;
    s->nproc = get_procs(s->proc, "top");
    return r >= 0 && s->nproc >= 0;
}

static void bar(unsigned pm, char *out, unsigned width)
{
    unsigned fill = (pm * width + 500) / 1000;
    for (unsigned i = 0; i < width; i++)
        out[i] = i < fill ? '|' : ' ';
    out[width] = '\0';
}

static void top_frame(const struct sample *a, const struct sample *b, const struct sys_info *si,
                      bool screen)
{
    uint64_t dt = b->t > a->t ? b->t - a->t : 1;
    if (screen)
        sh_say("\033[2J\033[H");
    /* The header. */
    uint64_t idle_all = 0;
    for (uint32_t i = 0; i < b->ncpu && i < a->ncpu; i++)
        idle_all += b->cpu[i].idle_ns > a->cpu[i].idle_ns ? b->cpu[i].idle_ns - a->cpu[i].idle_ns : 0;
    unsigned busy = 1000 - permille(idle_all, dt * (b->ncpu ? b->ncpu : 1));
    char up[40], used[24], total[24];
    fmt_uptime(si->uptime_ns, up, sizeof(up));
    uint64_t tot = si->mem_total_pages * 4096, fr = si->mem_free_pages * 4096;
    sh_say("top - up %s, %u CPUs, %d processes, CPU %u.%u%%, memory %s used of %s\n", up, b->ncpu,
           b->nproc, busy / 10, busy % 10, sh_human(tot - fr, used, sizeof(used)),
           sh_human(tot, total, sizeof(total)));
    /* Per-CPU bars, two columns. */
    uint32_t rows = (b->ncpu + 1) / 2;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t col = 0; col < 2; col++) {
            uint32_t i = r + col * rows;
            if (i >= b->ncpu || i >= a->ncpu)
                continue;
            uint64_t di = b->cpu[i].idle_ns > a->cpu[i].idle_ns
                              ? b->cpu[i].idle_ns - a->cpu[i].idle_ns : 0;
            unsigned pm = 1000 - permille(di, dt);
            char bb[24];
            bar(pm, bb, 20);
            const char *hi = !screen ? "" : pm >= 700 ? "\033[91m" : pm >= 300 ? "\033[93m"
                                                                              : "\033[92m";
            sh_say("%3u %s [%s%s%s] %3u%%%s", i, type_name(b->cpu[i].type), hi, bb,
                   screen ? "\033[0m" : "", (pm + 5) / 10, col == 0 ? "    " : "");
        }
        sh_say("\n");
    }
    /* Processes by CPU use in this interval. */
    struct row {
        int      i;
        uint64_t d;
    } rows_p[MAX_PROCS_SH];
    int np = 0;
    for (int i = 0; i < b->nproc; i++) {
        uint64_t prev = 0;
        for (int j = 0; j < a->nproc; j++)
            if (a->proc[j].koid == b->proc[i].koid)
                prev = a->proc[j].cpu_ns;
        uint64_t d = b->proc[i].cpu_ns > prev ? b->proc[i].cpu_ns - prev : 0;
        int k = np++;
        while (k > 0 && rows_p[k - 1].d < d) {
            rows_p[k] = rows_p[k - 1];
            k--;
        }
        rows_p[k] = (struct row){ i, d };
    }
    sh_say("%s  PID  NAME                 CPU%%    CPU TIME  THR  JOB MEM%s\n",
           screen ? "\033[1m" : "", screen ? "\033[0m" : "");
    for (int k = 0; k < np && k < 24; k++) {
        const struct proc_stat *p = &b->proc[rows_p[k].i];
        unsigned pm = (unsigned)(rows_p[k].d * 1000 / dt);   /* may pass 100% (threads) */
        char t[24], m[24];
        fmt_cpu_time(p->cpu_ns, t, sizeof(t));
        sh_say("%5lu  %-19s %3u.%u  %10s  %3u %8s\n", (unsigned long)p->koid, p->name, pm / 10,
               pm % 10, t, p->threads, sh_human(p->job_pages * 4096, m, sizeof(m)));
    }
    if (screen)
        sh_say("q or Ctrl+C: quit\n");
}

SH_CMD(top)
{
    uint64_t delay_ms = 1000, frames = 0;   /* 0: until q */
    for (int i = 1; i < argc; i++) {
        uint64_t v;
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            /* seconds, "0.5" allowed */
            const char *s = argv[++i];
            uint64_t whole = 0, frac = 0, scale = 1000;
            const char *dot = strchr(s, '.');
            char w[16];
            size_t wl = dot ? (size_t)(dot - s) : strlen(s);
            if (wl >= sizeof(w))
                wl = sizeof(w) - 1;
            memcpy(w, s, wl);
            w[wl] = '\0';
            if (wl && !sh_parse_u64(w, &whole))
                goto usage;
            for (const char *p = dot ? dot + 1 : ""; *p && scale > 1; p++) {
                if (*p < '0' || *p > '9')
                    goto usage;
                scale /= 10;
                frac += (uint64_t)(*p - '0') * scale;
            }
            delay_ms = whole * 1000 + frac;
            if (delay_ms < 100)
                delay_ms = 100;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc && sh_parse_u64(argv[i + 1], &v)) {
            frames = v;
            i++;
        } else {
            goto usage;
        }
    }
    struct sample s[2];
    for (int k = 0; k < 2; k++) {
        s[k].cpu = calloc(MAX_CPUS_SH, sizeof(struct cpu_stat));
        s[k].proc = calloc(MAX_PROCS_SH, sizeof(struct proc_stat));
    }
    int st = 0;
    if (!s[0].cpu || !s[0].proc || !s[1].cpu || !s[1].proc || !take(&s[0])) {
        st = 1;
    } else {
        bool screen = !sh_piped();
        for (uint64_t f = 0; !frames || f < frames; f++) {
            uint64_t deadline = (uint64_t)jam_clock_get() + delay_ms * SH_MS;
            bool quit = false;
            while (!quit && (uint64_t)jam_clock_get() < deadline) {
                int key = sh_poll_key(deadline);
                quit = key == 'q' || key == 'Q' || key == 3;
            }
            struct sample *a = &s[f % 2], *b = &s[(f + 1) % 2];
            struct sys_info si;
            if (quit || !take(b) || jam_sys_info(sh_root(), &si) != OK)
                break;
            top_frame(a, b, &si, screen);
        }
    }
    for (int k = 0; k < 2; k++) {
        free(s[k].cpu);
        free(s[k].proc);
    }
    return st;
usage:
    sh_tty("usage: top [-d seconds] [-n frames]\n");
    return 2;
}

/* ---- dmesg, history ------------------------------------------------------------------------ */

SH_CMD(dmesg)
{
    (void)argc;
    (void)argv;
    handle_t r;
    status_t st = jam_klog_open(sh_root(), &r);
    if (st != OK) {
        sh_tty("dmesg: %s\n", status_str(st));
        return 1;
    }
    size_t cap = 64 * 1024;
    char *buf = malloc(cap);
    uint64_t first = 0, pos = 0, got = 0, start_pos = 0;
    int64_t n;
    while (buf && got < cap && (n = jam_klog_read(r, pos, buf + got, cap - got, &first)) > 0) {
        if (got == 0)
            start_pos = first;
        got += (uint64_t)n;
        pos = first + (uint64_t)n;
    }
    jam_handle_close(r);
    if (!buf) {
        sh_tty("dmesg: out of memory\n");
        return 1;
    }
    /* Start at a whole line (the ring may have cut the first). */
    size_t start = 0;
    if (start_pos > 0) {
        while (start < got && buf[start] != '\n')
            start++;
        if (start < got)
            start++;
    }
    sh_put(buf + start, got - start);
    free(buf);
    return 0;
}

SH_CMD(history)
{
    (void)argc;
    (void)argv;
    unsigned n = sh_history_count();
    for (unsigned i = n > 32 ? n - 32 : 0; i < n; i++) {
        const char *l = sh_history_at(i);
        if (l)
            sh_say("%5u  %s\n", i + 1, l);
    }
    return 0;
}
