/* date: the date and time from the real-time clock in $TZ (sh_time.c,
 * <wallclock.h>);
 * -u UTC, -r the raw clock reading, -d @secs a given Unix time. */
#include "sh.h"

static bool rtc_keeps_utc(const char *rtc)
{
    return rtc && (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC"));
}

/* -r: what the clock said and how it was read. */
static void show_raw(const struct rtc_time *r, int64_t utc)
{
    const char *rtc = sh_getvar("RTC");
    sh_say("RTC %04u-%02u-%02u %02u:%02u:%02u (register B %#x: %s, %s), read as %s time "
           "(RTC=%s; set RTC=utc if the clock keeps UTC, RTC=local if Sydney time)\n",
           r->year, r->month, r->day, r->hour, r->minute, r->second, r->status_b,
           r->status_b & 4 ? "binary" : "BCD", r->status_b & 2 ? "24-hour" : "12-hour",
           rtc_keeps_utc(rtc) ? "UTC" : "Sydney", rtc ? rtc : "local");
    sh_say("Unix time %ld\n", (long)utc);
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
    if (!given && !sh_clock_now(&utc, &r, "date"))
        return 1;
    struct tz tz;
    if (utc_only)
        tz_parse("UTC", &tz);
    else
        sh_local_tz(&tz, "date");
    char buf[80];
    time_format(utc, &tz, buf, sizeof(buf), true);
    sh_say("%s\n", buf);
    if (raw && !given)
        show_raw(&r, utc);
    return 0;
}
