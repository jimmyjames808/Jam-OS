/* The calendar and time zones (<wallclock.h>). The calendar is Howard
 * Hinnant's days_from_civil / civil_from_days (public domain): eras of
 * 400 years, so it needs no tables and no loops. */
#include <os.h>
#include <wallclock.h>

static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- the calendar ------------------------------------------------------------------ */

int64_t civil_days(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_secs(int64_t t, struct civil *c)
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

#define H(h) ((int32_t)(h) * 3600)

/* The named zones (their rules as in the tz database, 2024): daylight time
 * from `start` to `end`. Southern zones switch on in spring (October),
 * northern ones in March. */
static const struct zone {
    const char *name;
    int         std_min, dst_min;
    const char *std_abbr, *dst_abbr;
    struct tz_switch start, end;
} zones[] = {
    { "Australia/Sydney", 600, 660, "AEST", "AEDT", { 10, 1, 0, H(2) }, { 4, 1, 0, H(3) } },
    { "Australia/Brisbane", 600, 600, "AEST", "AEST", { 0 }, { 0 } },
    { "Australia/Adelaide", 570, 630, "ACST", "ACDT", { 10, 1, 0, H(2) }, { 4, 1, 0, H(3) } },
    { "Australia/Darwin", 570, 570, "ACST", "ACST", { 0 }, { 0 } },
    { "Australia/Perth", 480, 480, "AWST", "AWST", { 0 }, { 0 } },
    { "Pacific/Auckland", 720, 780, "NZST", "NZDT", { 9, 5, 0, H(2) }, { 4, 1, 0, H(3) } },
    { "Europe/London", 0, 60, "GMT", "BST", { 3, 5, 0, H(1) }, { 10, 5, 0, H(2) } },
    { "America/New_York", -300, -240, "EST", "EDT", { 3, 2, 0, H(2) }, { 11, 1, 0, H(2) } },
    { "America/Los_Angeles", -480, -420, "PST", "PDT", { 3, 2, 0, H(2) }, { 11, 1, 0, H(2) } },
};

/* Other names for a table zone: same rules. */
static const struct { const char *alias, *zone; } aliases[] = {
    { "Sydney", "Australia/Sydney" },          { "AEST", "Australia/Sydney" },
    { "AEDT", "Australia/Sydney" },            { "local", "Australia/Sydney" },
    { "Australia/Melbourne", "Australia/Sydney" }, { "Australia/Canberra", "Australia/Sydney" },
    { "Australia/ACT", "Australia/Sydney" },   { "Australia/NSW", "Australia/Sydney" },
    { "Australia/Victoria", "Australia/Sydney" }, { "Australia/Hobart", "Australia/Sydney" },
    { "Australia/Tasmania", "Australia/Sydney" }, { "Australia/Queensland", "Australia/Brisbane" },
    { "Australia/South", "Australia/Adelaide" }, { "Australia/North", "Australia/Darwin" },
    { "Australia/West", "Australia/Perth" },   { "NZ", "Pacific/Auckland" },
    { "GB", "Europe/London" },                 { "US/Eastern", "America/New_York" },
    { "US/Pacific", "America/Los_Angeles" },
};

static bool table_zone(const char *s, struct tz *tz)
{
    const char *want = s;
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!strcmp(s, aliases[i].alias))
            want = aliases[i].zone;
    for (unsigned i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        const struct zone *z = &zones[i];
        if (strcmp(want, z->name))
            continue;
        tz->std_min = z->std_min;
        tz->dst_min = z->dst_min;
        snprintf(tz->std_abbr, sizeof(tz->std_abbr), "%s", z->std_abbr);
        snprintf(tz->dst_abbr, sizeof(tz->dst_abbr), "%s", z->dst_abbr);
        tz->start = z->start;
        tz->end = z->end;
        return true;
    }
    return false;
}

