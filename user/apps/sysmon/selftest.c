/* sysmon: the self-test (`run sysmon --selftest`): the figures from
 * readings made by hand, the made-up machine, and the layout for screens
 * and CPU counts QEMU doesn't have. It needs no handle. */
#include "sysmon.h"

static struct sample a, b;
static struct model m;

/* Two readings a second apart of 3 CPUs (E, P, P) and 3 processes. */
static void readings(void)
{
    memset(&a, 0, sizeof(a));
    a.t = 10 * NS_PER_S;
    a.ncpu = 3;
    a.cpu[0].type = CPU_TYPE_EFFICIENCY;
    a.cpu[1].type = a.cpu[2].type = CPU_TYPE_PERFORMANCE;
    a.nproc = 3;
    for (uint32_t i = 0; i < 3; i++) {
        a.cpu[i].index = i;
        a.cpu[i].idle_ns = 5 * NS_PER_S;
        a.cpu[i].switches = 1000;
        a.proc[i].koid = 100 + i;
        a.proc[i].threads = i + 1;
        a.proc[i].cpu_ns = (3 - i) * NS_PER_S;
        a.proc[i].job_pages = 256;
        snprintf(a.proc[i].name, sizeof(a.proc[i].name), "p%u", i);
    }
    a.si.mem_total_pages = 1u << 18;   /* 1 GiB */
    a.si.mem_free_pages = 3u << 16;    /* 768 MiB */
    a.si.uptime_ns = a.t;
    b = a;
    b.t = b.si.uptime_ns = 11 * NS_PER_S;
    b.cpu[0].idle_ns += NS_PER_S;            /* idle throughout */
    b.cpu[1].idle_ns += NS_PER_S / 4;        /* 75% busy */
    b.cpu[2].idle_ns += 0;                   /* flat out */
    b.cpu[0].switches += 50;
    b.cpu[1].switches += 250;
    b.proc[2].cpu_ns += NS_PER_S * 3 / 2;    /* one and a half CPUs */
    b.proc[0].cpu_ns += NS_PER_S / 4;
}

static void test_cpus(void)
{
    readings();
    model_init(&m, &a);
    fun_check(m.ncpu == 3 && m.group[0] == 2 && m.group[1] == 1 && m.group[2] == 0 &&
                  m.cpu[0].index == 1 && m.cpu[1].index == 2 && m.cpu[2].index == 0,
              "tiles: the P threads first, then the E threads");
    model_update(&m, &a, &a);
    fun_check(m.total.n == 0 && m.total_pm == 0 && m.ntop == 3,
              "one reading alone: no loads yet, but the processes are listed");
    model_update(&m, &a, &b);
    fun_check(m.cpu[0].pm == 750 && m.cpu[1].pm == 1000 && m.cpu[2].pm == 0,
              "a CPU's load is the time it was not idle: 75%, 100%, 0%");
    fun_check(m.total_pm == 584 && m.switches_s == 300, "all CPUs: 58.4%; 300 switches a second");
    fun_check(m.mem_total == 1ull << 30 && m.mem_used == 1ull << 28 && m.uptime_ns == b.t,
              "memory used is total less free; the uptime is the kernel's");
    b.cpu[1].idle_ns = 0;   /* a counter that went back is no load of 2^64 */
    model_update(&m, &a, &b);
    fun_check(m.cpu[0].pm == 1000 && m.total_pm <= 1000, "a counter going back stays in range");
}

static void test_procs(void)
{
    readings();
    model_init(&m, &a);
    model_update(&m, &a, &b);
    bool ok = m.ntop == 3 && m.nproc == 3 && m.nthreads == 6;
    ok &= m.top[0].koid == 102 && m.top[0].pm == 1500 && m.top[0].threads == 3;
    ok &= m.top[1].koid == 100 && m.top[1].pm == 250 && m.top[2].koid == 101 && m.top[2].pm == 0;
    fun_check(ok, "processes: the busiest in the interval first (150% = a CPU and a half)");
    model_update(&m, &b, &b);
    fun_check(m.top[0].koid == 100 && m.top[1].koid == 102 && m.top[2].koid == 101,
              "  ... and among equals the one with the most CPU time ever");
    /* More processes than rows: the busiest TOP_MAX stay, in order. */
    for (uint32_t i = 0; i < 40; i++) {
        a.proc[i].koid = b.proc[i].koid = 500 + i;
        a.proc[i].cpu_ns = 0;
        b.proc[i].cpu_ns = (uint64_t)((i * 7) % 40) * NS_PER_MS;
    }
    a.nproc = b.nproc = 40;
    model_update(&m, &a, &b);
    ok = m.ntop == TOP_MAX && m.top[0].pm == 39;
    for (int i = 1; i < m.ntop; i++)
        ok &= m.top[i].pm == m.top[i - 1].pm - 1;
    fun_check(ok, "40 processes: the 16 busiest are kept, in order");
}

static void test_history(void)
{
    struct history h = { 0 };
    for (uint32_t v = 1; v <= HIST + 30; v++)
        hist_push(&h, v);
    fun_check(h.n == HIST && hist_back(&h, 0) == HIST + 30 && hist_back(&h, HIST - 1) == 31 &&
                  hist_max(&h) == HIST + 30,
              "history: a ring of the last 120 readings, the newest first");
}

