/* The system's time zone as a POSIX TZ string, for a program started
 * without TZ (docs/M13-PLAN.md section 1, "Startup"): Jam OS's zones
 * (<wallclock.h>: a standard and a daylight offset, the days daylight
 * time starts and ends) written the way musl's localtime reads them,
 * e.g. Australia/Sydney as "AEST-10AEDT,M10.1.0/2,M4.1.0/3".
 *
 * It runs in the start, before musl's library is set up, so it formats
 * by hand: no printf. */
#include <wallclock.h>
#include "start.h"

/* A small appender that never runs past its buffer. */
struct out {
    char  *p;
    size_t left;   /* bytes left, the NUL's included */
};

static void put_c(struct out *o, char c)
{
    if (o->left > 1) {
        *o->p++ = c;
        o->left--;
    }
    *o->p = '\0';
}

static void put_s(struct out *o, const char *s)
{
    while (*s)
        put_c(o, *s++);
}

static void put_u(struct out *o, unsigned v)
{
    char d[10];
    int n = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    while (n)
        put_c(o, d[--n]);
}

static bool all_letters(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z')))
            return false;
    return n >= 3;
}

/* A zone's name: as it is if it is three or more letters, else in <>. */
static void put_name(struct out *o, const char *abbr)
{
    if (all_letters(abbr)) {
        put_s(o, abbr);
        return;
    }
    put_c(o, '<');
    put_s(o, abbr);
    put_c(o, '>');
}

/* h[:mm[:ss]] of s seconds (s >= 0). */
static void put_hms(struct out *o, unsigned s)
{
    put_u(o, s / 3600);
    if (s % 3600) {
        put_c(o, ':');
        put_c(o, (char)('0' + s % 3600 / 600));
        put_c(o, (char)('0' + s % 600 / 60));
    }
    if (s % 60) {
        put_c(o, ':');
        put_c(o, (char)('0' + s % 60 / 10));
        put_c(o, (char)('0' + s % 10));
    }
}

/* POSIX's offset is the other way round: hours west of UTC. */
static void put_offset(struct out *o, int min_east)
{
    if (min_east > 0)
        put_c(o, '-');
    put_hms(o, (unsigned)(min_east < 0 ? -min_east : min_east) * 60);
}

/* ",Mm.w.d/time" */
static void put_switch(struct out *o, const struct tz_switch *s)
{
    put_s(o, ",M");
    put_u(o, s->month);
    put_c(o, '.');
    put_u(o, s->week);
    put_c(o, '.');
    put_u(o, s->wday);
    if (s->at_s != 7200) {   /* 2:00 is POSIX's default */
        put_c(o, '/');
        if (s->at_s < 0)
            put_c(o, '-');
        put_hms(o, (unsigned)(s->at_s < 0 ? -s->at_s : s->at_s));
    }
}

void tz_posix(char *buf, size_t cap)
{
    struct tz z;
    (void)clock_now(NULL, &z);   /* the zone init gave, or TZ_DEFAULT: either way a zone */
    struct out o = { buf, cap };
    *buf = '\0';
    put_name(&o, z.std_abbr);
    put_offset(&o, z.std_min);
    if (z.dst_min == z.std_min)
        return;
    put_name(&o, z.dst_abbr);
    if (z.dst_min - z.std_min != 60)
        put_offset(&o, z.dst_min);
    put_switch(&o, &z.start);
    put_switch(&o, &z.end);
}
