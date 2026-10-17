/* nvme: the controller (drv/nvme): its registers, disable and enable,
 * identify, the namespace, the queues' creation, reset and the shutdown
 * notification.
 *
 * EVERY REGISTER THE DRIVER WRITES (BAR 0; NVMe 1.4, 3.1):
 *   CC        EN cleared (disable, a reset); CSS NVM, MPS 4 KiB, round
 *             robin, IOSQES 6, IOCQES 4, then EN; SHN 01b at the end
 *   AQA ASQ ACQ  the admin queue's size and pages, while disabled
 *   the doorbells of queues 0 and 1: completion heads (queue.c) and
 *   submission tails (cmd.c)
 * Read only: CAP, VS, CSTS. Never written: NSSR (a subsystem reset),
 * INTMS / INTMC (MSI-X doesn't use them), the controller memory buffer,
 * the boot partitions, PCI config space, the MSI-X table (the kernel's).
 *
 * Start: the firmware's UEFI driver leaves the controller enabled with
 * its queues in firmware memory. Bus mastering is off (devmgr's new
 * dma_cap turned it off), so nothing it queued can run; the controller is
 * disabled (CC.EN 0, CSTS.RDY 0) first, and only then is bus mastering
 * turned on (the safe rebind: ARCHITECTURE.md, "Drivers and services").
 *
 * Waits: every register wait is bounded by CAP.TO (at most READY_MAX) or
 * SHUTDOWN_WAIT; an admin command waited for here is bounded by its
 * deadline (ADMIN_TIMEOUT) and polled every 200 us without the port, so a
 * packet the loop is waiting for is never taken. */
#include "nvme.h"

#define POLL_STEP (200 * NS_PER_US)

static uint32_t rd32(struct nvme *c, uint32_t off)
{
    uint32_t v = drv_read32(c->regs, off);
    if (v == CSTS_GONE && off == NVME_CSTS)
        c->gone = true;
    return v;
}

static void wr32(struct nvme *c, uint32_t off, uint32_t v)
{
    drv_write32(c->regs, off, v);
}

/* A 64-bit register as two 32-bit writes, low first (NVMe allows either). */
static void wr64(struct nvme *c, uint32_t off, uint64_t v)
{
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}

/* CSTS.RDY == want, within c->ready_wait. ERR_PEER_CLOSED: gone; ERR_IO:
 * fatal status while enabling. */
static status_t wait_ready(struct nvme *c, bool want)
{
    uint64_t deadline = drv_clock_ns() + c->ready_wait;
    uint32_t csts;
    for (;;) {
        csts = rd32(c, NVME_CSTS);
        if (c->gone)
            return ERR_PEER_CLOSED;
        if (!!(csts & CSTS_RDY) == want)
            return OK;
        if (want && (csts & CSTS_CFS))
            break;
        if (drv_clock_ns() >= deadline)
            break;
        drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    }
    drv_log("%s: CSTS.RDY didn't become %u (CSTS %#x, CC %#x)", c->name, want, csts,
            rd32(c, NVME_CC));
    return want && (csts & CSTS_CFS) ? ERR_IO : ERR_TIMED_OUT;
}

/* CC.EN 0 and CSTS.RDY 0: the controller drops its queues and does
 * nothing more. */
static status_t disable(struct nvme *c)
{
    uint32_t cc = rd32(c, NVME_CC);
    if (cc & CC_EN)
        wr32(c, NVME_CC, cc & ~CC_EN);
    return wait_ready(c, false);
}

/* The admin queue's registers, then CC and EN, then RDY. */
static status_t enable(struct nvme *c)
{
    uint32_t n = (uint32_t)c->admin.depth - 1;
    wr32(c, NVME_AQA, n | n << 16);
    wr64(c, NVME_ASQ, sq_addr(&c->admin));
    wr64(c, NVME_ACQ, c->admin.cq_addr);
    uint32_t cc = CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR | CC_IOSQES | CC_IOCQES;
    wr32(c, NVME_CC, cc);
    wr32(c, NVME_CC, cc | CC_EN);
    c->admin.live = true;
    status_t st = wait_ready(c, true);
    if (st != OK)
        c->admin.live = false;
    return st;
}

