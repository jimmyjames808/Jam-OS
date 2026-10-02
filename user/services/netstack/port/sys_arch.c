/* lwIP's system layer for netstack in NO_SYS mode: the clock (sys_now),
 * random numbers (LWIP_RAND) and where lwIP's diagnostics and failed
 * assertions go (port/arch/cc.h names them), with netstack's own log
 * lines (nstack_log, stack.h) beside them. NO_SYS needs nothing else:
 * no threads, semaphores, mailboxes or critical sections, because
 * netstack's one loop is lwIP's only caller.
 *
 * The log is rate-limited here, so nothing lwIP prints can flood it
 * (netstack never logs per packet: docs/M9-PLAN.md "netlog"): a burst of
 * DIAG_BURST lines, then DIAG_PER_S a second; the lines dropped meanwhile
 * are counted and said on the next line that gets through. */
#include <stdarg.h>
#include <os.h>
#include "lwip/sys.h"
#include "stack.h"

#define DIAG_BURST  20u     /* lines at once */
#define DIAG_PER_S  5u      /* lines a second after the burst */

static uint64_t diag_tokens = DIAG_BURST;   /* lines allowed now */
static uint64_t diag_last;                  /* ns: when the tokens were last topped up */
static uint64_t diag_dropped;               /* lines dropped since the last one printed */

/* lwIP's clock: milliseconds since boot. It wraps after 49 days, which
 * lwIP's timers allow for (they compare differences). */
u32_t sys_now(void)
{
    return (u32_t)(now() / NS_PER_MS);
}

/* May a line be logged now? Takes a token if so. */
static bool diag_allowed(void)
{
    uint64_t t = now();
    uint64_t earned = (t - diag_last) * DIAG_PER_S / NS_PER_S;
    if (earned) {
        diag_tokens = diag_tokens + earned > DIAG_BURST ? DIAG_BURST : diag_tokens + earned;
        diag_last = t;
    }
    if (!diag_tokens) {
        diag_dropped++;
        return false;
    }
    diag_tokens--;
    return true;
}

static void log_line(const char *prefix, const char *fmt, va_list ap)
{
    if (!diag_allowed())
        return;
    char line[160];
    vsnprintf(line, sizeof(line), fmt, ap);
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n')   /* lwIP's messages end in one; printf adds its own */
        line[n - 1] = 0;
    if (diag_dropped)
        printf("netstack: %s(%llu lines dropped before this one) %s\n", prefix,
               (unsigned long long)diag_dropped, line);
    else
        printf("netstack: %s%s\n", prefix, line);
    diag_dropped = 0;
}

void nstack_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_line("", fmt, ap);
    va_end(ap);
}

void lwport_diag(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_line("lwip: ", fmt, ap);
    va_end(ap);
}

void lwport_assert(const char *msg, const char *file, int line)
{
    /* Always printed: it is the last line this netstack writes. */
    printf("netstack: lwip assertion failed: %s (%s:%d): ending\n", msg, file, line);
    jam_process_exit(4);
}

/* x86's RDRAND, if the CPU has it (CPUID leaf 1, ECX bit 30). */
static bool rdrand_present(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return c & (1u << 30);
}

static bool rdrand32(uint32_t *out)
{
    for (int tries = 0; tries < 10; tries++) {   /* Intel: retry up to 10 times */
        uint32_t v;
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

/* lwIP's random numbers (LWIP_RAND): a UDP socket's first local port.
 * Not for secrets. RDRAND when the CPU has it; else splitmix64 seeded
 * from the clock. */
uint32_t lwport_random(void)
{
    static int have = -1;   /* -1: not asked yet */
    static uint64_t state;
    if (have < 0) {
        have = rdrand_present();
        state = now();
    }
    uint32_t v;
    if (have && rdrand32(&v))
        return v;
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return (uint32_t)(z ^ (z >> 31));
}
