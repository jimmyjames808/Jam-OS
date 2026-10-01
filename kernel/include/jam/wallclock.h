/* The wall clock (kernel/dev/wallclock.c): UTC as an offset from the uptime,
 * started at boot from the real-time clock and set by init (wallclock_get and
 * wallclock_set, struct wall_clock in <jam/abi.h>). The kernel keeps no
 * calendar rules beyond turning the RTC's date into seconds: time zones,
 * and which one the RTC keeps, are user space's (libos <wallclock.h>,
 * init's settings). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/status.h>

/* At boot, once the TSC is calibrated: read the RTC and start the clock
 * from it, taken as UTC (WALLCLOCK_RTC). Without an RTC there is no clock
 * until wallclock_set. */
void     wallclock_init(void);
/* The time now; ERR_NOT_FOUND without a clock. Any context but interrupts
 * off with the clock's lock held. */
status_t wallclock_get(struct wall_clock *out);
/* Set the clock (checked with wallclock_check first). */
status_t wallclock_set(const struct wall_clock *in);
/* in is a wallclock_set request the kernel takes: flags and reserved 0, a
 * zone of printable ASCII within WALLCLOCK_ZONE_MAX with its NUL, a UTC time
 * from 1970 to 2200, an uptime not after `uptime_now`.
 * ERR_INVALID_ARGS otherwise. */
status_t wallclock_check(const struct wall_clock *in, uint64_t uptime_now);
/* Seconds since 1970-01-01 of a date and time (proleptic Gregorian,
 * years 1970..9999). */
int64_t  wallclock_civil_secs(unsigned year, unsigned month, unsigned day, unsigned hour,
                          unsigned minute, unsigned second);
