/* The wall clock (<jam/wallclock.h>): the RTC's date as seconds, what
 * wallclock_set refuses, and that the clock runs with the uptime. Setting
 * the live clock is left to utest (user/tests/utest/time.c), which puts it
 * back as it was. */
#include <jam/ktest.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/wallclock.h>

KTEST(wallclock_civil_secs)
{
    KT_EQ(wallclock_civil_secs(1970, 1, 1, 0, 0, 0), 0);
    KT_EQ(wallclock_civil_secs(2000, 1, 1, 0, 0, 0), 946684800);
    KT_EQ(wallclock_civil_secs(2000, 2, 29, 23, 59, 59), 951868799);   /* a leap day */
    KT_EQ(wallclock_civil_secs(2026, 1, 15, 1, 2, 3), 1768438923);
    KT_EQ(wallclock_civil_secs(2099, 12, 31, 23, 59, 59), 4102444799ll);   /* the RTC's last */
}

KTEST(wallclock_check_refuses)
{
    struct wall_clock c;
    memset(&c, 0, sizeof(c));
    c.utc_ns = 1768438923ll * 1000000000ll;
    c.uptime_ns = 1000;
    memcpy(c.zone, "Australia/Sydney", 17);
    KT_EQ(wallclock_check(&c, 2000), OK);
    KT_EQ(wallclock_check(&c, 999), ERR_INVALID_ARGS);   /* an uptime still to come */
    c.flags = WALLCLOCK_SET;
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    c.flags = WALLCLOCK_NET | WALLCLOCK_RTC;
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    c.flags = WALLCLOCK_NET;                              /* the one flag a setter may pass */
    KT_EQ(wallclock_check(&c, 2000), OK);
    c.flags = 0;
    c.reserved = 1;
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    c.reserved = 0;
    c.utc_ns = -1;
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    c.utc_ns = 7258118400ll * 1000000000ll;              /* 2200 */
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    c.utc_ns = 0;
    memset(c.zone, 'x', sizeof(c.zone));                  /* no NUL */
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    memcpy(c.zone, "UTC\x1b[2J", 8);                       /* not printable */
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    memcpy(c.zone, "a b", 4);                             /* no spaces either */
    KT_EQ(wallclock_check(&c, 2000), ERR_INVALID_ARGS);
    memset(c.zone, 0, sizeof(c.zone));                    /* "": the zone stays */
    KT_EQ(wallclock_check(&c, 2000), OK);
}

/* The clock runs with the uptime (QEMU has an RTC, so there is a clock). */
KTEST(wallclock_runs)
{
    struct wall_clock a, b;
    if (wallclock_get(&a) == ERR_NOT_FOUND)
        return;   /* a machine without an RTC that nobody set */
    thread_sleep_ms(20);
    KT_EQ(wallclock_get(&b), OK);
    int64_t clock = b.utc_ns - a.utc_ns, up = (int64_t)(b.uptime_ns - a.uptime_ns);
    KT_EQ(clock, up);
    KT_ASSERT(up >= 20000000);
    KT_ASSERT(a.utc_ns > 946684800ll * 1000000000ll);    /* after 2000 */
}
