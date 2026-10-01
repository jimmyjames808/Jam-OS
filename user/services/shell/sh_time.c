/* The time for date, uptime and top: the system's clock (the kernel's,
 * which init sets from the real-time clock), shown in $TZ if it is set
 * and else in the system's zone (/data/etc/settings' `timezone`), and
 * uptimes written out. The calendar and the zones are libos's
 * (<wallclock.h>). And what the commands keep in the settings file. */
#include <settings.h>
#include "sh.h"

/* ---- time zones -------------------------------------------------------------------- */

bool sh_local_tz(struct tz *tz, const char *who)
{
    const char *s = sh_getvar("TZ");
    if (!s || !*s) {
        (void)clock_now(NULL, tz);   /* the system's zone (or the default) either way */
        return true;
    }
    if (tz_parse(s, tz))
        return true;
    sh_tty("%s: TZ=%s not understood (Australia/Sydney, UTC, +10, -5:30): using UTC\n", who, s);
    tz_parse("UTC", tz);
    return false;
}

/* ---- the clock and uptimes ---------------------------------------------------------- */

bool sh_clock_now(int64_t *utc, const char *who)
{
    if (clock_now(utc, NULL))
        return true;
    struct wall_clock w;
    if (jam_wallclock_get(&w) == OK)
        return true;   /* not set by init (yet): the RTC's reading as it is */
    sh_tty("%s: there is no clock (no real-time clock, and nobody set one)\n", who);
    return false;
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

/* ---- settings ---------------------------------------------------------------------- */

void sh_keep_setting(const char *who, const char *key, const char *value)
{
    status_t st = settings_set(SETTINGS_FILE, key, value);
    if (st != OK)
        sh_tty("%s: not kept in " SETTINGS_FILE " (%s): it lasts until the next boot\n", who,
               sh_why(st));
}
