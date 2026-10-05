/* The VT-d invalidation queue and the invalidations built on it (see
 * vtd_internal.h; Intel VT-d specification 4.1, 6.5.2 and 11.4.9).
 *
 * The queue is a ring of VTD_QI_ENTRIES 128-bit descriptors. Software
 * writes descriptors at the tail and moves IQT past them; the unit reads
 * from IQH and moves it. The ring is full when the tail is one behind the
 * head, so at most VTD_QI_ENTRIES - 1 descriptors are outstanding (6.5.2).
 *
 * A submission (vtd_qi_submit) is a batch of descriptors and one
 * invalidation wait descriptor behind them. Under "vtd queue" the caller
 * takes a slot (a status word in the status page), checks there is room
 * for the batch, writes it, the wait (status write of a fresh sequence
 * number to its slot, fence flag set) and IQT, and drops the lock; then
 * it polls its own word, with no lock held, until the number appears or
 * VTD_QI_WAIT_NS has passed since it started. So many CPUs can wait at
 * once and none waits behind another's poll.
 *
 * Why a waiter's word can't be fooled: the unit completes a wait only
 * after every descriptor between it and the previous wait (6.5.2.11), and
 * with the fence flag it processes nothing behind a wait until that wait
 * completed, so waits complete in queue order. A slot freed after a time-
 * out may still get its old wait's late write: that write comes before the
 * next owner's own (its wait is behind), and the old number never equals
 * the new owner's.
 *
 * Errors (6.5.2.10): with IQE set the unit stops fetching at the refused
 * descriptor (IQH points at it). Whoever sees IQE first (a waiter, or the
 * fault log thread after the fault event) takes the lock, marks the slot
 * whose batch wrote that descriptor as refused, puts a harmless stand-in
 * there (a wait that writes the scratch word), clears IQE, and the queue
 * goes on with the rest. The refused batch's own wait still completes, and
 * its caller gets ERR_IO. ITE and ICE come only from device-TLB
 * invalidations, which Jam OS never sends: reported and cleared. */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/vtd.h>
#include <jam/x86.h>

#include "vtd_internal.h"

#define FSTS_INV_ERRS (VTD_FSTS_IQE | VTD_FSTS_ICE | VTD_FSTS_ITE)

/* ---- descriptors (pure) ------------------------------------------------------- */

struct vtd_desc vtd_desc_cc(enum vtd_cc_g g, uint16_t did, uint16_t sid, uint8_t fm)
{
    /* 6.5.2.1, Figure 6-1: G 5:4, DID 31:16, SID 47:32, FM 49:48. */
    return (struct vtd_desc){
        .lo = VTD_DESC_CC | ((uint64_t)g & 3) << 4 | (uint64_t)did << 16 |
              (uint64_t)sid << 32 | ((uint64_t)fm & 3) << 48,
        .hi = 0,
    };
}

struct vtd_desc vtd_desc_iotlb(enum vtd_iotlb_g g, uint16_t did, unsigned drain, uint64_t addr,
                               unsigned am, bool ih)
{
    /* 6.5.2.3, Figure 6-3: G 5:4, DW 6, DR 7, DID 31:16; in the upper
     * half AM 69:64, IH 70, ADDR 127:76 (the page address, 63:12). */
    uint64_t lo = VTD_DESC_IOTLB | ((uint64_t)g & 3) << 4 | (uint64_t)did << 16;
    if (drain & 2)
        lo |= 1ull << 6;   /* DW */
    if (drain & 1)
        lo |= 1ull << 7;   /* DR */
    uint64_t hi = 0;
    if (g == VTD_IOTLB_PAGE)
        hi = (addr & ~0xfffull) | (ih ? 1ull << 6 : 0) | (am & 0x3f);
    return (struct vtd_desc){ lo, hi };
}

struct vtd_desc vtd_desc_iec(bool global, uint32_t index, unsigned im)
{
    /* 6.5.2.7, Figure 6-7: G bit 4 (0 global, 1 index-selective), IM
     * 31:27, IIDX 47:32. */
    if (global)
        return (struct vtd_desc){ VTD_DESC_IEC, 0 };
    return (struct vtd_desc){
        .lo = VTD_DESC_IEC | 1ull << 4 | ((uint64_t)im & 0x1f) << 27 |
              ((uint64_t)index & 0xffff) << 32,
        .hi = 0,
    };
}