static void test_format(void)
{
    char x[32], y[32], z[32];
    fun_check(!strcmp(fmt_bytes(x, sizeof(x), 1536), "1.5 KiB") &&
                  !strcmp(fmt_bytes(y, sizeof(y), 412ull << 20), "412 MiB") &&
                  !strcmp(fmt_bytes(z, sizeof(z), 900), "900 B"),
              "sizes: 900 B, 1.5 KiB, 412 MiB");
    fun_check(!strcmp(fmt_uptime(x, sizeof(x), 3723 * NS_PER_S), "1:02:03") &&
                  !strcmp(fmt_uptime(y, sizeof(y), (2 * 86400 + 3 * 3600 + 240) * NS_PER_S),
                          "2 d 03:04") &&
                  !strcmp(fmt_cpu_time(z, sizeof(z), 83450 * NS_PER_MS), "1:23.45"),
              "times: 1:02:03, 2 d 03:04, 1:23.45");
    fun_check(!strcmp(fmt_count(x, sizeof(x), 12345), "12,345") &&
                  !strcmp(fmt_count(y, sizeof(y), 123456), "123 k") &&
                  !strcmp(fmt_count(z, sizeof(z), 1234567), "1.23 M"),
              "counts stay short: 12,345, 123 k, 1.23 M");
}

/* The made-up machine: the PC's topology, and loads in range over time. */
static void test_fake(void)
{
    static struct sample s0, s1;
    uint64_t rng = 9;
    memset(&s0, 0, sizeof(s0));
    sample_fake(&s0, 28, NS_PER_S, &rng);
    model_init(&m, &s0);
    fun_check(m.ncpu == 28 && m.group[0] == 16 && m.group[1] == 12 && m.group[2] == 0,
              "the made-up machine with 28 CPUs is the PC's: 16 P + 12 E threads");
    bool ok = true;
    uint32_t busy = 0;
    for (uint64_t t = 3; t < 3 + 2 * HIST; t++) {
        s1 = s0;
        sample_fake(&s1, 28, t * NS_PER_S / 2, &rng);
        model_update(&m, &s0, &s1);
        for (uint32_t k = 0; k < m.ncpu; k++)
            ok &= m.cpu[k].pm <= 1000;
        busy += m.total_pm > 0;
        s0 = s1;
    }
    fun_check(ok && busy > HIST && m.total.n == HIST && m.ntop == 12 &&
                  !strcmp(m.top[0].name, "fractal"),
              "  ... 240 readings: loads in range, history full, its busiest process first");
}

static bool apart(const struct rect *p, const struct rect *q)
{
    return p->x + p->w <= q->x || q->x + q->w <= p->x || p->y + p->h <= q->y || q->y + q->h <= p->y;
}

/* Every box of the layout, in one list. */
static int boxes(const struct layout *l, uint32_t ncpu, struct rect *out)
{
    int n = 0;
    out[n++] = l->title;
    out[n++] = l->table;
    for (int i = 0; i < NCARDS; i++)
        out[n++] = l->card[i];
    for (int g = 0; g < NGROUPS; g++)
        if (l->label[g].w)
            out[n++] = l->label[g];
    for (uint32_t k = 0; k < ncpu; k++)
        out[n++] = l->cpu[k];
    return n;
}

/* One screen and machine: everything on the screen, nothing overlapping,
 * at least five process rows. */
static void test_layout(int w, int h, uint32_t p, uint32_t e, uint32_t other)
{
    static struct layout l;
    static struct rect r[MAX_CPUS + NCARDS + NGROUPS + 2];
    const uint32_t group[NGROUPS] = { p, e, other };
    char what[96];
    layout_make(&l, w, h, h > 1100 ? 2 : 1, group);
    int n = boxes(&l, p + e + other, r);
    bool ok = l.rows >= 5 && l.rows <= TOP_MAX;
    for (int i = 0; i < n; i++) {
        ok &= r[i].x >= 0 && r[i].y >= 0 && r[i].w > 0 && r[i].h > 0;
        ok &= r[i].x + r[i].w <= w && r[i].y + r[i].h <= h;
        for (int j = 0; j < i; j++)
            ok &= apart(&r[i], &r[j]);
    }
    snprintf(what, sizeof(what), "layout %dx%d, %u P + %u E + %u CPUs: fits, %d process rows", w,
             h, p, e, other, l.rows);
    fun_check(ok, what);
}

int sysmon_selftest(void)
{
    fun_selftest_begin("sysmon", 74);
    test_cpus();
    test_procs();
    test_history();
    test_format();
    test_fake();
    test_layout(1280, 800, 16, 12, 0);
    test_layout(2560, 1440, 16, 12, 0);
    test_layout(1280, 800, 0, 0, 4);
    test_layout(2560, 1440, 0, 0, 4);
    test_layout(1920, 1080, 0, 0, 1);
    test_layout(1280, 800, 0, 0, 64);
    test_layout(1280, 800, 36, 28, 0);
    return fun_selftest_end();
}
