/* uptime: the time, how long since boot, and how busy the online CPUs
 * were since then. */
#include "../sh.h"

SH_CMD(uptime)
{
    (void)argc;
    (void)argv;
    struct sys_info s;
    struct cpu_stat *c = calloc(SH_MAX_CPUS, sizeof(*c));
    uint32_t n = 0;
    if (!c || !sh_sysinfo(&s, "uptime") || !sh_cpus(c, &n, "uptime")) {
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
    unsigned busy = 1000 - sh_permille(idle, total);
    char up[40], now[16] = "";
    sh_fmt_uptime(s.uptime_ns, up, sizeof(up));
    int64_t utc;
    struct rtc_time r;
    struct sh_tz tz;
    if (sh_clock_now(&utc, &r, "uptime")) {
        sh_local_tz(&tz, "uptime");
        sh_fmt_time(utc, &tz, now, sizeof(now), false);
    }
    sh_say("%s up %s, 1 user, %u CPUs, %u.%u%% busy since boot\n", now, up, s.cpu_count, busy / 10,
           busy % 10);
    return 0;
}
