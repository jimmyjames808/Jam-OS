/* date: the date and time from the system's clock (sh_time.c,
 * <wallclock.h>) in $TZ, or in the system's zone; -u UTC, -d @secs a given
 * Unix time; -r the real-time clock as it reads and how the clock came
 * from it; -z <zone> makes zone the system's (kept in /data/etc/settings). */
#include <settings.h>
#include "sh.h"

/* -r: what the RTC says, and what the system's clock is. */
static int show_raw(void)
{
    struct rtc_time r;
    status_t st = jam_rtc_read(sh_root(), &r);
    if (st != OK) {
        sh_tty("date: the real-time clock: %s\n", status_str(st));
        return 1;
    }
    char rtc[SETTINGS_VALUE_MAX] = "local";
    (void)settings_get(SETTINGS_FILE, "rtc", rtc, sizeof(rtc));   /* none: the default */
    sh_say("RTC %04u-%02u-%02u %02u:%02u:%02u (register B %#x: %s, %s), read as %s time "
           "(the setting rtc = %s in " SETTINGS_FILE ")\n",
           r.year, r.month, r.day, r.hour, r.minute, r.second, r.status_b,
           r.status_b & 4 ? "binary" : "BCD", r.status_b & 2 ? "24-hour" : "12-hour",
           !strcmp(rtc, "utc") || !strcmp(rtc, "UTC") ? "UTC" : rtc, rtc);
    struct wall_clock w;
    st = jam_wallclock_get(&w);
    w.zone[sizeof(w.zone) - 1] = '\0';
    if (st != OK) {
        sh_say("the clock: none\n");
        return 0;
    }
    struct tz utc;
    tz_parse("UTC", &utc);
    char when[48];
    time_format_iso(w.utc_ns / NS_PER_S, &utc, when, sizeof(when));
    sh_say("the clock: %s, %s, zone %s; Unix time %ld\n", when,
           w.flags & WALLCLOCK_NET   ? "set from the network (sntp)"
           : w.flags & WALLCLOCK_SET ? "set by init from the RTC"
                                     : "the RTC's reading (not set)",
           w.zone[0] ? w.zone : "(none)", (long)(w.utc_ns / NS_PER_S));
    return 0;
}

/* -z: the system's zone from now on, and in the settings. */
static int set_zone(const char *name)
{
    struct tz tz;
    if (!tz_parse(name, &tz)) {
        sh_tty("date: %s is not a zone (Australia/Sydney, Australia/Perth, Europe/London, UTC, "
               "UTC+05:30, ...)\n", name);
        return 2;
    }
    struct wall_clock w;
    status_t st = jam_wallclock_get(&w);
    if (st == OK) {
        w.flags &= WALLCLOCK_NET;   /* a time from the network stays one */
        w.reserved = 0;
        memset(w.zone, 0, sizeof(w.zone));
        memcpy(w.zone, tz.name, strlen(tz.name));
        st = jam_wallclock_set(sh_root(), &w);
    }
    if (st != OK) {
        sh_tty("date: the clock didn't take the zone: %s\n", status_str(st));
        return 1;
    }
    sh_keep_setting("date", "timezone", tz.name);
    char buf[80];
    time_format(w.utc_ns / NS_PER_S, &tz, buf, sizeof(buf), true);
    sh_say("date: the zone is %s now: %s\n", tz.name, buf);
    return 0;
}

SH_CMD(date)
{
    bool utc_only = false, given = false;
    int64_t utc = 0;
    for (int i = 1; i < argc; i++) {
        uint64_t v;
        if (!strcmp(argv[i], "-u")) {
            utc_only = true;
        } else if (!strcmp(argv[i], "-r") && argc == 2) {
            return show_raw();
        } else if (!strcmp(argv[i], "-z") && argc == 3) {
            return set_zone(argv[2]);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc && argv[i + 1][0] == '@' &&
                   sh_parse_u64(argv[i + 1] + 1, &v)) {
            utc = (int64_t)v;
            given = true;
            i++;
        } else {
            sh_tty("usage: date [-u] [-d @unix-seconds] | date -r | date -z <zone>\n");
            return 2;
        }
    }
    if (!given && !sh_clock_now(&utc, "date"))
        return 1;
    struct tz tz;
    if (utc_only)
        tz_parse("UTC", &tz);
    else
        sh_local_tz(&tz, "date");
    char buf[80];
    time_format(utc, &tz, buf, sizeof(buf), true);
    sh_say("%s\n", buf);
    return 0;
}
