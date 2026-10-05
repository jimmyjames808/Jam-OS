/* VT-d fault reporting (see vtd_internal.h; Intel VT-d specification 4.1,
 * 7.2, 7.3 and 11.4.7): the fault event interrupt, the ring its handler
 * fills, and the thread that logs.
 *
 * The unit records a fault (a blocked DMA, or a blocked interrupt once
 * remapping is on) in a fault recording register and raises the fault
 * event interrupt, an MSI it sends itself (FEDATA, FEADDR, FEUADDR; never
 * remapped, 5.1.6), routed here to a device vector of its own. It raises
 * another only once software has serviced every condition in FSTS (7.3):
 * the records (cleared one by one, F is write-1-to-clear), the overflow
 * PFO, and the invalidation errors IQE, ICE, ITE (left to the queue's
 * recovery, vtd_qi_check_errors, which must fix the queue before IQE may
 * be cleared).
 *
 * The handler (interrupts off) reads each record holding a fault (the
 * upper half first: F is there, 11.4.7.6), copies it into the unit's ring
 * and clears it, then wakes the log thread. The thread, one for every
 * unit, takes records from the rings and logs them ("vtd: fault: ..."),
 * counts them per requester, logs the first VTD_FAULT_LOGGED of a device
 * and only counts the rest, and puts each device's first fault in the
 * RESULTS box. It also looks at every unit once a second, so a fault that
 * raised no interrupt (an error bit was set at the time) is still seen,
 * and it runs the queue's error recovery when the handler saw an error.
 *
 * The storm guard (VTD_STORM_*, vtd_internal.h). The mute (vtd_domain.c)
 * silences a function's DMA faults in its context entry, but nothing can
 * silence an interrupt's fault (the entry it names is past the table, not
 * present, or someone else's) or a DMA from a requester id no function
 * has, and a driver can point its device's every write at the interrupt
 * window: a fault event interrupt per write, for as long as it likes. So
 * the handler counts its interrupts; past VTD_STORM_IRQS in
 * VTD_STORM_WINDOW_NS it masks the interrupt (FECTL.IM), and the thread
 * then looks at the unit every VTD_STORM_POLL_NS (draining what was
 * recorded meanwhile: one record per register and the overflow) until a
 * look finds nothing new, and unmasks it. The cost of a storm is then at
 * most VTD_STORM_IRQS interrupts per window and one look per poll.
 *
 * Counts per device are the log thread's alone (struct vtd_fault_counts);
 * the ring is "vtd fault ring" (irqsave: the handler takes it). */
#include <jam/dbghook.h>
#include <jam/interrupt.h>
#include <jam/kprintf.h>
#include <jam/report.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "vtd_domain.h"
#include "vtd_internal.h"

#define FAULT_POLL_NS (1000 * NS_PER_MS)   /* the thread's look at every unit */

static struct waitqueue log_wq;
static spinlock_t log_lock = SPINLOCK_INIT("vtd fault log");   /* kicked */
static bool kicked;                    /* something for the thread; log_lock */
static bool thread_started;            /* only at boot (vtd_units_start), one CPU */
static struct vtd_fault_counts counts; /* the log thread's */

const struct vtd_fault_counts *vtd_fault_counts_get(void)
{
    return &counts;   /* the log thread writes it; a reader (the iommu command) races benignly */
}

/* ---- counting and the words (pure) ---------------------------------------------- */

const char *vtd_fault_reason_words(uint32_t reason)
{
    /* Table 26 (legacy-mode DMA faults) and Table 13 (interrupt remapping). */
    switch (reason) {
    case 0x01: return "root entry not present";
    case 0x02: return "context entry not present";
    case 0x03: return "context entry invalid";
    case 0x04: return "address above the domain's width";
    case 0x05: return "write to a page not mapped writable";
    case 0x06: return "read from a page not mapped readable";
    case 0x07: return "page-table access error";
    case 0x0c: return "reserved bit in a page-table entry";
    case 0x0d: return "translation type blocks the request";
    case 0x20: return "reserved bit in the interrupt request";
    case 0x21: return "interrupt index past the table";
    case 0x22: return "interrupt entry not present";
    case 0x24: return "reserved bit in the interrupt entry";
    case 0x25: return "compatibility-format interrupt blocked";
    case 0x26: return "interrupt from a requester the entry doesn't allow";
    default:   return NULL;
    }
}

