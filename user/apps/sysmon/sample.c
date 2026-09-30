/* sysmon: readings of the kernel's figures (sysmon.h): the real ones
 * through the root resource, and a made-up machine for tests and
 * screenshots (so the layout for the PC's 28 CPUs can be looked at under
 * QEMU, which has neither that many nor P- and E-cores). */
#include "sysmon.h"

status_t sample_take(handle_t root, struct sample *s)
{
    s->t = now();
    status_t st = jam_sys_info(root, &s->si);
    if (st != OK)
        return st;
    int64_t n = jam_cpu_stat(root, 0, s->cpu, MAX_CPUS);
    if (n < 0)
        return (status_t)n;
    s->ncpu = (uint32_t)n;
    n = jam_proc_list(root, s->proc, MAX_PROCS);
    if (n < 0)
        return (status_t)n;
    s->nproc = (uint32_t)n;
    return OK;
}

/* ---- the made-up machine ---- */

static const char *const fake_names[] = {
    "init", "console", "devmgr", "usb-bus", "hid-7:0", "hid-8:0", "serialin", "shell", "fat",
    "logd", "sysmon", "fractal",
};
#define NFAKE ((uint32_t)(sizeof(fake_names) / sizeof(fake_names[0])))

static void fake_setup(struct sample *s, uint32_t ncpu)
{
    memset(s, 0, sizeof(*s));
    s->ncpu = ncpu < 1 ? 1 : ncpu > MAX_CPUS ? MAX_CPUS : ncpu;
    /* From 8 CPUs a hybrid part: 4/7 of them P threads (two a core), the
     * rest E-cores, which is 16 + 12 for 28. */
    uint32_t p = s->ncpu >= 8 ? (s->ncpu * 4 / 7) & ~1u : 0;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        struct cpu_stat *c = &s->cpu[i];
        c->index = c->apic_id = i;
        c->online = 1;
        c->type = !p ? CPU_TYPE_UNKNOWN : i < p ? CPU_TYPE_PERFORMANCE : CPU_TYPE_EFFICIENCY;
        c->core_id = i < p ? i / 2 : i;
        c->smt_id = i < p ? i % 2 : 0;
    }
    s->nproc = NFAKE;
    for (uint32_t i = 0; i < NFAKE; i++) {
        struct proc_stat *q = &s->proc[i];
        q->koid = 1000 + 7 * i;
        q->threads = i == NFAKE - 1 ? s->ncpu : 1 + i % 3;
        q->job_pages = 40 + 97 * i * i;
        q->state = PROCESS_RUNNING;
        snprintf(q->name, sizeof(q->name), "%s", fake_names[i]);
    }
    snprintf(s->si.version, sizeof(s->si.version), "fake");
    snprintf(s->si.cpu_brand, sizeof(s->si.cpu_brand), "A made-up %u-thread CPU", s->ncpu);
    s->si.cpu_count = s->ncpu;
    s->si.flags = p ? SYSINFO_HYBRID : 0;
    s->si.mem_total_pages = 32ull << 18;   /* 32 GiB */
}

void sample_fake(struct sample *s, uint32_t ncpu, uint64_t t, uint64_t *rng)
{
    if (!s->t)
        fake_setup(s, ncpu);
    uint64_t dt = t > s->t && s->t ? t - s->t : 0, busy_all = 0;
    s->t = t;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        /* A load per CPU that holds its level for a while: a slow wave per
         * CPU plus noise, the E threads quieter. */
        uint64_t wave = (t / (NS_PER_S / 4) + i * 37) % 64;
        uint64_t load = (wave < 32 ? wave : 63 - wave) * 28 + rng_next(rng) % 120;
        if (s->cpu[i].type == CPU_TYPE_EFFICIENCY)
            load /= 3;
        load = load > 1000 ? 1000 : load;
        s->cpu[i].idle_ns += dt * (1000 - load) / 1000;
        s->cpu[i].switches += dt / 100000 * (load + 20) / 100;
        busy_all += dt * load / 1000;
    }
    /* The processes share the busy time: most of it to the last one. */
    for (uint32_t i = 0; i < NFAKE; i++)
        s->proc[i].cpu_ns += i == NFAKE - 1 ? busy_all * 3 / 4 : busy_all / 4 / (NFAKE - 1);
    s->si.uptime_ns += dt;
    s->si.mem_free_pages = s->si.mem_total_pages * (600 + rng_next(rng) % 20) / 1000;
}
