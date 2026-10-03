/* linuxbench: the CPUs. The topology comes from sysfs: which CPUs are
 * online, each CPU's core (the first CPU of its thread_siblings_list),
 * and on a hybrid CPU which are P-cores (/sys/devices/cpu_core/cpus) and
 * which E-cores (/sys/devices/cpu_atom/cpus). P, P2, HT and E are chosen
 * exactly as Jam OS's bench chooses them (kernel/test/bench.c:
 * pick_cpus): P is the first P-core thread that is not CPU 0 and not on
 * CPU 0's core, HT its sibling, P2 a P-core thread of another core (not
 * CPU 0's), E the first E-core. Linux numbers the CPUs in the firmware's
 * order, as Jam OS does, so on the PC they should be the same numbers:
 * the header prints them (and their APIC ids) to check. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cpu_p = -1, cpu_p2 = -1, cpu_ht = -1, cpu_e = -1;
int ncpus_online;
bool fifo_ok = true;

static bool online[MAX_CPUS];
static int core[MAX_CPUS];         /* the first CPU of its core */
static bool is_e[MAX_CPUS];        /* an E-core (hybrid only) */
static bool hybrid;

/* A CPU list ("0-15,17,20-23") into set[]; false if unreadable. */
static bool read_list(const char *path, bool set[MAX_CPUS])
{
    char buf[1024];
    if (!read_line(path, buf, sizeof(buf)) || !buf[0])
        return false;
    for (char *s = buf; *s;) {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s)
            return false;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long i = a; i <= b && i >= 0 && i < MAX_CPUS; i++)
            set[i] = true;
        s = *end == ',' ? end + 1 : end;
        if (*s == '\n')
            break;
    }
    return true;
}

static void read_topology(void)
{
    if (!read_list("/sys/devices/system/cpu/online", online))
        online[0] = true;
    bool p_set[MAX_CPUS] = { false };
    hybrid = read_list("/sys/devices/cpu_atom/cpus", is_e) &&
             read_list("/sys/devices/cpu_core/cpus", p_set);
    if (!hybrid)
        memset(is_e, 0, sizeof(is_e));
    for (int i = 0; i < MAX_CPUS; i++) {
        core[i] = i;
        if (!online[i])
            continue;
        ncpus_online++;
        char path[128], buf[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
                 i);
        if (read_line(path, buf, sizeof(buf)))
            core[i] = atoi(buf);
    }
}

void pick_cpus(void)
{
    read_topology();
    /* The big cores: the P-cores of a hybrid CPU, else every core (is_e
     * is all false). */
    for (int i = 1; i < MAX_CPUS && cpu_p < 0; i++)
        if (online[i] && !is_e[i] && core[i] != core[0])
            cpu_p = i;
    for (int i = 1; i < MAX_CPUS && cpu_p < 0; i++)
        if (online[i] && !is_e[i])
            cpu_p = i;   /* no SMT, or only one core */
    if (cpu_p < 0) {
        cpu_p = 0;       /* one CPU: the cross-CPU lines are skipped */
        return;
    }
    for (int i = 1; i < MAX_CPUS; i++) {
        if (!online[i] || i == cpu_p)
            continue;
        if (cpu_ht < 0 && core[i] == core[cpu_p])
            cpu_ht = i;
        else if (cpu_p2 < 0 && !is_e[i] && core[i] != core[cpu_p] &&
                 core[i] != core[0])
            cpu_p2 = i;
        if (hybrid && cpu_e < 0 && is_e[i])
            cpu_e = i;
    }
}

const char *kind(int cpu)
{
    if (cpu == cpu_p)
        return "P";
    if (cpu == cpu_p2)
        return "P2";
    if (cpu == cpu_ht)
        return "HT";
    if (cpu == cpu_e)
        return "E";
    return "?";
}

bool cpu_online(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS && online[cpu];
}

int core_of(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS ? core[cpu] : -1;
}

bool cpu_is_e(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS && is_e[cpu];
}

void pin_self_set(const cpu_set_t *set)
{
    struct sched_param sp = { .sched_priority = FIFO_PRIO };
    if (sched_setaffinity(0, sizeof(*set), set) != 0)
        perror("linuxbench: sched_setaffinity");
    /* pid 0 is the calling thread, not the whole process. */
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        __atomic_store_n(&fifo_ok, false, __ATOMIC_RELAXED);
}

void pin_self(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pin_self_set(&set);
}

struct start {
    void *(*fn)(void *);
    void *arg;
    cpu_set_t set;
};

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    pin_self_set(&s.set);
    return s.fn(s.arg);
}

pthread_t start_set(const cpu_set_t *set, void *(*fn)(void *), void *arg)
{
    struct start *s = malloc(sizeof(*s));
    pthread_t t;
    if (!s) {
        perror("linuxbench: malloc");
        exit(1);
    }
    s->fn = fn;
    s->arg = arg;
    s->set = *set;
    if (pthread_create(&t, NULL, trampoline, s) != 0) {
        perror("linuxbench: pthread_create");
        exit(1);
    }
    return t;
}

pthread_t start_on(int cpu, void *(*fn)(void *), void *arg)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return start_set(&set, fn, arg);
}

void run_on(int cpu, void *(*fn)(void *), void *arg)
{
    pthread_join(start_on(cpu, fn, arg), NULL);
}
