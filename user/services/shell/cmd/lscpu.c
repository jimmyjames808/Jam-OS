/* lscpu: the CPU's model, cores by type (P and E on a hybrid part) and
 * threads; -e one line per CPU. */
#include "sh.h"

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

static void each_cpu(const struct cpu_stat *c, uint32_t n, const struct sys_info *s)
{
    sh_say("CPU  APIC  TYPE  CORE  SMT  IDLE%%  SWITCHES\n");
    for (uint32_t i = 0; i < n; i++) {
        unsigned idle = sh_permille(c[i].idle_ns, s->uptime_ns);
        sh_say("%3u  %4u  %-4s  %4u  %3u  %3u.%u  %lu\n", c[i].index, c[i].apic_id,
               sh_cpu_type(c[i].type), c[i].core_id, c[i].smt_id, idle / 10, idle % 10,
               (unsigned long)c[i].switches);
    }
}

/* "Hybrid:     yes: CPUs 0-15 P, 16-27 E" */
static void hybrid_ranges(const struct cpu_stat *c, uint32_t n)
{
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
}

static void summary(const struct cpu_stat *c, uint32_t n, const struct sys_info *s)
{
    struct core_count k;
    count_cores(c, n, &k);
    uint32_t cores = k.cores[0] + k.cores[1] + k.cores[2];
    sh_say("Model:      %s\n", s->cpu_brand);
    sh_say("Vendor:     %s\n", s->cpu_vendor);
    sh_say("CPUs:       %u online (logical processors)\n", s->cpu_count);
    if (s->flags & SYSINFO_HYBRID) {
        sh_say("Cores:      %u = %u P-cores (%u thread%s each) + %u E-cores (%u thread%s each)\n",
               cores, k.cores[1], k.max_smt[1], k.max_smt[1] == 1 ? "" : "s", k.cores[2],
               k.max_smt[2], k.max_smt[2] == 1 ? "" : "s");
        hybrid_ranges(c, n);
    } else {
        sh_say("Cores:      %u (%u thread%s each)\n", cores, k.max_smt[0] ? k.max_smt[0] : 1,
               k.max_smt[0] == 1 ? "" : "s");
    }
    sh_say("TSC:        %lu.%03lu MHz\n", (unsigned long)(s->tsc_hz / 1000000),
           (unsigned long)(s->tsc_hz / 1000 % 1000));
}

SH_CMD(lscpu)
{
    bool each = argc > 1 && !strcmp(argv[1], "-e");
    struct sys_info s;
    struct cpu_stat *c = calloc(SH_MAX_CPUS, sizeof(*c));
    uint32_t n = 0;
    if (!c || !sh_sysinfo(&s, "lscpu") || !sh_cpus(c, &n, "lscpu")) {
        free(c);
        return 1;
    }
    if (each)
        each_cpu(c, n, &s);
    else
        summary(c, n, &s);
    free(c);
    return 0;
}