/* ---- admin commands, waited for -------------------------------------------------- */

struct waiter {
    bool     done;
    uint16_t status;
    uint32_t dw0;
};

static void waiter_done(struct nvme *c, struct nvme_slot *s, uint16_t status, uint32_t dw0)
{
    (void)c;
    struct waiter *w = s->ctx;
    w->done = true;
    w->status = status;
    w->dw0 = dw0;
}

status_t ctrl_admin_send(struct nvme *c, enum admin_op op, const struct admin_args *a,
                         void (*done)(struct nvme *, struct nvme_slot *, uint16_t, uint32_t),
                         void *ctx)
{
    if (!c->admin.live)
        return ERR_BAD_STATE;
    struct nvme_slot *s = slot_get(c, &c->admin);
    if (!s)
        return ERR_NO_RESOURCES;
    s->done = done;
    s->ctx = ctx;
    sqe_admin(c, s, op, a);
    return OK;
}

status_t ctrl_admin_wait(struct nvme *c, enum admin_op op, const struct admin_args *a,
                         uint32_t *dw0)
{
    struct waiter w = { 0 };
    uint64_t deadline = drv_clock_ns() + ADMIN_TIMEOUT;
    status_t st = ctrl_admin_send(c, op, a, waiter_done, &w);
    if (st != OK)
        return st;
    struct nvme_slot *s = NULL;   /* ours, to forget it if it never answers */
    for (uint32_t i = 0; i < c->admin.depth && !s; i++)
        if (c->admin.slots[i].busy && c->admin.slots[i].ctx == &w)
            s = &c->admin.slots[i];
    while (!w.done) {
        if (!queue_reap_one(c, &c->admin))
            return ERR_IO;
        if (w.done)
            break;
        uint32_t csts = rd32(c, NVME_CSTS);
        if (c->gone)
            return ERR_PEER_CLOSED;
        if ((csts & CSTS_CFS) || drv_clock_ns() >= deadline) {
            if (s) {   /* the slot stays out; its late answer goes nowhere */
                s->ctx = NULL;
                s->done = NULL;
            }
            drv_log("%s: admin command %u: %s", c->name, (unsigned)op,
                    (csts & CSTS_CFS) ? "the controller reports a fatal error" : "no answer");
            return (csts & CSTS_CFS) ? ERR_IO : ERR_TIMED_OUT;
        }
        drv_sleep_until(drv_clock_ns() + POLL_STEP);
    }
    if (dw0)
        *dw0 = w.dw0;
    if (w.status != NVME_SC_SUCCESS) {
        drv_log("%s: admin command %u failed: status %#x", c->name, (unsigned)op, w.status);
        return ERR_IO;
    }
    return OK;
}

static status_t identify(struct nvme *c, uint32_t cns, uint32_t nsid)
{
    struct admin_args a = { .nsid = nsid, .prp1 = c->data_addr, .cdw10 = cns };
    return ctrl_admin_wait(c, ADMIN_IDENTIFY, &a, NULL);
}

/* ---- identify ----------------------------------------------------------------------- */

/* n bytes of space-padded ASCII into out (n + 1): trimmed, '?' for others. */
static void text(char *out, const uint8_t *p, unsigned n)
{
    while (n && (p[n - 1] == ' ' || !p[n - 1]))
        n--;
    for (unsigned i = 0; i < n; i++)
        out[i] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?';
    out[n] = 0;
}

/* Identify Controller (NVMe 1.4, Figure 247). */
static status_t identify_ctrl(struct nvme *c)
{
    status_t st = identify(c, 1, 0);
    if (st != OK)
        return st;
    const uint8_t *d = c->data;
    text(c->serial, d + 4, 20);
    text(c->model, d + 24, 40);
    text(c->firmware, d + 64, 8);
    uint8_t mdts = d[77];
    c->max_cmd = mdts && mdts < 9 ? NVME_PAGE << mdts : NVME_CMD_MAX;
    if (c->max_cmd > NVME_CMD_MAX)
        c->max_cmd = NVME_CMD_MAX;
    c->nn = le32(d + 516);
    c->vwc = d[525] & 1;   /* VWC: a volatile write cache is there */
    if ((d[512] & 0xf) > 6 || (d[513] & 0xf) > 4) {   /* SQES / CQES: the required sizes */
        drv_log("%s: entries of 2^%u and 2^%u bytes required: not 64 and 16", c->name,
                d[512] & 0xf, d[513] & 0xf);
        return ERR_NOT_SUPPORTED;
    }
    return OK;
}