uint64_t vtd_fault_counts_add(struct vtd_fault_counts *c, uint32_t unit, uint16_t sid)
{
    for (uint32_t i = 0; i < c->ndev; i++)
        if (c->dev[i].unit == unit && c->dev[i].sid == sid)
            return ++c->dev[i].count;
    if (c->ndev == VTD_FAULT_DEVS) {
        c->other++;
        return 0;
    }
    c->dev[c->ndev] = (struct vtd_fault_dev){ .unit = unit, .sid = sid, .count = 1 };
    c->ndev++;
    return 1;
}

void vtd_fault_line(char *buf, size_t n, uint32_t unit, uint64_t lo, uint64_t hi)
{
    char what[96];
    vtd_describe_fault(what, sizeof(what), lo, hi);
    const char *words = vtd_fault_reason_words((uint32_t)VTD_FRCD_REASON(hi));
    ksnprintf(buf, n, "vtd: fault: unit %u: %s%s%s", unit, what, words ? ": " : "",
              words ? words : "");
}

/* ---- the handler ---------------------------------------------------------------- */

/* Copy every recorded fault into the ring and clear it, count an overflow
 * and clear it; note invalidation errors for the thread. ring_lock held. */
static void drain_locked(struct vtd_unit *u)
{
    uint32_t fsts = vtd_rd32(u, VTD_FSTS);
    uint32_t fro = (uint32_t)VTD_CAP_FRO(u->cap) * 16, nfr = (uint32_t)VTD_CAP_NFR(u->cap) + 1;
    uint32_t first = (uint32_t)VTD_FSTS_FRI(fsts);
    for (uint32_t k = 0; k < nfr && (fsts & VTD_FSTS_PPF); k++) {
        uint32_t off = fro + 16 * ((first + k) % nfr);
        if (off + 16 > u->span)
            break;
        uint64_t hi = vtd_rd64(u, off + 8);   /* F first (11.4.7.6) */
        if (!(hi & VTD_FRCD_F))
            continue;
        uint64_t lo = vtd_rd64(u, off);
        if (u->rtail - u->rhead < VTD_FAULT_RING)
            u->ring[u->rtail++ % VTD_FAULT_RING] = (struct vtd_fault_rec){ lo, hi };
        else
            __atomic_add_fetch(&u->stats.faults_lost, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&u->stats.faults, 1, __ATOMIC_RELAXED);
        vtd_wr32(u, off + 12, 1u << 31);   /* F is bit 127: write 1 to clear */
    }
    if (fsts & VTD_FSTS_PFO) {
        __atomic_add_fetch(&u->stats.faults_lost, 1, __ATOMIC_RELAXED);
        vtd_wr32(u, VTD_FSTS, VTD_FSTS_PFO);
    }
    u->inv_errs |= fsts & (VTD_FSTS_IQE | VTD_FSTS_ICE | VTD_FSTS_ITE);
}

static void wake_thread(void)
{
    uint64_t f = spin_lock_irqsave(&log_lock);
    kicked = true;
    spin_unlock_irqrestore(&log_lock, f);
    waitqueue_wake_one(&log_wq);
}

/* What raises the fault event interrupt (7.3), counted so far: fault
 * records read, faults lost (the overflow, a full ring), and the queue's
 * errors (a refused descriptor, device-TLB errors). */
static uint64_t storm_events(const struct vtd_unit *u)
{
    const struct vtd_unit_stats *s = &u->stats;
    return __atomic_load_n(&s->faults, __ATOMIC_RELAXED) +
           __atomic_load_n(&s->faults_lost, __ATOMIC_RELAXED) +
           __atomic_load_n(&s->refused, __ATOMIC_RELAXED) +
           __atomic_load_n(&s->ite, __ATOMIC_RELAXED) + __atomic_load_n(&s->ice, __ATOMIC_RELAXED);
}

/* Count one fault event interrupt in the current window; past
 * VTD_STORM_IRQS, mask the interrupt (the log thread takes over). ring_lock
 * held. */