struct vtd_desc vtd_desc_wait(uint64_t addr, uint32_t data)
{
    /* 6.5.2.8, Figure 6-8: IF 4 (no), SW 5, FN 6, status data 63:32; the
     * status address 127:66 (bits 63:2 of a 4-byte aligned address). */
    return (struct vtd_desc){
        .lo = VTD_DESC_WAIT | 1ull << 5 | 1ull << 6 | (uint64_t)data << 32,
        .hi = addr & ~3ull,
    };
}

int vtd_split_aligned(uint64_t first, uint64_t n, unsigned max_order, struct vtd_block *out,
                      unsigned cap)
{
    if (max_order > 63)
        max_order = 63;
    unsigned count = 0;
    while (n) {
        unsigned order = max_order;
        if (first && (unsigned)__builtin_ctzll(first) < order)
            order = (unsigned)__builtin_ctzll(first);
        while ((1ull << order) > n)
            order--;
        if (count == cap)
            return -1;
        out[count++] = (struct vtd_block){ first, order };
        first += 1ull << order;
        n -= 1ull << order;
    }
    return (int)count;
}

/* ---- the queue's registers ------------------------------------------------------ */

static uint32_t head_index(const struct vtd_unit *u)
{
    return (uint32_t)(vtd_rd64(u, VTD_IQH) >> VTD_IQ_SHIFT) & (VTD_QI_ENTRIES - 1);
}

static void write_tail(struct vtd_unit *u)
{
    vtd_wr64(u, VTD_IQT, (uint64_t)u->tail << VTD_IQ_SHIFT);
}

/* Wait (bounded) until a register-based invalidation the firmware may have
 * started is done: queued invalidation may only be turned on with none
 * pending (6.5.2). */
static status_t wait_register_invalidations(struct vtd_unit *u)
{
    uint32_t iotlb = (uint32_t)VTD_ECAP_IRO(u->ecap) * 16 + VTD_IOTLB_REG_OFF;
    bool has_iotlb = iotlb + 8 <= u->span;
    uint64_t deadline = uptime_ns() + VTD_REG_WAIT_NS;
    for (;;) {
        bool busy = (vtd_rd64(u, VTD_CCMD) & VTD_CCMD_ICC) ||
                    (has_iotlb && (vtd_rd64(u, iotlb) & VTD_IOTLB_IVT));
        if (!busy)
            return OK;
        if (uptime_ns() > deadline)
            return ERR_TIMED_OUT;
        cpu_relax();
    }
}

/* Wait (bounded) until the unit has fetched everything up to the tail. */
static status_t wait_drained(struct vtd_unit *u)
{
    uint64_t deadline = uptime_ns() + VTD_QI_WAIT_NS;
    while (vtd_rd64(u, VTD_IQH) != vtd_rd64(u, VTD_IQT)) {
        if (uptime_ns() > deadline)
            return ERR_TIMED_OUT;
        cpu_relax();
    }
    return OK;
}

/* ---- turning it on and off ------------------------------------------------------ */

/* A queue someone else left on (the firmware, or a kexec'd kernel): it
 * must be drained before it is turned off (6.5.2). One stopped by an error
 * isn't drained: left alone. */
static status_t take_over(struct vtd_unit *u)
{
    uint32_t fsts = vtd_rd32(u, VTD_FSTS);
    if ((fsts & (VTD_FSTS_IQE | VTD_FSTS_ITE)) && vtd_rd64(u, VTD_IQH) != vtd_rd64(u, VTD_IQT))
        return ERR_BAD_STATE;
    status_t st = wait_drained(u);
    if (st != OK)
        return ERR_BAD_STATE;
    kprintf("vtd:         unit %u: queued invalidation was on (head %lx): drained, "
            "turned off and set up again\n", u->index, vtd_rd64(u, VTD_IQH));
    return vtd_gcmd(u, VTD_GCMD_QIE, false, VTD_GSTS_QIES);
}

status_t vtd_qi_enable(struct vtd_unit *u)
{
    /* 6.5.2: tail 0, then the address, size (2^0 pages) and width (128-bit
     * descriptors), then QIE. The unit resets the head to 0 when QI goes
     * off. */
    vtd_wr64(u, VTD_IQT, 0);
    vtd_wr64(u, VTD_IQA, u->queue_phys);
    status_t st = vtd_gcmd(u, VTD_GCMD_QIE, true, VTD_GSTS_QIES);
    if (st != OK)
        return st;
    spin_lock(&u->qlock);
    u->tail = 0;
    u->qi_on = true;
    spin_unlock(&u->qlock);
    return OK;
}