/* Namespace nsid, served if it is the first usable one (no metadata, 512
 * or 4096-byte blocks); else logged. */
static status_t consider_ns(struct nvme *c, uint32_t nsid, uint32_t *others)
{
    status_t st = identify(c, 0, nsid);
    if (st != OK)
        return st;
    const uint8_t *d = c->data;
    uint64_t nsze = le64(d);
    uint8_t flbas = d[26];
    unsigned f = (flbas & 0xf) | ((flbas >> 5) & 3) << 4;   /* NVMe 2.0's upper bits */
    uint32_t lbaf = f < 64 ? le32(d + 128 + 4 * f) : 0;
    uint32_t ms = lbaf & 0xffff, lbads = (lbaf >> 16) & 0xff;
    uint32_t bs = lbads >= 9 && lbads <= 12 ? 1u << lbads : 0;
    bool ok = nsze && !ms && (bs == 512 || bs == 4096);
    if (ok && !c->ns.nsid) {
        c->ns = (struct nvme_ns){ .nsid = nsid, .block_size = bs, .blocks = nsze };
        return OK;
    }
    if ((*others)++ < 8)
        drv_log("%s: namespace %u: not served (%lu blocks, format %u: %u-byte blocks, %u bytes "
                "of metadata%s)", c->name, nsid, (unsigned long)nsze, f,
                lbads < 32 ? 1u << lbads : 0, ms, ok ? "; one namespace is served" : "");
    return OK;
}

/* The active namespaces (CNS 2; numbered 1..NN before NVMe 1.1), the
 * first usable one served. */
static status_t find_ns(struct nvme *c)
{
    uint32_t *ids = drv_malloc(NVME_PAGE);
    if (!ids)
        return ERR_NO_MEMORY;
    uint32_t n = 0, others = 0;
    status_t st = OK;
    if (c->vs >= 0x10100 && (st = identify(c, 2, 0)) == OK) {
        for (; n < NVME_PAGE / 4 && le32(c->data + 4 * n); n++)
            ids[n] = le32(c->data + 4 * n);
    } else {
        for (; n < c->nn && n < NVME_PAGE / 4; n++)
            ids[n] = n + 1;
    }
    for (uint32_t i = 0; st == OK && i < n; i++)
        st = consider_ns(c, ids[i], &others);
    drv_free(ids);
    if (st == OK && !c->ns.nsid) {
        drv_log("%s: no namespace to serve (%u active)", c->name, n);
        st = ERR_NOT_FOUND;
    }
    return st;
}

/* ---- the I/O queue pair -------------------------------------------------------------- */

/* Set Features (number of queues: one pair), Create I/O Completion Queue
 * (interrupts on vector 0), Create I/O Submission Queue. */
static status_t io_queues(struct nvme *c)
{
    struct admin_args nq = { .cdw10 = 0x07 };
    uint32_t got = 0;
    status_t st = ctrl_admin_wait(c, ADMIN_SET_FEATURES, &nq, &got);
    if (st != OK)
        return st;
    uint32_t qsize = (uint32_t)(c->io.depth - 1) << 16;
    struct admin_args cq = { .prp1 = c->io.cq_addr, .cdw10 = qsize | NVME_IO_QID,
                             .cdw11 = 1u << 0 | 1u << 1 };
    if ((st = ctrl_admin_wait(c, ADMIN_CREATE_CQ, &cq, NULL)) != OK)
        return st;
    c->io.live = true;   /* its completions may come now */
    struct admin_args sq = { .cdw10 = qsize | NVME_IO_QID, .cdw11 = NVME_IO_QID << 16 | 1u << 0 };
    if ((st = ctrl_admin_wait(c, ADMIN_CREATE_SQ, &sq, NULL)) != OK)
        c->io.live = false;
    return st;
}