static void storm_count_locked(struct vtd_unit *u, uint64_t now)
{
    if (u->storm_masked)
        return;
    if (now - u->storm_window >= VTD_STORM_WINDOW_NS) {
        u->storm_window = now;
        u->storm_irqs = 0;
    }
    if (++u->storm_irqs <= VTD_STORM_IRQS)
        return;
    vtd_fault_mask(u);
    __atomic_store_n(&u->storm_masked, true, __ATOMIC_RELAXED);
    u->storm_look = now;
    u->storm_seen = storm_events(u);
    __atomic_add_fetch(&u->stats.storms, 1, __ATOMIC_RELAXED);
}

/* The log thread's look at a unit the guard masked, VTD_STORM_POLL_NS
 * after the last: when no new event came since, the storm is over
 * and the interrupt is unmasked (a fault held pending meanwhile raises it
 * at once, 7.3). True when it unmasked. ring_lock held, the records just
 * drained. */
static bool storm_look_locked(struct vtd_unit *u, uint64_t now)
{
    if (!u->storm_masked || now - u->storm_look < VTD_STORM_POLL_NS)
        return false;
    uint64_t seen = storm_events(u);
    u->storm_look = now;
    if (seen != u->storm_seen) {
        u->storm_seen = seen;
        return false;
    }
    __atomic_store_n(&u->storm_masked, false, __ATOMIC_RELAXED);
    u->storm_window = now;
    u->storm_irqs = 0;
    uint32_t c = vtd_rd32(u, VTD_FECTL);
    vtd_wr32(u, VTD_FECTL, c & ~(VTD_FECTL_IM | VTD_FECTL_IP));
    return true;
}

static void fault_irq(void *ctx)
{
    struct vtd_unit *u = ctx;
    __atomic_add_fetch(&u->stats.fault_irqs, 1, __ATOMIC_RELAXED);
    uint64_t f = spin_lock_irqsave(&u->ring_lock);
    drain_locked(u);
    storm_count_locked(u, uptime_ns());
    spin_unlock_irqrestore(&u->ring_lock, f);
    wake_thread();
}

void vtd_fault_kick(struct vtd_unit *u)
{
    if (!__atomic_load_n(&u->fault_on, __ATOMIC_ACQUIRE))
        return;
    uint64_t f = spin_lock_irqsave(&u->ring_lock);
    drain_locked(u);
    spin_unlock_irqrestore(&u->ring_lock, f);
    wake_thread();
}

/* ---- the log thread --------------------------------------------------------------- */

static void log_one(uint32_t unit, struct vtd_fault_rec r)
{
    char line[192];
    vtd_fault_line(line, sizeof(line), unit, r.lo, r.hi);
    uint16_t sid = (uint16_t)VTD_FRCD_SID(r.hi);
    uint64_t n = vtd_fault_counts_add(&counts, unit, sid);
    vtd_domain_fault_seen(unit, sid, (uint32_t)VTD_FRCD_REASON(r.hi));   /* the mute */
    if (n == 1)
        report("%s (the first from %02x:%02x.%x)", line, sid >> 8, (sid >> 3) & 0x1f, sid & 7);
    else if (n <= VTD_FAULT_LOGGED)
        kprintf("%s\n", line);
    if (n == VTD_FAULT_LOGGED)
        kprintf("vtd: fault: unit %u: %02x:%02x.%x has faulted %lu times: its next faults are "
                "counted, not logged\n", unit, sid >> 8, (sid >> 3) & 0x1f, sid & 7, n);
}

/* Log what u's ring holds, and run the queue's recovery if the handler saw
 * an error. */
static void service(struct vtd_unit *u)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&u->ring_lock);
        uint32_t errs = u->inv_errs;
        u->inv_errs = 0;
        bool have = u->rhead != u->rtail;
        struct vtd_fault_rec r = { 0, 0 };
        if (have)
            r = u->ring[u->rhead++ % VTD_FAULT_RING];
        spin_unlock_irqrestore(&u->ring_lock, f);
        if (errs)
            vtd_qi_check_errors(u);
        if (!have)
            return;
        DBG_HOOK(DBG_VTD_FAULT, &r);   /* the tests' look at each fault */
        log_one(u->index, r);
    }
}

/* Is a unit's interrupt masked by the storm guard (the thread looks more
 * often then)? */
