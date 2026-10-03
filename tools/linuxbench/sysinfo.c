/* linuxbench: what the numbers were measured on, printed first so BENCH.md
 * can say it beside them: the CPU and its microcode, the kernel and its
 * command line, every file in /sys/devices/system/cpu/vulnerabilities
 * (the mitigations Linux has on), the cpufreq governor and energy
 * preference, the idle driver, what else runs (load, a desktop session),
 * and whether the threads got SCHED_FIFO.
 *
 * The plan's method sets the `performance` governor and energy
 * preference on every CPU (sys_performance); Jam OS leaves the firmware's
 * P-state choice as it is, and BENCH.md says so next to the numbers. */
#include "lb.h"   /* first: it defines _GNU_SOURCE before any system header */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

bool read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok)
        return false;
    buf[strcspn(buf, "\n")] = 0;
    return true;
}

static bool write_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    bool ok = fputs(s, f) >= 0;
    return fclose(f) == 0 && ok;
}

const char *sys_boot_tag(void)
{
    char cmd[4096];
    if (read_line("/proc/cmdline", cmd, sizeof(cmd)) && strstr(cmd, "mitigations=off"))
        return "mitigations-off";
    return "default";
}

/* "<key>\t: value" of the first processor in /proc/cpuinfo, or of
 * processor `cpu` (>= 0). */
static bool cpuinfo(int cpu, const char *key, char *out, size_t n)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    int at = -1;
    bool found = false;
    if (!f)
        return false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "processor", 9))
            at = atoi(strchr(line, ':') ? strchr(line, ':') + 1 : line);
        if ((cpu < 0 || at == cpu) && !strncmp(line, key, strlen(key)) && strchr(line, ':')) {
            char *v = strchr(line, ':') + 1;
            v += strspn(v, " \t");
            v[strcspn(v, "\n")] = 0;
            snprintf(out, n, "%s", v);
            found = true;
        }
    }
    fclose(f);
    return found;
}

static void cpufreq_of(int cpu, const char *file, char *out, size_t n)
{
    char path[160];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/%s", cpu, file);
    if (!read_line(path, out, n))
        snprintf(out, n, "?");
}

void sys_performance(bool as_is)
{
    char gov[64], epp[64], drv[64];
    cpufreq_of(cpu_p, "scaling_governor", gov, sizeof(gov));
    cpufreq_of(cpu_p, "energy_performance_preference", epp, sizeof(epp));
    cpufreq_of(cpu_p, "scaling_driver", drv, sizeof(drv));
    say("bench: cpufreq as it came (P): driver %s, governor %s, energy preference %s\n", drv, gov,
        epp);
    if (as_is)
        return;
    unsigned set = 0, tried = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!cpu_online(i))
            continue;
        char path[160];
        tried++;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        set += write_str(path, "performance");
        /* With intel_pstate's performance governor the preference is
         * already "performance" and the write is refused: that's fine. */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", i);
        (void)write_str(path, "performance");
    }
    cpufreq_of(cpu_p, "scaling_governor", gov, sizeof(gov));
    cpufreq_of(cpu_p, "energy_performance_preference", epp, sizeof(epp));
    say("bench: cpufreq for the run: governor %s, energy preference %s (set on %u of %u CPUs)\n",
        gov, epp, set, tried);
}

static void vulnerabilities(void)
{
    const char *dir = "/sys/devices/system/cpu/vulnerabilities";
    struct dirent **names;
    int n = scandir(dir, &names, NULL, alphasort);
    if (n < 0) {
        say("bench: vulnerabilities: %s unreadable\n", dir);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (names[i]->d_name[0] != '.') {
            char path[512], v[512];
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]->d_name);
            if (!read_line(path, v, sizeof(v)))
                snprintf(v, sizeof(v), "?");
            say("bench: vulnerability %-28s %s\n", names[i]->d_name, v);
        }
        free(names[i]);
    }
    free(names);
}

/* A graphical session would run beside the benchmark: say so. */
static const char *desktop(void)
{
    static const char *const known[] = { "gnome-shell", "Xorg", "Xwayland", "kwin_wayland" };
    DIR *d = opendir("/proc");
    struct dirent *e;
    const char *found = NULL;
    while (d && !found && (e = readdir(d))) {
        char path[300], comm[64];
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        if (!read_line(path, comm, sizeof(comm)))
            continue;
        for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            if (!strcmp(comm, known[k]))
                found = known[k];
    }
    if (d)
        closedir(d);
    return found;
}

static void cpu_lines(void)
{
    char model[256] = "?", ucode[64] = "?", apic[4][32];
    cpuinfo(-1, "model name", model, sizeof(model));
    cpuinfo(-1, "microcode", ucode, sizeof(ucode));
    int which[4] = { cpu_p, cpu_p2, cpu_ht, cpu_e };
    for (int i = 0; i < 4; i++)
        if (which[i] < 0 || !cpuinfo(which[i], "apicid", apic[i], sizeof(apic[i])))
            snprintf(apic[i], sizeof(apic[i]), "-");
    say("bench: %s, microcode %s, TSC %llu MHz (measured), %d CPUs online\n", model, ucode,
        (unsigned long long)(tsc_hz / 1000000), ncpus_online);
    say("bench: P=cpu%d P2=cpu%d HT=cpu%d E=cpu%d (APIC ids %s %s %s %s; Jam OS's bench names "
        "its CPUs the same way)\n", cpu_p, cpu_p2, cpu_ht, cpu_e, apic[0], apic[1], apic[2],
        apic[3]);
}

void sys_header(void)
{
    struct utsname u;
    char line[4096];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(line, sizeof(line), "%Y-%m-%d %H:%M UTC", &tm);
    say("bench: linuxbench, %s, run as the boot '%s'\n", line, sys_boot_tag());
    cpu_lines();
    if (uname(&u) == 0)
        say("bench: %s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    if (read_line("/proc/cmdline", line, sizeof(line)))
        say("bench: command line: %s\n", line);
    vulnerabilities();
    char drv[64] = "?", gov[64] = "?", rt[64] = "?", load[128] = "?";
    read_line("/sys/devices/system/cpu/cpuidle/current_driver", drv, sizeof(drv));
    if (!read_line("/sys/devices/system/cpu/cpuidle/current_governor_ro", gov, sizeof(gov)))
        read_line("/sys/devices/system/cpu/cpuidle/current_governor", gov, sizeof(gov));
    read_line("/proc/sys/kernel/sched_rt_runtime_us", rt, sizeof(rt));
    read_line("/proc/loadavg", load, sizeof(load));
    const char *gui = desktop();
    say("bench: cpuidle driver %s, governor %s; sched_rt_runtime_us %s; load %s; desktop %s\n",
        drv, gov, rt, load, gui ? gui : "none seen");
}