status_t vtd_qi_disable(struct vtd_unit *u)
{
    spin_lock(&u->qlock);
    u->qi_on = false;
    status_t st = wait_drained(u);
    if (st == OK)
        st = vtd_gcmd(u, VTD_GCMD_QIE, false, VTD_GSTS_QIES);
    if (st != OK)
        u->qi_on = true;   /* still on: submissions may go on */
    spin_unlock(&u->qlock);
    return st;
}

status_t vtd_qi_init(struct vtd_unit *u)
{
    if (!VTD_ECAP_QI(u->ecap))
        return ERR_NOT_SUPPORTED;
    spin_init(&u->qlock, "vtd queue");
    uint64_t q = pmm_alloc_page_phys(PMM_ZERO), s = pmm_alloc_page_phys(PMM_ZERO);
    if (!q || !s) {
        if (q)
            pmm_free_page_phys(q);
        if (s)
            pmm_free_page_phys(s);
        return ERR_NO_MEMORY;
    }
    u->queue_phys = q;
    u->queue = phys_to_virt(q);
    u->status_phys = s;
    u->status = phys_to_virt(s);
    status_t st = wait_register_invalidations(u);
    if (st == OK && (vtd_rd32(u, VTD_GSTS) & VTD_GSTS_QIES))
        st = take_over(u);
    if (st == OK) {
        /* Errors left from before stop the new queue's fetches (6.5.2):
         * nothing of ours is in it yet, so they are cleared, and said. */
        uint32_t old = vtd_rd32(u, VTD_FSTS) & FSTS_INV_ERRS;
        if (old) {
            kprintf("vtd:         unit %u: invalidation errors %x left from before: cleared\n",
                    u->index, old);
            vtd_wr32(u, VTD_FSTS, old);
        }
        st = vtd_qi_enable(u);
    }
    return st;   /* on failure the two pages stay with the unit, unused */
}

/* ---- submitting ------------------------------------------------------------------ */

static uint32_t room_locked(const struct vtd_unit *u)
{
    return (head_index(u) - u->tail - 1) & (VTD_QI_ENTRIES - 1);
}

static int slot_take_locked(struct vtd_unit *u)
{
    for (unsigned w = 0; w < VTD_QI_SLOTS / 64; w++) {
        if (u->slot_used[w] == ~0ull)
            continue;
        unsigned b = (unsigned)__builtin_ctzll(~u->slot_used[w]);
        u->slot_used[w] |= 1ull << b;
        return (int)(w * 64 + b);
    }
    return -1;
}

static void put_locked(struct vtd_unit *u, struct vtd_desc d, int slot)
{
    struct vtd_desc *e = &u->queue[u->tail];
    e->lo = d.lo;
    e->hi = d.hi;
    u->owner[u->tail] = (uint8_t)slot;
    u->tail = (u->tail + 1) & (VTD_QI_ENTRIES - 1);
    if (u->tail == 0)
        __atomic_add_fetch(&u->stats.wraps, 1, __ATOMIC_RELAXED);
}

/* Write the batch and its wait if there is a slot and room: OK with the
 * slot and the sequence number its wait writes; ERR_SHOULD_WAIT; or
 * ERR_BAD_STATE (the queue is off). */
static status_t write_batch_locked(struct vtd_unit *u, const struct vtd_desc *d, uint32_t n,
                                   int *out_slot, uint32_t *out_seq)
{
    if (!u->qi_on)
        return ERR_BAD_STATE;
    if (room_locked(u) < n + 1)
        return ERR_SHOULD_WAIT;
    int slot = slot_take_locked(u);
    if (slot < 0)
        return ERR_SHOULD_WAIT;
    uint32_t seq = ++u->seq;
    if (seq == 0)
        seq = ++u->seq;   /* 0 is what a fresh slot holds */
    u->slot_refused[slot] = false;
    __atomic_store_n(&u->status[slot], 0, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; i++)
        put_locked(u, d[i], slot);
    put_locked(u, vtd_desc_wait(u->status_phys + 4ull * (unsigned)slot, seq), slot);
    /* The descriptors are in RAM the unit snoops (11.4.3, ECAP.C: the queue
     * is always snooped); the tail write is ordered after them (x86 keeps
     * stores in order, and the register is uncached). */
    write_tail(u);
    __atomic_add_fetch(&u->stats.submissions, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&u->stats.descriptors, n + 1, __ATOMIC_RELAXED);
    *out_slot = slot;
    *out_seq = seq;
    return OK;
}

