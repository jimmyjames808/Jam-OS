/* The CMOS real-time clock (kernel/dev/rtc.c; M7 shell `date`). */
#pragma once

#include <stdint.h>
#include <jam/status.h>

struct rtc_time;

/* Read the clock (waits out an update in progress; may take a few ms, may
 * sleep-spin, never with interrupts off for long). ERR_TIMED_OUT if the
 * chip never settled, ERR_INTERNAL if it holds an impossible date. */
status_t rtc_read(struct rtc_time *out);
/* Registers 0, 2, 4, 7, 8, 9 (sec min hour day month year) as the chip
 * holds them, decoded with register B's format bits (tests use this). */
void rtc_decode(const uint8_t raw[6], uint8_t status_b, struct rtc_time *out);
