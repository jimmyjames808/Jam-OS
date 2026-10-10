/* The wall clock (wallclock.h). Kept as base_utc at uptime base_up: the time
 * now is base_utc plus the uptime since, so it runs at the TSC's rate and
 * never needs a tick. The RTC is read once, at boot; init sets the clock
 * from its own reading of the RTC once it knows which zone the RTC keeps
 * (its settings), and with the zone programs show times in.
 *
 * The time page (struct time_page in <jam/abi.h>, clock_page) is a kept
 * VMO of one page, made at boot and never freed, that this file writes
 * through a kernel mapping: the TSC's conversion once, then the wall
 * clock's offset (base_utc - base_up) and flags at every change, under
 * the page's sequence word (time_page_publish). Every process may map it
 * read-only, so it reads both clocks without a system call.
 *
 * The lock is a spinlock (wallclock_get may come from any thread); nothing
 * under it but copying a few words, and the page's few stores. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/rtc.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vmo.h>
#include <jam/wallclock.h>

#define UTC_MAX_S (7258118400ll)   /* 2200-01-01: past it, a wallclock_set is refused */

static spinlock_t lock = SPINLOCK_INIT("wall clock");
static int64_t  base_utc;               /* ns since 1970, at ... (lock) */
static uint64_t base_up;                /* ... this uptime (lock) */
static uint32_t flags;                  /* WALLCLOCK_* (lock); 0: no clock */
static char     zone[WALLCLOCK_ZONE_MAX];   /* (lock) */
static struct vmo       *page_vmo;      /* the time page, or NULL (set once at boot) */
static struct time_page *page;          /* its kernel mapping; wall_* under lock */

int64_t wallclock_civil_secs(unsigned year, unsigned month, unsigned day, unsigned hour,
                             unsigned minute, unsigned second)
{
    /* days_from_civil (H. Hinnant): March-based years, so February is last. */
    int64_t y = (int64_t)year - (month <= 2);
    int64_t era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

void time_page_publish(struct time_page *p, int64_t offset, uint32_t wall_flags)
{
    uint32_t s = p->seq;   /* only writers store it, and they are serialised */
    __atomic_store_n(&p->seq, s + 1, __ATOMIC_RELAXED);
    /* The odd word before the fields: a reader that sees a new field sees
     * the word moved (time_page_retry's fence is the other side). */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&p->wall_offset, offset, __ATOMIC_RELAXED);
    __atomic_store_n(&p->wall_flags, wall_flags, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s + 2, __ATOMIC_RELEASE);   /* the fields before the even word */
}

/* lock held: the clock as it stands, onto the page. */
static void page_sync_locked(void)
{
    if (page)
        time_page_publish(page, base_utc - (int64_t)base_up, flags);
}

/* The page, made once at boot before anybody can ask for it. Without it
 * clock_page fails and readers use the calls. */
static void page_init(void)
{
    struct vmo *v;
    void *va;
    status_t st = vmo_create(PAGE_SIZE, VMO_KEEP_PAGES, &v);
    if (st == OK && (st = vmo_map_kernel(v, 0, PAGE_SIZE, VM_WRITE, &va)) != OK)
        kobject_unref(vmo_kobject(v));
    if (st != OK) {
        kprintf("clock: no time page (%d): programs read the clocks with calls\n", st);
        return;
    }
    tsc_time_page(va);
    page_vmo = v;
    page = va;
}

status_t wallclock_page(struct vmo **out)
{
    if (!page_vmo)
        return ERR_NOT_SUPPORTED;
    kobject_ref(vmo_kobject(page_vmo));
    *out = page_vmo;
    return OK;
}

const struct time_page *wallclock_page_kernel(void)
{
    return page;
}

void wallclock_init(void)
{
    page_init();
    struct rtc_time r;
    status_t st = rtc_read(&r);
    if (st != OK) {
        kprintf("clock: no real-time clock (%d): no date until init sets one\n", st);
        return;
    }
    int64_t s = wallclock_civil_secs(r.year, r.month, r.day, r.hour, r.minute, r.second);
    uint64_t f = spin_lock_irqsave(&lock);
    base_utc = s * 1000000000ll;
    base_up = r.uptime_ns;
    flags = WALLCLOCK_RTC;
    page_sync_locked();
    spin_unlock_irqrestore(&lock, f);
    kprintf("clock: the RTC says %04u-%02u-%02u %02u:%02u:%02u (its own zone, taken as UTC "
            "until init sets the clock)\n", r.year, r.month, r.day, r.hour, r.minute, r.second);
}

status_t wallclock_get(struct wall_clock *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t up = uptime_ns();
    uint64_t f = spin_lock_irqsave(&lock);
    out->flags = flags;
    out->utc_ns = base_utc + (int64_t)(up - base_up);
    out->uptime_ns = up;
    memcpy(out->zone, zone, sizeof(zone));
    spin_unlock_irqrestore(&lock, f);
    return out->flags ? OK : ERR_NOT_FOUND;
}

status_t wallclock_check(const struct wall_clock *in, uint64_t uptime_now)
{
    if ((in->flags & ~WALLCLOCK_NET) || in->reserved || in->uptime_ns > uptime_now || in->utc_ns < 0 ||
        in->utc_ns / 1000000000ll >= UTC_MAX_S)
        return ERR_INVALID_ARGS;
    size_t n = 0;
    while (n < WALLCLOCK_ZONE_MAX && in->zone[n])
        n++;
    if (n == WALLCLOCK_ZONE_MAX)
        return ERR_INVALID_ARGS;   /* no NUL */
    for (size_t i = 0; i < n; i++)
        if (in->zone[i] <= ' ' || in->zone[i] > '~')
            return ERR_INVALID_ARGS;
    return OK;
}

status_t wallclock_set(const struct wall_clock *in)
{
    status_t st = wallclock_check(in, uptime_ns());
    if (st != OK)
        return st;
    uint64_t f = spin_lock_irqsave(&lock);
    base_utc = in->utc_ns;
    base_up = in->uptime_ns;
    flags = WALLCLOCK_SET | (in->flags & WALLCLOCK_NET);
    if (in->zone[0])
        memcpy(zone, in->zone, sizeof(zone));
    page_sync_locked();
    char z[WALLCLOCK_ZONE_MAX];
    memcpy(z, zone, sizeof(z));
    spin_unlock_irqrestore(&lock, f);
    int64_t s = in->utc_ns / 1000000000ll;
    kprintf("clock: set to %ld s since 1970 UTC at uptime %lu.%03lu s, zone %s%s\n", (long)s,
            (unsigned long)(in->uptime_ns / 1000000000ull),
            (unsigned long)(in->uptime_ns / 1000000ull % 1000), z[0] ? z : "(none)",
            in->flags & WALLCLOCK_NET ? ", from the network" : "");
    return OK;
}