static void timed_out(struct vtd_unit *u, const char *what)
{
    uint64_t n = __atomic_add_fetch(&u->stats.timeouts, 1, __ATOMIC_RELAXED);
    uint64_t h = vtd_rd64(u, VTD_IQH), t = vtd_rd64(u, VTD_IQT);
    uint32_t fsts = vtd_rd32(u, VTD_FSTS);
    if (n == 1)
        report("vtd: unit %u: %s for over %u ms (head %lx tail %lx fsts %x)", u->index, what,
               VTD_WAIT_MS, h, t, fsts);
    else
        kprintf("vtd: unit %u: %s for over %u ms (head %lx tail %lx fsts %x; %lu times)\n",
                u->index, what, VTD_WAIT_MS, h, t, fsts, n);
}

static status_t wait_slot(struct vtd_unit *u, int slot, uint32_t seq, uint64_t deadline)
{
    for (;;) {
        if (__atomic_load_n(&u->status[slot], __ATOMIC_ACQUIRE) == seq)
            return OK;
        if (vtd_rd32(u, VTD_FSTS) & FSTS_INV_ERRS)
            vtd_qi_check_errors(u);
        if (uptime_ns() > deadline) {
            if (__atomic_load_n(&u->status[slot], __ATOMIC_ACQUIRE) == seq)
                return OK;
            timed_out(u, "an invalidation wait did not complete");
            return ERR_TIMED_OUT;
        }
        cpu_relax();
    }
}

status_t vtd_qi_submit(struct vtd_unit *u, const struct vtd_desc *d, uint32_t n)
{
    if (n == 0 || n > VTD_QI_BATCH_MAX)
        return ERR_INVALID_ARGS;
    uint64_t deadline = uptime_ns() + VTD_QI_WAIT_NS;
    int slot;
    uint32_t seq;
    for (;;) {
        spin_lock(&u->qlock);
        status_t st = write_batch_locked(u, d, n, &slot, &seq);
        spin_unlock(&u->qlock);
        if (st == OK)
            break;
        if (st != ERR_SHOULD_WAIT)
            return st;
        if (uptime_ns() > deadline) {
            timed_out(u, "the queue stayed full");
            return ERR_TIMED_OUT;
        }
        if (vtd_rd32(u, VTD_FSTS) & FSTS_INV_ERRS)
            vtd_qi_check_errors(u);   /* a stopped queue never drains by itself */
        cpu_relax();
    }
    status_t st = wait_slot(u, slot, seq, deadline);
    spin_lock(&u->qlock);
    bool refused = u->slot_refused[slot];
    u->slot_used[slot / 64] &= ~(1ull << (slot % 64));
    spin_unlock(&u->qlock);
    if (st == OK && refused)
        st = ERR_IO;
    return st;
}

/* ---- errors ---------------------------------------------------------------------- */

/* What one recovery found, to log once the lock is dropped. */
struct inv_error {
    uint32_t        fsts;     /* the error bits handled */
    uint32_t        head;     /* the refused descriptor's index */
    uint32_t        iqei;     /* IQERCD.IQEI */
    uint64_t        iqercd;
    struct vtd_desc bad;      /* the refused descriptor */
};

/* IQE: stand a harmless wait in for the refused descriptor, mark its batch,
 * let the unit go on (6.5.2.10). */
static void refused_locked(struct vtd_unit *u, struct inv_error *e)
{
    e->head = head_index(u);
    e->iqercd = vtd_rd64(u, VTD_IQERCD);
    e->iqei = (uint32_t)VTD_IQERCD_IQEI(e->iqercd);
    e->bad = u->queue[e->head];
    u->slot_refused[u->owner[e->head]] = true;
    u->last_iqei = e->iqei;
    struct vtd_desc stand_in = vtd_desc_wait(u->status_phys + 4ull * VTD_QI_SCRATCH, 0);
    u->queue[e->head].lo = stand_in.lo;
    u->queue[e->head].hi = stand_in.hi;
    vtd_wr32(u, VTD_FSTS, VTD_FSTS_IQE);
    /* A unit fetches again by itself once IQE is clear; rewriting the same
     * tail changes nothing on one that does, and wakes one that waits for
     * a tail write to look at the queue again (QEMU's). */
    write_tail(u);
    __atomic_add_fetch(&u->stats.refused, 1, __ATOMIC_RELAXED);
}

