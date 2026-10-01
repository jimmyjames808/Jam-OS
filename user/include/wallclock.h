/* The calendar, time zones and the system's clock (user/lib/wallclock.c):
 * civil dates from Unix seconds and back, a small table of named zones
 * with their daylight-time rules (Australia/Sydney first: the owner's),
 * fixed offsets, the kernel's wall clock (wallclock_get, <jam/abi.h>) as
 * UTC and as local time in the zone init gave it, and dates written out
 * for people. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>

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

/* A daylight-time switch: the `week`th `wday` (0 Sunday) of `month`
 * (week 5: the last one), at `at_s` seconds after midnight in the time
 * that is in force until then. */
struct tz_switch {
    uint8_t month, week, wday;
    int32_t at_s;
};

#define TZ_DEFAULT "Australia/Sydney"   /* the zone when none is set */

/* A time zone. */
struct tz {
    char name[WALLCLOCK_ZONE_MAX];   /* as tz_parse was given it ("Australia/Sydney", "UTC+05:30") */
    int  std_min, dst_min;           /* offsets, minutes east of UTC (equal: no daylight time) */
    char std_abbr[16], dst_abbr[8];  /* what dates show: "AEST", "AEDT"; "UTC+05:30" */
    struct tz_switch start, end;     /* daylight time from start to end (if any) */
};

/* A zone by its name: one of the table's (Australia/Sydney, Melbourne,
 * Canberra, Hobart, Brisbane, Adelaide, Darwin, Perth, Pacific/Auckland,
 * Europe/London, America/New_York, America/Los_Angeles; Sydney, AEST,
 * AEDT and local are Sydney too), UTC/GMT/Z, or [UTC|GMT]+H[:MM] /
 * -H[:MM]; NULL or "" is TZ_DEFAULT. false if not understood. */
bool    tz_parse(const char *s, struct tz *tz);
/* The offset (minutes east of UTC) at UTC time t, and the zone's
 * abbreviation then ("AEDT"). */
int     tz_offset_at(const struct tz *tz, int64_t t, const char **abbr);
/* Local wall time `local` (seconds, as if UTC) in tz -> UTC. A time that
 * happens twice (the hour daylight time ends) is taken as standard time;
 * one that never happens (the hour it starts) is taken as daylight time. */
int64_t tz_local_to_utc(const struct tz *tz, int64_t local);
/* "Thu 15 Jan 2026 12:02:03 AEDT (UTC+11:00)", or just "12:02:03". */
void    time_format(int64_t utc, const struct tz *tz, char *buf, size_t cap, bool with_zone);
/* "2026-10-01 14:03:22 AEST" */
void    time_format_iso(int64_t utc, const struct tz *tz, char *buf, size_t cap);

/* The system's clock (the kernel's, which init sets at boot): UTC seconds
 * now into *utc and the zone init gave (TZ_DEFAULT if none, or one this
 * libos doesn't know) into *zone (either may be NULL). false: there is
 * no clock, or nobody has set it yet (only the RTC's reading in its own
 * zone, which *utc then gets as if it were UTC). */
bool    clock_now(int64_t *utc, struct tz *zone);
/* The local date and time now in the system's zone, for file times and
 * the like. false (and *c the RTC's reading as it is) if the clock isn't
 * set; false and 2026-01-01 00:00 with no clock at all. */
bool    clock_local(struct civil *c);
