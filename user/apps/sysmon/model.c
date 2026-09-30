/* sysmon: the figures shown, from two readings of the kernel's counters
 * (sysmon.h). The kernel counts since boot (a CPU's idle time and context
 * switches, a process's CPU time); what the screen shows is how much each
 * grew between two readings, over the time between them. Nothing here
 * touches the screen or makes a system call, so the self-test feeds it
 * readings made by hand. */
#include "sysmon.h"

void hist_push(struct history *h, uint32_t v)
{
    h->v[h->at] = v;
    h->at = (h->at + 1) % HIST;
    if (h->n < HIST)
        h->n++;
}

uint32_t hist_max(const struct history *h)
{
    uint32_t max = 0;
    for (int k = 0; k < h->n; k++)
        max = hist_back(h, k) > max ? hist_back(h, k) : max;
    return max;
}

/* Which group a CPU's tile is in: P threads, E threads, the rest. */
static unsigned group_of(uint32_t type)
{
    return type == CPU_TYPE_PERFORMANCE ? 0 : type == CPU_TYPE_EFFICIENCY ? 1 : 2;
}

void model_init(struct model *m, const struct sample *s)
{
    memset(m, 0, sizeof(*m));
    for (unsigned g = 0; g < NGROUPS; g++)
        for (uint32_t i = 0; i < s->ncpu && i < MAX_CPUS; i++) {
            if (group_of(s->cpu[i].type) != g)
                continue;
            m->cpu[m->ncpu].index = i;
            m->cpu[m->ncpu++].type = s->cpu[i].type;
            m->group[g]++;
        }
    snprintf(m->brand, sizeof(m->brand), "%s", s->si.cpu_brand);
    snprintf(m->version, sizeof(m->version), "%s", s->si.version);
}

/* part / whole in thousandths, 0..1000. */
static uint32_t permille(uint64_t part, uint64_t whole)
{
    if (!whole)
        return 0;
    uint64_t v = part * 1000 / whole;
    return v > 1000 ? 1000 : (uint32_t)v;
}

/* How much a counter grew (0 if it went back: a CPU or process replaced). */
static uint64_t grew(uint64_t before, uint64_t after)
{
    return after > before ? after - before : 0;
}

static void update_cpus(struct model *m, const struct sample *a, const struct sample *b,
                        uint64_t dt)
{
    uint64_t idle_all = 0, switches = 0;
    uint32_t counted = 0;
    for (uint32_t k = 0; k < m->ncpu; k++) {
        struct cpu_view *c = &m->cpu[k];
        if (c->index >= a->ncpu || c->index >= b->ncpu)
            continue;
        uint64_t idle = grew(a->cpu[c->index].idle_ns, b->cpu[c->index].idle_ns);
        idle = idle > dt ? dt : idle;
        c->pm = 1000 - permille(idle, dt);
        hist_push(&c->h, c->pm);
        idle_all += idle;
        switches += grew(a->cpu[c->index].switches, b->cpu[c->index].switches);
        counted++;
    }
    m->total_pm = counted ? 1000 - permille(idle_all, dt * counted) : 0;
    hist_push(&m->total, m->total_pm);
    m->switches_s = switches * NS_PER_S / dt;
    hist_push(&m->switches, m->switches_s > UINT32_MAX ? UINT32_MAX : (uint32_t)m->switches_s);
}

/* p comes before q in the list: busier now, or as busy with more CPU time ever. */
static bool busier(const struct proc_view *p, const struct proc_view *q)
{
    return p->pm != q->pm ? p->pm > q->pm : p->cpu_ns > q->cpu_ns;
}

/* v into the list of the TOP_MAX busiest, kept in order. */
static void top_insert(struct model *m, const struct proc_view *v)
{
    if (m->ntop == TOP_MAX && !busier(v, &m->top[TOP_MAX - 1]))
        return;
    int k = m->ntop < TOP_MAX ? m->ntop++ : TOP_MAX - 1;   /* the slot that opens */
    for (; k > 0 && busier(v, &m->top[k - 1]); k--)
        m->top[k] = m->top[k - 1];
    m->top[k] = *v;
}

static void update_procs(struct model *m, const struct sample *a, const struct sample *b,
                         uint64_t dt)
{
    m->ntop = 0;
    m->nthreads = 0;
    m->nproc = b->nproc;
    memset(m->top, 0, sizeof(m->top));
    for (uint32_t i = 0; i < b->nproc; i++) {
        const struct proc_stat *p = &b->proc[i];
        uint64_t before = p->cpu_ns;   /* a process not in a: no time in this interval */
        for (uint32_t j = 0; j < a->nproc; j++)
            if (a->proc[j].koid == p->koid)
                before = a->proc[j].cpu_ns;
        struct proc_view v = {
            .koid = p->koid, .cpu_ns = p->cpu_ns, .mem = p->job_pages * 4096,
            .pm = (uint32_t)(grew(before, p->cpu_ns) * 1000 / dt), .threads = p->threads,
        };
        snprintf(v.name, sizeof(v.name), "%s", p->name);
        m->nthreads += p->threads;
        top_insert(m, &v);
    }
}

void model_update(struct model *m, const struct sample *a, const struct sample *b)
{
    /* No time between them (the first frame shows one reading): the
     * loads stay as they are, nothing joins the history. */
    uint64_t dt = b->t > a->t ? b->t - a->t : 0;
    if (dt)
        update_cpus(m, a, b, dt);
    update_procs(m, a, b, dt ? dt : 1);
    m->mem_total = b->si.mem_total_pages * 4096;
    m->mem_used = grew(b->si.mem_free_pages, b->si.mem_total_pages) * 4096;
    m->uptime_ns = b->si.uptime_ns;
}

char *fmt_bytes(char *buf, size_t cap, uint64_t bytes)
{
    static const char *const unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    unsigned k = 0;
    uint64_t scale = 1;
    for (; k < 4 && bytes >= scale * 1024; k++)
        scale *= 1024;
    uint64_t tenths = (bytes * 10 + scale / 2) / scale;
    if (!k || tenths >= 1000)   /* whole numbers for bytes and from 100 up */
        snprintf(buf, cap, "%lu %s", (unsigned long)((tenths + 5) / 10), unit[k]);
    else
        snprintf(buf, cap, "%lu.%lu %s", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10), unit[k]);
    return buf;
}

char *fmt_count(char *buf, size_t cap, uint64_t n)
{
    if (n < 100000)
        return commas(buf, cap, n);
    if (n < 1000000)
        snprintf(buf, cap, "%lu k", (unsigned long)(n / 1000));
    else if (n < 100000000)
        snprintf(buf, cap, "%lu.%02lu M", (unsigned long)(n / 1000000),
                 (unsigned long)(n / 10000 % 100));
    else
        snprintf(buf, cap, "%lu M", (unsigned long)(n / 1000000));
    return buf;
}

char *fmt_uptime(char *buf, size_t cap, uint64_t ns)
{
    uint64_t s = ns / NS_PER_S, d = s / 86400;
    if (d)
        snprintf(buf, cap, "%lu d %02lu:%02lu", (unsigned long)d, (unsigned long)(s / 3600 % 24),
                 (unsigned long)(s / 60 % 60));
    else
        snprintf(buf, cap, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    return buf;
}

char *fmt_cpu_time(char *buf, size_t cap, uint64_t ns)
{
    uint64_t cs = ns / (NS_PER_S / 100);
    snprintf(buf, cap, "%lu:%02lu.%02lu", (unsigned long)(cs / 6000),
             (unsigned long)(cs / 100 % 60), (unsigned long)(cs % 100));
    return buf;
}
