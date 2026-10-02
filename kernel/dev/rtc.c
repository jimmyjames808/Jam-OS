/* The CMOS real-time clock (the shell's `date`), read through ports 0x70/0x71.
 *
 * The chip updates its registers once a second; for ~2 ms around that,
 * status A bit 7 (update in progress) is set and the registers may be
 * mid-change. So: wait until no update is in progress, read everything,
 * and read again until two reads agree. Register B says whether the values
 * are BCD (bit 2 clear) and whether the hour is 12-hour (bit 1 clear, with
 * bit 7 of the hour meaning PM). The year is two digits: 2000..2099 is
 * assumed (the FADT century register is not consulted). Index writes keep
 * bit 7 (NMI disable) clear, as firmware leaves it. */
#include <jam/abi.h>
#include <jam/kprintf.h>
#include <jam/rtc.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

static spinlock_t rtc_lock = SPINLOCK_INIT("rtc");

static uint8_t cmos(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

struct raw {
    uint8_t sec, min, hour, day, mon, year;   /* the CMOS registers as read (BCD or binary) */
};

/* An update keeps status A's "update in progress" bit set for about 2 ms. */
#define UPDATE_WAIT_NS (10 * NS_PER_MS)

/* One snapshot, taken while no update is in progress; false (with status A
 * as last read in *status_a) if one never ended within UPDATE_WAIT_NS. The
 * lock (interrupts off) is held only for the handful of port accesses,
 * never while waiting. */
static bool raw_read(struct raw *r, uint8_t *status_b, uint8_t *status_a)
{
    uint64_t deadline = uptime_ns() + UPDATE_WAIT_NS;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&rtc_lock);
        *status_a = cmos(0x0a);
        bool busy = *status_a & 0x80;
        if (!busy) {
            r->sec = cmos(0x00);
            r->min = cmos(0x02);
            r->hour = cmos(0x04);
            r->day = cmos(0x07);
            r->mon = cmos(0x08);
            r->year = cmos(0x09);
            *status_b = cmos(0x0b);
        }
        spin_unlock_irqrestore(&rtc_lock, f);
        if (!busy)
            return true;
        if (uptime_ns() >= deadline)
            return false;
        udelay(10);
    }
}

static uint8_t bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 15));
}

void rtc_decode(const uint8_t raw[6], uint8_t status_b, struct rtc_time *out)
{
    uint8_t sec = raw[0], min = raw[1], hour = raw[2], day = raw[3], mon = raw[4],
            year = raw[5];
    bool pm = hour & 0x80;
    hour &= 0x7f;
    if (!(status_b & 0x04)) {
        sec = bcd(sec);
        min = bcd(min);
        hour = bcd(hour);
        day = bcd(day);
        mon = bcd(mon);
        year = bcd(year);
    }
    if (!(status_b & 0x02)) {   /* 12-hour: 12 AM is 0, 12 PM is 12 */
        if (hour == 12)
            hour = 0;
        if (pm)
            hour += 12;
    }
    memset(out, 0, sizeof(*out));
    out->year = (uint16_t)(2000 + year % 100);
    out->month = mon;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    out->status_b = status_b;
}

status_t rtc_read(struct rtc_time *out)
{
    /* Two snapshots that agree: none of them straddled an update. A chip
     * that stays "in update" (none there: status A reads 0xff) fails at
     * once, not after every try. */
    static bool told;   /* the timeout is logged once a boot */
    struct raw a, b;
    uint8_t sa = 0, sb = 0, status_a = 0;
    bool ok = false;
    for (int tries = 0; tries < 5 && !ok; tries++) {
        if (!raw_read(&a, &sa, &status_a) || !raw_read(&b, &sb, &status_a)) {
            if (!__atomic_exchange_n(&told, true, __ATOMIC_RELAXED))
                kprintf("rtc: still updating after %llu ms (status A %02x)\n",
                        UPDATE_WAIT_NS / NS_PER_MS, status_a);
            return ERR_TIMED_OUT;
        }
        ok = !memcmp(&a, &b, sizeof(a)) && sa == sb;
    }
    if (!ok)
        return ERR_TIMED_OUT;
    const uint8_t raw[6] = { a.sec, a.min, a.hour, a.day, a.mon, a.year };
    rtc_decode(raw, sa, out);
    out->uptime_ns = uptime_ns();
    if (out->month < 1 || out->month > 12 || out->day < 1 || out->day > 31 || out->hour > 23 ||
        out->minute > 59 || out->second > 59)
        return ERR_INTERNAL;   /* nonsense from the chip (or no RTC) */
    return OK;
}