static void log_error(struct vtd_unit *u, const struct inv_error *e)
{
    if (e->fsts & VTD_FSTS_IQE) {
        static const char *const why[] = {
            "no detail", "a bad tail", "a fetch error", "an invalid type",
            "a reserved field set", "a bad descriptor width", "a misaligned tail",
        };
        const char *w = e->iqei < sizeof(why) / sizeof(why[0]) ? why[e->iqei] : "?";
        /* The first in the RESULTS box, the next few logged, the rest
         * counted (the `iommu` command): a run of them can't flood the log. */
        uint64_t n = __atomic_load_n(&u->stats.refused, __ATOMIC_RELAXED);
        bool first = n == 1;
        const char *fmt = "vtd: unit %u: the queue refused descriptor %u (%016lx %016lx): %s "
                          "(iqei %u)%s";
        char line[192];
        ksnprintf(line, sizeof(line), fmt, u->index, e->head, e->bad.hi, e->bad.lo, w, e->iqei,
                  first ? "" : "; logged only");
        if (first)
            report("%s", line);
        else if (n <= VTD_FAULT_LOGGED)
            kprintf("%s\n", line);
        if (n == VTD_FAULT_LOGGED)
            kprintf("vtd: unit %u: the queue refused %lu descriptors: the next are counted, not "
                    "logged\n", u->index, n);
    }
    if (e->fsts & (VTD_FSTS_ITE | VTD_FSTS_ICE))
        report("vtd: unit %u: device-TLB invalidation error (fsts %x, iqercd %lx), never "
               "expected: no device-TLB invalidation is sent", u->index, e->fsts, e->iqercd);
}

void vtd_qi_check_errors(struct vtd_unit *u)
{
    struct inv_error e = { 0 };
    spin_lock(&u->qlock);
    uint32_t fsts = vtd_rd32(u, VTD_FSTS) & FSTS_INV_ERRS;
    if (fsts & VTD_FSTS_IQE)
        refused_locked(u, &e);
    if (fsts & (VTD_FSTS_ITE | VTD_FSTS_ICE)) {
        if (!(fsts & VTD_FSTS_IQE))
            e.iqercd = vtd_rd64(u, VTD_IQERCD);
        /* ITE also drops the waits the unit held (6.5.2.10): their callers
         * time out. Every batch in the queue is marked failed. */
        if (fsts & VTD_FSTS_ITE)
            memset(u->slot_refused, 1, sizeof(u->slot_refused));
        vtd_wr32(u, VTD_FSTS, fsts & (VTD_FSTS_ITE | VTD_FSTS_ICE));
        __atomic_add_fetch((fsts & VTD_FSTS_ITE) ? &u->stats.ite : &u->stats.ice, 1,
                           __ATOMIC_RELAXED);
    }
    spin_unlock(&u->qlock);
    if (!fsts)
        return;
    e.fsts = fsts;
    log_error(u, &e);
    /* Faults recorded while an error bit was set raised no interrupt (7.3). */
    vtd_fault_kick(u);
}

/* ---- the invalidations ------------------------------------------------------------ */

static status_t check_did(const struct vtd_unit *u, uint16_t did)
{
    uint64_t domains = 1ull << (4 + 2 * VTD_CAP_ND(u->cap));
    return did < domains ? OK : ERR_OUT_OF_RANGE;
}

/* DR and DW where the unit offers them (CAP.DRD, CAP.DWD): reads and
 * writes it already translated reach memory before the next wait
 * completes (6.5.4). Version 2 units drain always. */
static unsigned drain_of(const struct vtd_unit *u)
{
    return (VTD_CAP_DRD(u->cap) ? 1u : 0u) | (VTD_CAP_DWD(u->cap) ? 2u : 0u);
}

static status_t submit1(struct vtd_unit *u, struct vtd_desc d)
{
    return vtd_qi_submit(u, &d, 1);
}

status_t vtd_inv_context_global(struct vtd_unit *u)
{
    return submit1(u, vtd_desc_cc(VTD_CC_GLOBAL, 0, 0, 0));
}

status_t vtd_inv_context_domain(struct vtd_unit *u, uint16_t did)
{
    status_t st = check_did(u, did);
    return st != OK ? st : submit1(u, vtd_desc_cc(VTD_CC_DOMAIN, did, 0, 0));
}