/* ---- memory -------------------------------------------------------------------------- */

/* The data page (writable) and the PRP list pages (read only). */
static status_t buffers(struct nvme *c)
{
    void *p = NULL;
    status_t st = drv_vmo_create(NVME_PAGE, DRV_VMO_CONTIGUOUS, &c->data_vmo);
    if (st == OK && (st = drv_vmo_map(c->data_vmo, 0, NVME_PAGE, VMAR_READ | VMAR_WRITE,
                                      &p)) == OK)
        c->data = p;
    if (st == OK && (st = drv_vmo_pin(c->data_vmo, c->dma, 0, NVME_PAGE, &c->data_addr,
                                      &c->data_pin)) == OK)
        c->data_pinned = true;
    uint64_t bytes = (uint64_t)NVME_QDEPTH * NVME_PAGE;
    if (st == OK)
        st = drv_vmo_create(bytes, DRV_VMO_CONTIGUOUS, &c->prp_vmo);
    if (st == OK && (st = drv_vmo_map(c->prp_vmo, 0, bytes, VMAR_READ | VMAR_WRITE, &p)) == OK)
        c->prp = p;
    if (st == OK && (st = drv_vmo_pin_read_only(c->prp_vmo, c->dma, 0, bytes, c->prp_addr,
                                                &c->prp_pin)) == OK)
        c->prp_pinned = true;
    return st;
}

static void buffers_free(struct nvme *c)
{
    if (c->prp_pinned && drv_vmo_unpin(c->prp_vmo, c->dma, c->prp_pin) == OK)
        c->prp_pinned = false;
    if (c->data_pinned && drv_vmo_unpin(c->data_vmo, c->dma, c->data_pin) == OK)
        c->data_pinned = false;
    if (c->prp)
        (void)drv_vmo_unmap(c->prp, (uint64_t)NVME_QDEPTH * NVME_PAGE);
    if (c->data)
        (void)drv_vmo_unmap(c->data, NVME_PAGE);
    if (c->prp_vmo != HANDLE_INVALID)
        drv_handle_close(c->prp_vmo);
    if (c->data_vmo != HANDLE_INVALID)
        drv_handle_close(c->data_vmo);
    c->prp = NULL;
    c->data = NULL;
    c->prp_vmo = c->data_vmo = HANDLE_INVALID;
}

/* ---- start ------------------------------------------------------------------------------ */

/* BAR 0's registers and the doorbells of queues 0 and 1 (their own
 * mapping: the page after the registers; never the MSI-X table's). */
static status_t map_regs(struct nvme *c, handle_t bar)
{
    volatile void *p;
    status_t st = drv_mmio_map(bar, 0, NVME_REGS_BYTES, VMO_CACHE_UC, &p);
    if (st != OK) {
        drv_log("%s: can't map the registers (%s)", c->name, status_str(st));
        return st;
    }
    c->regs = p;
    c->cap = drv_read64(c->regs, NVME_CAP);
    c->vs = drv_read32(c->regs, NVME_VS);
    if (c->cap == ~0ull) {
        c->gone = true;
        return ERR_PEER_CLOSED;
    }
    c->dstrd = 4u << CAP_DSTRD(c->cap);
    st = drv_mmio_map(bar, NVME_DOORBELLS, 4 * c->dstrd, VMO_CACHE_UC, &p);
    if (st != OK) {
        drv_log("%s: can't map the doorbells at %#x (%s): they share the MSI-X table's page?",
                c->name, NVME_DOORBELLS, status_str(st));
        return st;
    }
    c->dbs = p;
    return OK;
}

/* What CAP says against what the driver needs. */
static status_t check_cap(struct nvme *c)
{
    uint64_t cap = c->cap;
    if (!(cap & CAP_CSS_NVM) || CAP_MPSMIN(cap)) {
        drv_log("%s: CAP %#lx: %s", c->name, (unsigned long)cap,
                !(cap & CAP_CSS_NVM) ? "no NVM command set" : "its pages are larger than 4 KiB");
        return ERR_NOT_SUPPORTED;
    }
    uint32_t mqes = CAP_MQES(cap) + 1;
    uint16_t depth = mqes < NVME_QDEPTH ? (uint16_t)mqes : NVME_QDEPTH;
    if (depth < 2)
        return ERR_NOT_SUPPORTED;
    c->admin.depth = c->io.depth = depth;
    uint64_t to = (uint64_t)CAP_TO(cap) * 500 * NS_PER_MS;
    c->ready_wait = to < 500 * NS_PER_MS ? 500 * NS_PER_MS : to > READY_MAX ? READY_MAX : to;
    return OK;
}

