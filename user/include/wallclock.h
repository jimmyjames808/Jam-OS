/* The calendar and time zones (user/lib/wallclock.c): civil dates from
 * Unix seconds and back, Australia/Sydney's daylight time, fixed offsets,
 * and dates written out for people. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A date and time of day, in whatever zone it was made for. */
struct civil {
    int64_t  year;                    /* e.g. 2026 */
    unsigned month, day;              /* 1..12, 1..31 */
    unsigned hour, minute, second;    /* 0..23, 0..59, 0..59 */
    unsigned wday;                    /* 0: Sunday */
};

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian; negative before). */
int64_t civil_days(int64_t y, unsigned m, unsigned d);
/* Unix seconds t as a date and time (no zone: add the offset first). */
void    civil_from_secs(int64_t t, struct civil *c);

/* A time zone. */
struct tz {
    bool sydney;       /* Australia/Sydney: AEST +10, AEDT +11 (Oct..Apr) */
    int  off_min;      /* fixed zones: minutes east of UTC */
    char name[24];     /* what dates show: "UTC", "UTC+05:30" */
};

/* Australia/Sydney (also Sydney, AEST, AEDT, local), UTC/GMT, or
 * [UTC|GMT]+H[:MM] / -H[:MM]; NULL or "" is Sydney. false if not
 * understood. */
bool    tz_parse(const char *s, struct tz *tz);
/* The offset (minutes east of UTC) at UTC time t, and the zone's name
 * then ("AEDT"). */
int     tz_offset_at(const struct tz *tz, int64_t t, const char **name);
/* Local wall time `local` (seconds, as if UTC) in tz -> UTC. A time that
 * happens twice (the hour daylight time ends) is taken as standard time. */
int64_t tz_local_to_utc(const struct tz *tz, int64_t local);
/* "Thu 15 Jan 2026 12:02:03 AEDT (UTC+11:00)", or just "12:02:03". */
void    time_format(int64_t utc, const struct tz *tz, char *buf, size_t cap, bool with_zone);
