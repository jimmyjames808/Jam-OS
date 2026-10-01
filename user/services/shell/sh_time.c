/* The time for date, uptime and top: $TZ (a zone libos's <wallclock.h>
 * understands), the real-time clock read as $RTC says it keeps time, and
 * uptimes written out. The calendar and the zones themselves are libos's. */
#include "sh.h"

/* ---- time zones -------------------------------------------------------------------- */

bool sh_local_tz(struct tz *tz, const char *who)
{
    const char *s = sh_getvar("TZ");
    if (tz_parse(s, tz))
        return true;
    sh_tty("%s: TZ=%s not understood (Australia/Sydney, UTC, +10, -5:30): using UTC\n", who, s);
    tz_parse("UTC", tz);
    return false;
}

/* ---- the clock and uptimes ---------------------------------------------------------- */

bool sh_clock_now(int64_t *utc, struct rtc_time *raw, const char *who)
{
    status_t st = jam_rtc_read(sh_root(), raw);
    if (st != OK) {
        sh_tty("%s: the real-time clock: %s\n", who, status_str(st));
        return false;
    }
    int64_t local = civil_days(raw->year, raw->month, raw->day) * 86400 +
                    raw->hour * 3600 + raw->minute * 60 + raw->second;
    const char *rtc = sh_getvar("RTC");
    struct tz tz;
    if (rtc && (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC")))
        *utc = local;
    else if (tz_parse(rtc, &tz))   /* "local" = Australia/Sydney */
        *utc = tz_local_to_utc(&tz, local);
    else
        *utc = local;
    return true;
}

void sh_fmt_uptime(uint64_t ns, char *buf, size_t cap)
{
    uint64_t s = ns / NS_PER_S, d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d)
        snprintf(buf, cap, "%lu day%s, %lu:%02lu", (unsigned long)d, d == 1 ? "" : "s",
                 (unsigned long)h, (unsigned long)m);
    else if (h)
        snprintf(buf, cap, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m,
                 (unsigned long)(s % 60));
    else
        snprintf(buf, cap, "%lu min %lu s", (unsigned long)m, (unsigned long)(s % 60));
}