/* Disabled, then (once) disabled again, before any DMA is possible. */
static status_t quiesce(struct nvme *c)
{
    uint32_t cc = rd32(c, NVME_CC), csts = rd32(c, NVME_CSTS);
    status_t st = disable(c);
    if (st == ERR_TIMED_OUT) {
        drv_log("%s: the controller didn't stop: once more", c->name);
        st = disable(c);
    }
    drv_log("%s: the controller was %s (CC %#x, CSTS %#x): %s", c->name,
            (cc & CC_EN) ? "enabled" : "disabled", cc, csts,
            st == OK ? "now disabled" : status_str(st));
    return st;
}

static status_t bring_up(struct nvme *c)
{
    uint16_t depth = c->io.depth;
    status_t st = queue_create(c, &c->admin, 0, depth);
    if (st == OK)
        st = queue_create(c, &c->io, NVME_IO_QID, depth);
    if (st == OK)
        st = buffers(c);
    if (st == OK)
        st = enable(c);
    if (st == OK)
        st = identify_ctrl(c);
    if (st == OK)
        st = find_ns(c);
    if (st == OK)
        st = io_queues(c);
    return st;
}

status_t ctrl_start(struct nvme *c, handle_t bar)
{
    status_t st = map_regs(c, bar);
    if (st == OK)
        st = check_cap(c);
    if (st == OK)
        st = quiesce(c);
    if (st != OK)
        return st;
    if ((st = drv_dma_bus_master(c->dma, 1)) != OK) {   /* disabled first, bus mastering after */
        drv_log("%s: bus mastering refused (%s)", c->name, status_str(st));
        return st;
    }
    c->bus_master = true;
    if ((st = bring_up(c)) != OK)
        drv_log("%s: bring-up failed (%s)", c->name, status_str(st));
    return st;
}

/* ---- running ---------------------------------------------------------------------------- */

/* Bus mastering off: a command the controller still holds can move no
 * data from here on (only the next driver turns it on again, once it has
 * disabled the controller). Before any such command is answered, so its
 * buffer is free when its client hears of it. */
static void bus_master_off(struct nvme *c, const char *why)
{
    if (!c->bus_master)
        return;
    status_t st = drv_dma_bus_master(c->dma, 0);
    if (st == OK)
        c->bus_master = false;
    if (why)
        drv_log("%s: bus mastering off (%s): %s", c->name, why, status_str(st));
}

status_t ctrl_reset(struct nvme *c, const char *why)
{
    uint64_t now = drv_clock_ns();
    uint32_t recent = 0;
    for (unsigned i = 0; i < RESETS_MAX; i++)
        recent += c->reset_at[i] && now - c->reset_at[i] < RESET_WINDOW;
    c->reset_at[c->resets++ % RESETS_MAX] = now;
    drv_log("%s: reset %u (%s)", c->name, c->resets, why);
    if (recent >= RESETS_MAX) {
        drv_report("%s: FAILED: %u resets within %lu s: giving up", c->name, recent + 1,
                   (unsigned long)(RESET_WINDOW / NS_PER_S));
        return ERR_IO;
    }
    c->io.live = c->admin.live = false;
    status_t st = disable(c);
    if (st == OK && (c->test & NVME_TEST_NOSTOP)) {
        drv_log("%s: test: the controller taken as not stopped", c->name);
        st = ERR_TIMED_OUT;
    }
    if (st != OK)   /* it may still be running what is out */
        bus_master_off(c, "the controller didn't stop");
    queue_fail_all(c);   /* reads back on the wait list, once; the rest answered */
    queue_reset(&c->admin);
    queue_reset(&c->io);
    if (st == OK)
        st = enable(c);
    if (st == OK)
        st = io_queues(c);
    if (st != OK) {
        drv_report("%s: FAILED: reset %u: the controller didn't come back (%s)", c->name,
                   c->resets, status_str(st));
        return ERR_IO;
    }
    io_kick(c);
    return OK;
}

