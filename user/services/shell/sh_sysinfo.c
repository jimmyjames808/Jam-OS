/* System, CPU and process figures for uname, uptime, lscpu, free, ps and
 * top: syscalls 130-132 (sys_info, cpu_stat, proc_list; RIGHT_READ on the
 * root resource) and how the commands show them. */
#include "sh.h"

bool sh_sysinfo(struct sys_info *s, const char *who)
{
    status_t st = jam_sys_info(sh_root(), s);
    if (st != OK)
        sh_tty("%s: %s\n", who, status_str(st));
    return st == OK;
}

bool sh_cpus(struct cpu_stat *c, uint32_t *n, const char *who)
{
    int64_t r = jam_cpu_stat(sh_root(), 0, c, SH_MAX_CPUS);
    if (r < 0) {
        sh_tty("%s: %s\n", who, status_str((status_t)r));
        return false;
    }
    *n = (uint32_t)r;
    return true;
}

int sh_procs(struct proc_stat *p, const char *who)
{
    int64_t r = jam_proc_list(sh_root(), p, SH_MAX_PROCS);
    if (r < 0)
        sh_tty("%s: %s\n", who, status_str((status_t)r));
    return (int)r;
}

unsigned sh_permille(uint64_t part, uint64_t whole)
{
    if (!whole)
        return 0;
    uint64_t v = part * 1000 / whole;
    return v > 1000 ? 1000 : (unsigned)v;
}

const char *sh_cpu_type(uint32_t t)
{
    return t == CPU_TYPE_PERFORMANCE ? "P" : t == CPU_TYPE_EFFICIENCY ? "E" : "-";
}

void sh_fmt_cpu_time(uint64_t ns, char *buf, size_t cap)
{
    uint64_t cs = ns / 10000000;   /* hundredths */
    snprintf(buf, cap, "%lu:%02lu.%02lu", (unsigned long)(cs / 6000), (unsigned long)(cs / 100 % 60),
             (unsigned long)(cs % 100));
}