status_t vtd_inv_context_device(struct vtd_unit *u, uint16_t did, uint16_t sid, uint8_t fm)
{
    status_t st = check_did(u, did);
    if (st == OK && fm > 3)
        st = ERR_INVALID_ARGS;
    return st != OK ? st : submit1(u, vtd_desc_cc(VTD_CC_DEVICE, did, sid, fm));
}

status_t vtd_inv_iotlb_global(struct vtd_unit *u)
{
    return submit1(u, vtd_desc_iotlb(VTD_IOTLB_GLOBAL, 0, drain_of(u), 0, 0, false));
}

status_t vtd_inv_iotlb_domain(struct vtd_unit *u, uint16_t did)
{
    status_t st = check_did(u, did);
    return st != OK ? st
                    : submit1(u, vtd_desc_iotlb(VTD_IOTLB_DOMAIN, did, drain_of(u), 0, 0, false));
}

/* Page-selective descriptors for [iova, + pages) appended to d[*n..]: false
 * if the unit has no PSI or they don't fit in VTD_QI_BATCH_MAX. */
static bool add_pages(const struct vtd_unit *u, uint16_t did, uint64_t iova, uint64_t pages,
                      bool keep_tables, struct vtd_desc *d, uint32_t *n)
{
    if (!VTD_CAP_PSI(u->cap))
        return false;
    struct vtd_block b[VTD_QI_BATCH_MAX];
    int k = vtd_split_aligned(iova >> PAGE_SHIFT, pages, (unsigned)VTD_CAP_MAMV(u->cap), b,
                              VTD_QI_BATCH_MAX - *n);
    if (k < 0)
        return false;
    for (int i = 0; i < k; i++)
        d[(*n)++] = vtd_desc_iotlb(VTD_IOTLB_PAGE, did, drain_of(u), b[i].first << PAGE_SHIFT,
                                   b[i].order, keep_tables);
    return true;
}

status_t vtd_inv_iotlb_pages(struct vtd_unit *u, uint16_t did, uint64_t iova, uint64_t pages,
                             bool keep_tables)
{
    status_t st = check_did(u, did);
    if (st != OK)
        return st;
    if (pages == 0)
        return OK;
    struct vtd_desc d[VTD_QI_BATCH_MAX];
    uint32_t n = 0;
    if (!add_pages(u, did, iova, pages, keep_tables, d, &n))
        return vtd_inv_iotlb_domain(u, did);
    return vtd_qi_submit(u, d, n);
}

status_t vtd_inv_iec_global(struct vtd_unit *u)
{
    return submit1(u, vtd_desc_iec(true, 0, 0));
}

status_t vtd_inv_iec_index(struct vtd_unit *u, uint32_t index, uint32_t count)
{
    if (index > 0xffff || count == 0 || count > 0x10000 - index)
        return ERR_OUT_OF_RANGE;
    struct vtd_block b[VTD_QI_BATCH_MAX];
    int k = vtd_split_aligned(index, count, (unsigned)VTD_ECAP_MHMV(u->ecap), b,
                              VTD_QI_BATCH_MAX);
    if (k < 0)
        return vtd_inv_iec_global(u);
    struct vtd_desc d[VTD_QI_BATCH_MAX];
    for (int i = 0; i < k; i++)
        d[i] = vtd_desc_iec(false, (uint32_t)b[i].first, b[i].order);
    return vtd_qi_submit(u, d, (uint32_t)k);
}

/* ---- vtd_pt's invalidation --------------------------------------------------------- */

/* A gather (vtd_pt.h): its runs page-selectively in one batch, or the
 * domain when it says "whole" or the runs need too many descriptors.
 * IH = 1 (keep the paging-structure caches) only when no table was
 * unlinked and the unit caches no not-present entries (CAP.CM = 0): in
 * caching mode a gather may hold new mappings under tables the unit
 * remembered as absent. */
status_t vtd_unit_pt_invalidate(void *ctx, const struct vtd_pt_gather *g)
{
    const struct vtd_unit_domain *dom = ctx;
    struct vtd_unit *u = dom->unit;
    if (g->whole)
        return vtd_inv_iotlb_domain(u, dom->did);
    status_t st = check_did(u, dom->did);
    if (st != OK)
        return st;
    bool keep = g->ntables == 0 && !VTD_CAP_CM(u->cap);
    struct vtd_desc d[VTD_QI_BATCH_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->nruns; i++)
        if (!add_pages(u, dom->did, g->run[i].base, g->run[i].pages, keep, d, &n))
            return vtd_inv_iotlb_domain(u, dom->did);
    return n ? vtd_qi_submit(u, d, n) : OK;
}