status_t ctrl_poll(struct nvme *c)
{
    uint32_t csts = rd32(c, NVME_CSTS);
    if (c->gone)
        return ERR_PEER_CLOSED;
    if (csts & CSTS_CFS)
        return ctrl_reset(c, "the controller reports a fatal error");
    if (!queue_reap(c))
        return ctrl_reset(c, "a completion for no command");
    if (!queue_timeouts(c))
        return ctrl_reset(c, "a command or its abort got no answer");
    io_kick(c);
    return OK;
}

void health_args(const struct nvme *c, struct admin_args *a)
{
    /* log 02h, the whole controller, 512 bytes (NUMDL 127) */
    *a = (struct admin_args){ .nsid = 0xffffffffu, .prp1 = c->data_addr,
                              .cdw10 = 0x02 | (512 / 4 - 1) << 16 };
}

void health_parse(const uint8_t *d, struct nvme_health *h)
{
    *h = (struct nvme_health){
        .critical = d[0], .percent_used = d[5],
        .units_read = le64(d + 32), .units_written = le64(d + 48),
        .host_reads = le64(d + 64), .host_writes = le64(d + 80),
        .power_cycles = le64(d + 112), .unsafe_shutdowns = le64(d + 144),
        .media_errors = le64(d + 160),
    };
}

status_t ctrl_health(struct nvme *c, struct nvme_health *h)
{
    struct admin_args a;
    health_args(c, &a);
    status_t st = ctrl_admin_wait(c, ADMIN_GET_LOG_PAGE, &a, NULL);
    if (st == OK)
        health_parse(c->data, h);
    return st;
}

/* Commands in flight get DRAIN_WAIT to finish (answered as they do); the
 * rest are failed by ctrl_stop once nothing can move their data. */
static void drain(struct nvme *c)
{
    uint64_t deadline = drv_clock_ns() + DRAIN_WAIT;
    while (c->io.busy && !c->gone && drv_clock_ns() < deadline) {
        if (!queue_reap(c))
            break;
        drv_sleep_until(drv_clock_ns() + POLL_STEP);
    }
}

/* The normal shutdown notification: the drive saves its state. */
static void shutdown(struct nvme *c)
{
    uint32_t cc = rd32(c, NVME_CC);
    if (c->gone || !(cc & CC_EN))
        return;
    wr32(c, NVME_CC, (cc & ~CC_SHN_MASK) | CC_SHN_NORMAL);
    uint64_t t0 = drv_clock_ns(), deadline = t0 + SHUTDOWN_WAIT;
    uint32_t csts;
    while (((csts = rd32(c, NVME_CSTS)) & CSTS_SHST_MASK) != CSTS_SHST_DONE && !c->gone &&
           drv_clock_ns() < deadline)
        drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    bool done = (csts & CSTS_SHST_MASK) == CSTS_SHST_DONE && !c->gone;
    drv_log("%s: shutdown notification: %s after %lu ms (CSTS %#x)", c->name,
            done ? "complete" : "NOT complete", (unsigned long)((drv_clock_ns() - t0) / NS_PER_MS),
            csts);
}

void ctrl_stop(struct nvme *c)
{
    if (c->io.live)
        drain(c);
    if (c->io.live && !c->gone) {
        struct admin_args q = { .cdw10 = NVME_IO_QID };
        if (ctrl_admin_wait(c, ADMIN_DELETE_SQ, &q, NULL) == OK)
            (void)ctrl_admin_wait(c, ADMIN_DELETE_CQ, &q, NULL);
        c->io.live = false;
    }
    if (c->admin.live)
        shutdown(c);
    c->admin.live = false;
    bus_master_off(c, c->io.busy ? "commands still out at the end" : NULL);
    io_end_all(c, ERR_PEER_CLOSED);   /* only now: nothing of theirs can still move */
    queue_destroy(c, &c->io);
    queue_destroy(c, &c->admin);
    buffers_free(c);
}