/* [UTC|GMT|Z][+H[:MM]|-H[:MM]] */
static bool fixed_zone(const char *p, struct tz *tz)
{
    if (!strncmp(p, "UTC", 3) || !strncmp(p, "GMT", 3))
        p += 3;
    else if (*p == 'Z' && !p[1])
        p++;
    if (!*p) {
        snprintf(tz->std_abbr, sizeof(tz->std_abbr), "UTC");
        memcpy(tz->dst_abbr, "UTC", 4);
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
    tz->std_min = tz->dst_min = sign * (int)(h * 60 + m);
    snprintf(tz->std_abbr, sizeof(tz->std_abbr), "UTC%c%02u:%02u", sign < 0 ? '-' : '+', h, m);
    return true;
}

bool tz_parse(const char *s, struct tz *tz)
{
    memset(tz, 0, sizeof(*tz));
    if (!s || !*s)
        s = TZ_DEFAULT;
    if (strlen(s) >= sizeof(tz->name))
        return false;
    if (!table_zone(s, tz) && !fixed_zone(s, tz))
        return false;
    memcpy(tz->name, s, strlen(s) + 1);
    return true;
}

/* The moment (UTC seconds) of switch w in year y, `before_min` being the
 * offset in force until it. */
static int64_t switch_utc(const struct tz_switch *w, int64_t y, int before_min)
{
    int64_t first = civil_days(y, w->month, 1);
    int64_t next = w->month == 12 ? civil_days(y + 1, 1, 1) : civil_days(y, w->month + 1u, 1);
    unsigned wd = (unsigned)(((first % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    int64_t d = first + (int64_t)((w->wday + 7 - wd) % 7) + 7 * (int64_t)(w->week - 1);
    while (d >= next)
        d -= 7;   /* week 5: the last one in the month */
    return d * 86400 + w->at_s - (int64_t)before_min * 60;
}

static bool in_dst(const struct tz *tz, int64_t utc)
{
    if (tz->std_min == tz->dst_min || !tz->start.month)
        return false;
    struct civil c;
    civil_from_secs(utc + (int64_t)tz->std_min * 60, &c);
    int64_t on = switch_utc(&tz->start, c.year, tz->std_min);
    int64_t off = switch_utc(&tz->end, c.year, tz->dst_min);
    if (on < off)
        return utc >= on && utc < off;   /* northern: March to autumn */
    return utc >= on || utc < off;       /* southern: spring to April */
}

int tz_offset_at(const struct tz *tz, int64_t t, const char **abbr)
{
    bool dst = in_dst(tz, t);
    *abbr = dst ? tz->dst_abbr : tz->std_abbr;
    return dst ? tz->dst_min : tz->std_min;
}

int64_t tz_local_to_utc(const struct tz *tz, int64_t local)
{
    const char *n;
    int64_t t = local - (int64_t)tz->std_min * 60;   /* standard time first */
    if (tz_offset_at(tz, t, &n) != tz->std_min)
        t = local - (int64_t)tz->dst_min * 60;
    return t;
}

/* ---- formatting -------------------------------------------------------------------- */

void time_format(int64_t utc, const struct tz *tz, char *buf, size_t cap, bool with_zone)
{
    const char *name;
    int off = tz_offset_at(tz, utc, &name);
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

void time_format_iso(int64_t utc, const struct tz *tz, char *buf, size_t cap)
{
    const char *name;
    int off = tz_offset_at(tz, utc, &name);
    struct civil c;
    civil_from_secs(utc + (int64_t)off * 60, &c);
    snprintf(buf, cap, "%04ld-%02u-%02u %02u:%02u:%02u %s", (long)c.year, c.month, c.day, c.hour,
             c.minute, c.second, name);
}

/* ---- the system's clock ------------------------------------------------------------ */

bool clock_now(int64_t *utc, struct tz *zone)
{
    struct wall_clock w;
    status_t st = jam_wallclock_get(&w);
    w.zone[sizeof(w.zone) - 1] = '\0';
    if (zone && (st != OK || !tz_parse(w.zone, zone)))
        tz_parse(TZ_DEFAULT, zone);
    if (st != OK)
        return false;
    if (utc)
        *utc = w.utc_ns >= 0 ? w.utc_ns / NS_PER_S : -((-w.utc_ns + NS_PER_S - 1) / NS_PER_S);
    return (w.flags & WALLCLOCK_SET) != 0;
}

bool clock_local(struct civil *c)
{
    int64_t utc;
    struct tz zone;
    struct wall_clock w;
    if (jam_wallclock_get(&w) != OK) {
        civil_from_secs(civil_days(2026, 1, 1) * 86400, c);
        return false;
    }
    bool set = clock_now(&utc, &zone);
    const char *abbr;
    civil_from_secs(set ? utc + (int64_t)tz_offset_at(&zone, utc, &abbr) * 60 : utc, c);
    return set;
}
