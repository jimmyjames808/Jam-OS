/* Calendar and time zones for date, uptime and top: civil dates from Unix
 * seconds and back, $TZ (Sydney's daylight time, or a fixed offset), the
 * real-time clock read as $RTC says it keeps time. */
#include "sh.h"

static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- the calendar ------------------------------------------------------------------ */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

struct civil {
    int64_t  year;                         /* e.g. 2026 */
    unsigned month, day;                   /* 1..12, 1..31 */
    unsigned hour, minute, second;         /* 0..23, 0..59, 0..59 */
    unsigned wday;                         /* 0: Sunday */
};

static void civil_from_secs(int64_t t, struct civil *c)
{
    int64_t z = (t >= 0 ? t : t - 86399) / 86400;
    int64_t secs = t - z * 86400;
    c->wday = (unsigned)(((z % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = (int64_t)yoe + era * 400 + (c->month <= 2);
    c->hour = (unsigned)(secs / 3600);
    c->minute = (unsigned)(secs / 60 % 60);
    c->second = (unsigned)(secs % 60);
}

/* ---- time zones -------------------------------------------------------------------- */

bool sh_parse_tz(const char *s, struct sh_tz *tz)
{
    memset(tz, 0, sizeof(*tz));
    if (!s || !*s || !strcmp(s, "Australia/Sydney") || !strcmp(s, "Sydney") ||
        !strcmp(s, "AEST") || !strcmp(s, "AEDT") || !strcmp(s, "local") ||
        !strcmp(s, "Australia/Melbourne") || !strcmp(s, "Australia/Canberra")) {
        tz->sydney = true;
        return true;
    }
    const char *p = s;
    if (!strncmp(p, "UTC", 3) || !strncmp(p, "GMT", 3))
        p += 3;
    else if (*p == 'Z' && !p[1])
        p++;
    if (!*p) {
        memcpy(tz->name, "UTC", 4);
        return true;
    }
    if (*p != '+' && *p != '-')
        return false;
    int sign = *p++ == '-' ? -1 : 1;
    unsigned h = 0, m = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) {
        h = h * 10 + (unsigned)(*p++ - '0');
        digits++;
    }
    if (!digits || h > 14)
        return false;
    if (*p == ':') {
        p++;
        if (!(p[0] >= '0' && p[0] <= '5' && p[1] >= '0' && p[1] <= '9'))
            return false;
        m = (unsigned)(p[0] - '0') * 10 + (unsigned)(p[1] - '0');
        p += 2;
    }
    if (*p)
        return false;
    tz->off_min = sign * (int)(h * 60 + m);
    snprintf(tz->name, sizeof(tz->name), "UTC%c%02u:%02u", sign < 0 ? '-' : '+', h, m);
    return true;
}

/* 02:00 AEST on the first Sunday of `month` in year y, as UTC seconds,
 * with `local_hour` and offset hours: the Sydney switch moments. */
static int64_t first_sunday_utc(int64_t y, unsigned month, int local_hour, int off_h)
{
    int64_t d = days_from_civil(y, month, 1);
    unsigned wd = (unsigned)(((d % 7) + 11) % 7);
    d += (7 - wd) % 7;
    return d * 86400 + (int64_t)(local_hour - off_h) * 3600;
}

/* Sydney: daylight time from the first Sunday of October 02:00 AEST to the
 * first Sunday of April 03:00 AEDT (since 2008). */
static bool sydney_dst(int64_t utc)
{
    struct civil c;
    civil_from_secs(utc + 10 * 3600, &c);
    int64_t end = first_sunday_utc(c.year, 4, 3, 11), start = first_sunday_utc(c.year, 10, 2, 10);
    return utc < end || utc >= start;
}

/* The offset (minutes) and zone name at UTC time t. */
static int tz_at(const struct sh_tz *tz, int64_t t, const char **name)
{
    if (tz->sydney) {
        bool dst = sydney_dst(t);
        *name = dst ? "AEDT" : "AEST";
        return dst ? 660 : 600;
    }
    *name = tz->name;
    return tz->off_min;
}

/* Local wall time `local` (seconds, as if UTC) in tz -> UTC. */
static int64_t tz_local_to_utc(const struct sh_tz *tz, int64_t local)
{
    const char *n;
    if (!tz->sydney)
        return local - (int64_t)tz->off_min * 60;
    int64_t t = local - 600 * 60;   /* standard time first */
    if (tz_at(tz, t, &n) == 660)
        t = local - 660 * 60;
    return t;
}

bool sh_local_tz(struct sh_tz *tz, const char *who)
{
    const char *s = sh_getvar("TZ");
    if (sh_parse_tz(s, tz))
        return true;
    sh_tty("%s: TZ=%s not understood (Australia/Sydney, UTC, +10, -5:30): using UTC\n", who, s);
    sh_parse_tz("UTC", tz);
    return false;
}

/* ---- the clock and formatting ------------------------------------------------------ */

bool sh_clock_now(int64_t *utc, struct rtc_time *raw, const char *who)
{
    status_t st = jam_rtc_read(sh_root(), raw);
    if (st != OK) {
        sh_tty("%s: the real-time clock: %s\n", who, status_str(st));
        return false;
    }
    int64_t local = days_from_civil(raw->year, raw->month, raw->day) * 86400 +
                    raw->hour * 3600 + raw->minute * 60 + raw->second;
    const char *rtc = sh_getvar("RTC");
    struct sh_tz tz;
    if (rtc && (!strcmp(rtc, "utc") || !strcmp(rtc, "UTC")))
        *utc = local;
    else if (sh_parse_tz(rtc, &tz))   /* "local" = Australia/Sydney */
        *utc = tz_local_to_utc(&tz, local);
    else
        *utc = local;
    return true;
}

void sh_fmt_time(int64_t utc, const struct sh_tz *tz, char *buf, size_t cap, bool with_zone)
{
    const char *name;
    int off = tz_at(tz, utc, &name);
    struct civil c;
    civil_from_secs(utc + (int64_t)off * 60, &c);
    int a = off < 0 ? -off : off;
    if (with_zone)
        snprintf(buf, cap, "%s %u %s %ld %02u:%02u:%02u %s (UTC%c%02d:%02d)", wdays[c.wday], c.day,
                 months[c.month - 1], (long)c.year, c.hour, c.minute, c.second, name,
                 off < 0 ? '-' : '+', a / 60, a % 60);
    else
        snprintf(buf, cap, "%02u:%02u:%02u", c.hour, c.minute, c.second);
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