static bool storming(void)
{
    for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
        struct vtd_unit *u = vtd_unit_get(i);
        if (u && __atomic_load_n(&u->storm_masked, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

/* Say a storm the handler found (the first in the RESULTS box, the next
 * few in the log, the rest only counted), and its end. */
static void say_storm(struct vtd_unit *u, bool over)
{
    uint64_t n = __atomic_load_n(&u->stats.storms, __ATOMIC_RELAXED);
    if (n != u->storms_said) {
        u->storms_said = n;
        if (n == 1)
            report("vtd: fault: unit %u: fault storm (over %u interrupts in %u ms): masked, "
                   "polled every %u ms", u->index, VTD_STORM_IRQS,
                   (unsigned)(VTD_STORM_WINDOW_NS / NS_PER_MS),
                   (unsigned)(VTD_STORM_POLL_NS / NS_PER_MS));
        else if (n <= VTD_FAULT_LOGGED)
            kprintf("vtd: fault: unit %u: fault storm %lu: the interrupt is masked again\n",
                    u->index, n);
    }
    if (over && n <= VTD_FAULT_LOGGED)
        kprintf("vtd: fault: unit %u: the fault storm is over (%lu fault(s) read, %lu lost so "
                "far): the interrupt is unmasked\n", u->index,
                __atomic_load_n(&u->stats.faults, __ATOMIC_RELAXED),
                __atomic_load_n(&u->stats.faults_lost, __ATOMIC_RELAXED));
}

static void log_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&log_lock);
        uint64_t deadline = uptime_ns() + (storming() ? VTD_STORM_POLL_NS : FAULT_POLL_NS);
        while (!kicked && uptime_ns() < deadline)
            waitqueue_wait_until(&log_wq, &log_lock, &f, deadline);
        kicked = false;
        spin_unlock_irqrestore(&log_lock, f);
        for (uint32_t i = 0; i < VTD_MAX_UNITS; i++) {
            struct vtd_unit *u = vtd_unit_get(i);
            if (!u || !__atomic_load_n(&u->fault_on, __ATOMIC_ACQUIRE))
                continue;
            uint64_t g = spin_lock_irqsave(&u->ring_lock);
            drain_locked(u);   /* the once-a-second look (in a storm, every poll) */
            bool over = storm_look_locked(u, uptime_ns());
            spin_unlock_irqrestore(&u->ring_lock, g);
            say_storm(u, over);
            service(u);
        }
    }
}

/* ---- setting it up ------------------------------------------------------------------ */

void vtd_fault_mask(struct vtd_unit *u)
{
    uint32_t c = vtd_rd32(u, VTD_FECTL);
    vtd_wr32(u, VTD_FECTL, (c & ~VTD_FECTL_IP) | VTD_FECTL_IM);
}

status_t vtd_fault_init(struct vtd_unit *u)
{
    spin_init(&u->ring_lock, "vtd fault ring");
    if (!thread_started) {
        waitqueue_init(&log_wq, "vtd fault log wait");
        struct thread *t = thread_try_create_on("vtd faults", log_main, NULL, PRIO_DEFAULT, NULL);
        if (!t)
            return ERR_NO_MEMORY;
        thread_detach(t);
        thread_started = true;
    }
    uint32_t cpu;
    uint8_t vec;
    status_t st = vector_alloc(fault_irq, u, &cpu, &vec);
    if (st != OK)
        return st;
    u->fault_cpu = cpu;
    u->fault_vec = vec;
    /* 5.1.6.1/5.1.6.2: data = the vector, fixed delivery; address = 0xfee
     * with the APIC id in 19:12 (vector_alloc picks CPUs whose ids fit
     * 8 bits), no redirection hint, physical mode; upper address = the
     * id's bits 31:8, so 0. Then the mask off (FECTL 29:0 are RsvdP). */
    vtd_fault_mask(u);
    vtd_wr32(u, VTD_FEDATA, msi_data(vec));
    vtd_wr32(u, VTD_FEADDR, (uint32_t)msi_address(cpu));
    vtd_wr32(u, VTD_FEUADDR, 0);
    __atomic_store_n(&u->fault_on, true, __ATOMIC_RELEASE);
    uint32_t c = vtd_rd32(u, VTD_FECTL);
    vtd_wr32(u, VTD_FECTL, c & ~(VTD_FECTL_IM | VTD_FECTL_IP));
    /* Faults recorded before (the firmware's, or a kexec'd kernel's) raise
     * the held interrupt now, or are found here. */
    vtd_fault_kick(u);
    return OK;
}
