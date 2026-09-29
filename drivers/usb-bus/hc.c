/* usb-bus: the xHCI host controller. The bring-up is xhci-noop's, proven
 * on the PC's Intel 8086:7A60 and QEMU's qemu-xhci: USB Legacy Support
 * handoff, halt, HCRST, bus mastering on (M7: only now), DCBAA + scratchpads, command ring, one event ring
 * on interrupter 0 (MSI / MSI-X entry 0 as a port packet), run. Added
 * here: the Supported Protocol capabilities (which root ports are USB 2
 * and which USB 3), port power, a DMA page pool for contexts, rings and
 * buffers, commands with a timeout (and Command Abort), and the event loop
 * that every wait goes through: events are drained on each interrupt and
 * also polled at least every 50 ms, so a lost MSI costs latency, never a
 * hang. */
#include "usbbus.h"

struct hc g_hc;

#define DMA_DCBAA   0x0000
#define DMA_ERST    0x0800
#define DMA_CMDRING 0x1000
#define DMA_EVRING  0x2000
#define DMA_SPARRAY 0x3000

static uint32_t hi32(uint64_t v) { return (uint32_t)(v >> 32); }
static uint32_t lo32(uint64_t v) { return (uint32_t)v; }

const char *cc_str(uint32_t cc)
{
    switch (cc) {
    case 0: return "Invalid";
    case CC_SUCCESS: return "Success";
    case CC_DATA_BUFFER: return "Data Buffer Error";
    case CC_BABBLE: return "Babble Detected";
    case CC_TRANSACTION: return "USB Transaction Error";
    case CC_TRB: return "TRB Error";
    case CC_STALL: return "Stall";
    case CC_RESOURCE: return "Resource Error";
    case CC_BANDWIDTH: return "Bandwidth Error";
    case CC_NO_SLOTS: return "No Slots Available";
    case 11: return "Slot Not Enabled";
    case 12: return "Endpoint Not Enabled";
    case CC_SHORT_PACKET: return "Short Packet";
    case CC_PARAMETER: return "Parameter Error";
    case CC_CONTEXT_STATE: return "Context State Error";
    case 22: return "Incompatible Device";
    case CC_RING_STOPPED: return "Command Ring Stopped";
    case CC_ABORTED: return "Command Aborted";
    case CC_STOPPED: return "Stopped";
    case CC_STOPPED_LEN: return "Stopped - Length Invalid";
    case 35: return "Secondary Bandwidth Error";
    case 36: return "Split Transaction Error";
    case CC_TIMEOUT: return "timed out";
    case CC_GONE: return "device gone";
    default: return "error";
    }
}

/* ---- registers ------------------------------------------------------------- */

/* The pointer for BAR0 offset `off`, mapping its page on first use (the
 * kernel refuses pages holding an MSI-X table or PBA, which qemu-xhci keeps
 * in BAR0 next to the registers). On a failure the access goes to a dummy
 * word and map_failed says so. */
static volatile uint8_t *reg(struct hc *x, uint32_t off)
{
    static uint32_t sink[2];
    uint32_t page = off & ~(PAGE - 1);
    for (unsigned i = 0; i < x->nmap; i++)
        if (x->map[i].page == page)
            return x->map[i].va + (off - page);
    volatile void *va = NULL;
    status_t st = x->nmap < MAX_MAPS ? drv_mmio_map(x->bar, page, PAGE, VMO_CACHE_UC, &va)
                                     : ERR_NO_RESOURCES;
    if (st != OK) {
        if (!x->map_failed) {
            x->map_failed = true;
            x->map_fail_off = off;
            x->map_fail_st = st;
        }
        sink[0] = 0xffffffffu;
        return (volatile uint8_t *)sink;
    }
    x->map[x->nmap].page = page;
    x->map[x->nmap].va = va;
    x->nmap++;
    return (volatile uint8_t *)va + (off - page);
}

uint32_t hc_rd(struct hc *x, uint32_t off) { return drv_read32(reg(x, off), 0); }
void hc_wr(struct hc *x, uint32_t off, uint32_t v) { drv_write32(reg(x, off), 0, v); }
static uint32_t op_rd(struct hc *x, uint32_t r) { return hc_rd(x, x->caplen + r); }
static void op_wr(struct hc *x, uint32_t r, uint32_t v) { hc_wr(x, x->caplen + r, v); }
static uint32_t ir_rd(struct hc *x, uint32_t r) { return hc_rd(x, x->rtsoff + IR0 + r); }
static void ir_wr(struct hc *x, uint32_t r, uint32_t v) { hc_wr(x, x->rtsoff + IR0 + r, v); }

static void op_wr64(struct hc *x, uint32_t r, uint64_t v)
{
    op_wr(x, r, lo32(v));
    op_wr(x, r + 4, hi32(v));
}

static void ir_wr64(struct hc *x, uint32_t r, uint64_t v)
{
    ir_wr(x, r, lo32(v));
    ir_wr(x, r + 4, hi32(v));
}

uint32_t hc_portsc(struct hc *h, uint32_t port)
{
    return op_rd(h, OP_PORTSC(port));
}

/* Write PORTSC: the RWS bits as they are, plus `set` (PR, a change bit to
 * clear, LWS | PLS, ...). Never writes PED (that would disable the port). */
void hc_portsc_write(struct hc *h, uint32_t port, uint32_t set)
{
    uint32_t v = hc_portsc(h, port);
    op_wr(h, OP_PORTSC(port), (v & PS_KEEP) | set);
}

bool hc_port_is_usb3(struct hc *h, uint32_t port)
{
    for (unsigned i = 0; i < h->nproto; i++)
        if (port >= h->proto[i].first && port < h->proto[i].first + h->proto[i].count)
            return h->proto[i].major >= 3;
    return false;
}

static void snapshot(struct hc *x)
{
    if (!x->caplen)
        return;
    bool mf = x->map_failed;
    drv_report("regs: USBCMD %08x USBSTS %08x CRCR %08x IMAN %08x ERDP %08x", op_rd(x, OP_USBCMD),
               op_rd(x, OP_USBSTS), op_rd(x, OP_CRCR), x->rtsoff ? ir_rd(x, IR_IMAN) : 0,
               x->rtsoff ? ir_rd(x, IR_ERDP) : 0);
    x->map_failed = mf;
}

#define fail(x, step, fmt, ...) \
    (drv_report("FAILED at %s: " fmt, (step), ##__VA_ARGS__), snapshot(x), 1)

static bool wait_op(struct hc *x, uint32_t r, uint32_t mask, uint32_t want, uint64_t timeout_ms,
                    uint32_t *last)
{
    uint64_t end = drv_clock_ns() + timeout_ms * NS_PER_MS;
    for (;;) {
        uint32_t v = op_rd(x, r);
        if (last)
            *last = v;
        if (v != 0xffffffffu && (v & mask) == want)
            return true;
        if (drv_clock_ns() > end || x->map_failed)
            return false;
        drv_sleep_until(drv_clock_ns() + 50 * NS_PER_US);
    }
}

/* ---- PCI config ------------------------------------------------------------- */

static uint32_t cfg(struct hc *x, uint32_t off, uint32_t width)
{
    uint32_t v = 0;
    if (drv_pci_config_read(x->dev, off, width, &v) != OK)
        return 0xffffffffu;
    return v;
}

static uint32_t pci_cap(struct hc *x, uint32_t id)
{
    if (!(cfg(x, 0x06, 2) & (1u << 4)))
        return 0;
    uint32_t off = cfg(x, 0x34, 1) & 0xfc;
    for (int guard = 0; off >= 0x40 && off < 0x100 && guard < 48; guard++) {
        if ((cfg(x, off, 1) & 0xff) == id)
            return off;
        off = cfg(x, off + 1, 1) & 0xfc;
    }
    return 0;
}

static int check_pci(struct hc *x)
{
    uint32_t id = cfg(x, 0x00, 4);
    if (id == 0xffffffffu)
        return fail(x, "PCI config", "can't read the function's config space (DR_PCIDEV)");
    x->vendor = (uint16_t)id;
    x->device = (uint16_t)(id >> 16);
    x->revision = (uint8_t)cfg(x, 0x08, 1);
    uint32_t class = cfg(x, 0x08, 4) >> 8;
    if (class != 0x0c0330)
        return fail(x, "PCI config", "class %06x is not an xHCI (0c0330)", class);
    uint32_t cmd = cfg(x, 0x04, 2);
    if (!(cmd & (1u << 1)))
        return fail(x, "PCI config", "memory decode is off (command %04x)", cmd);
    /* Bus mastering is off (M7 safe rebind): hc_bring_up turns it on
     * through DR_DMA once the controller is halted and reset, so nothing a
     * previous driver left queued reaches memory. */
    /* Power state: devmgr wakes a function found in D1-D3 before it binds
     * a driver (drivers may not change it). */
    uint32_t pm = pci_cap(x, 0x01);
    if (pm && (cfg(x, pm + 4, 2) & 3))
        return fail(x, "PCI power", "the function is in D%u, not D0", cfg(x, pm + 4, 2) & 3);
    uint32_t msix = pci_cap(x, 0x11), msi = pci_cap(x, 0x05);
    uint32_t mc_x = msix ? cfg(x, msix + 2, 2) : 0, mc = msi ? cfg(x, msi + 2, 2) : 0;
    if (mc_x & (1u << 15)) {
        x->msix = true;
        x->irq_vectors = (mc_x & 0x7ff) + 1;
    } else if (mc & 1) {
        x->msix = false;
        x->irq_vectors = 1u << ((mc >> 1) & 7);
    } else {
        return fail(x, "PCI config", "neither MSI-X (%04x) nor MSI (%04x) is enabled", mc_x, mc);
    }
    drv_log("%04x:%04x rev %02x, command %04x, %s", x->vendor, x->device, x->revision, cmd,
            x->msix ? "MSI-X on" : "MSI on");
    return 0;
}

/* ---- bring-up ------------------------------------------------------------------ */

static int read_caps(struct hc *x)
{
    uint32_t v = hc_rd(x, CAP_CAPLENGTH);
    if (x->map_failed)
        return fail(x, "map BAR0", "offset %x: %s", x->map_fail_off, status_str(x->map_fail_st));
    if (v == 0xffffffffu)
        return fail(x, "capability registers", "BAR0 reads all ones (device not answering)");
    x->caplen = v & 0xff;
    x->hciver = v >> 16;
    x->hcs1 = hc_rd(x, CAP_HCSPARAMS1);
    x->hcs2 = hc_rd(x, CAP_HCSPARAMS2);
    x->hcc1 = hc_rd(x, CAP_HCCPARAMS1);
    x->dboff = hc_rd(x, CAP_DBOFF) & ~3u;
    x->rtsoff = hc_rd(x, CAP_RTSOFF) & ~0x1fu;
    x->ports = x->hcs1 >> 24;
    x->slots = x->hcs1 & 0xff;
    x->scratchpads = (((x->hcs2 >> 21) & 0x1f) << 5) | (x->hcs2 >> 27);
    x->csz = (x->hcc1 & (1u << 2)) ? 64 : 32;
    if (x->caplen < 0x20 || x->caplen >= PAGE || !x->dboff || !x->rtsoff)
        return fail(x, "capability registers", "CAPLENGTH %x DBOFF %x RTSOFF %x make no sense",
                    x->caplen, x->dboff, x->rtsoff);
    if (!x->ports || x->ports > 255 || !x->slots)
        return fail(x, "capability registers", "%u ports, %u slots make no sense", x->ports,
                    x->slots);
    drv_log("xHCI %x.%02x: %u ports, %u slots, %u scratchpads, AC64 %u, CSZ %u (%u-byte contexts), "
            "PPC %u", x->hciver >> 8, x->hciver & 0xff, x->ports, x->slots, x->scratchpads,
            x->hcc1 & 1, (x->hcc1 >> 2) & 1, x->csz, (x->hcc1 >> 3) & 1);
    return 0;
}

/* Walk the extended capabilities: the legacy handoff, and the Supported
 * Protocol capabilities (which root ports speak USB 2, which USB 3). */
static int ext_caps(struct hc *x)
{
    x->handoff = "none";
    uint32_t off = (x->hcc1 >> 16) * 4;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = hc_rd(x, off);
        if (x->map_failed) {
            drv_log("extended capabilities: can't map offset %x (%s); stopped walking",
                    x->map_fail_off, status_str(x->map_fail_st));
            x->map_failed = false;
            break;
        }
        if (v == 0xffffffffu)
            break;
        uint32_t id = v & 0xff, next = (v >> 8) & 0xff;
        if (id == XCAP_PROTOCOL) {
            uint32_t name = hc_rd(x, off + 8), st = hc_rd(x, off + 12);
            uint8_t first = name & 0xff, count = (name >> 8) & 0xff;
            drv_log("supported protocol: USB %x.%02x, ports %u-%u, %u speed IDs, slot type %u",
                    v >> 24, (v >> 16) & 0xff, first, first + count - 1, name >> 28, st & 0x1f);
            if (x->nproto < MAX_PROTOS && first && count) {
                x->proto[x->nproto].major = (uint8_t)(v >> 24);
                x->proto[x->nproto].minor = (uint8_t)(v >> 16);
                x->proto[x->nproto].first = first;
                x->proto[x->nproto].count = count;
                x->proto[x->nproto].slot_type = st & 0x1f;
                x->proto[x->nproto].psic = (uint8_t)(name >> 28);
                x->nproto++;
            }
        }
        if (id == XCAP_LEGACY) {
            uint32_t ctl = hc_rd(x, off + 4);
            uint64_t t0 = drv_clock_ns();
            drv_write8(reg(x, off + 3), 0, 1);
            bool released = false;
            while (drv_clock_ns() - t0 < 1000 * NS_PER_MS) {
                if (!(hc_rd(x, off) & LEG_BIOS_OWNED)) {
                    released = true;
                    break;
                }
                drv_sleep_until(drv_clock_ns() + 1 * NS_PER_MS);
            }
            if (released) {
                x->handoff = v & LEG_BIOS_OWNED ? "ok" : "ok (not BIOS-owned)";
            } else {
                x->handoff = "timeout";
                drv_log("BIOS did not release the controller in 1 s; clearing BIOS Owned");
                drv_write8(reg(x, off + 2), 0, 0);
            }
            ctl = hc_rd(x, off + 4);
            hc_wr(x, off + 4, (ctl & ~(LEGCTL_SMI_ENABLES | LEGCTL_SMI_STATUS)) | LEGCTL_SMI_STATUS);
            drv_log("BIOS handoff %s after %lu ms", x->handoff,
                    (unsigned long)((drv_clock_ns() - t0) / NS_PER_MS));
        }
        if (!next)
            break;
        off += next * 4;
    }
    if (!x->nproto)
        drv_log("no supported-protocol capability: treating every port as USB 2");
    return 0;
}

static int stop(struct hc *x, const char *step)
{
    uint32_t cmd = op_rd(x, OP_USBCMD), sts;
    op_wr(x, OP_USBCMD, cmd & ~(CMD_RS | CMD_INTE | CMD_HSEE));
    if (!wait_op(x, OP_USBSTS, STS_HCH, STS_HCH, 100, &sts))
        return fail(x, step, "no HCH 100 ms after RS=0 (USBCMD was %08x, USBSTS %08x)", cmd, sts);
    x->running = false;
    return 0;
}

static int reset(struct hc *x, const char *step)
{
    uint32_t v;
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &v))
        return fail(x, step, "Controller Not Ready 1 s before reset (USBSTS %08x)", v);
    op_wr(x, OP_USBCMD, CMD_HCRST);
    drv_sleep_until(drv_clock_ns() + 1 * NS_PER_MS);
    if (!wait_op(x, OP_USBCMD, CMD_HCRST, 0, 1000, &v))
        return fail(x, step, "HCRST still set 1 s after reset (USBCMD %08x)", v);
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &v))
        return fail(x, step, "Controller Not Ready 1 s after reset (USBSTS %08x)", v);
    return 0;
}

/* ---- the DMA page pool ---------------------------------------------------------- */

static int pool_setup(struct hc *h)
{
    uint64_t len = (uint64_t)POOL_PAGES * PAGE;
    status_t st = drv_vmo_create(len, DRV_VMO_DMA32, &h->pool_vmo);
    if (st != OK)
        return fail(h, "DMA pool", "VMO of %u pages: %s", POOL_PAGES, status_str(st));
    h->pool_addr = drv_malloc(POOL_PAGES * sizeof(uint64_t));
    if (!h->pool_addr)
        return fail(h, "DMA pool", "no memory for the page addresses");
    st = drv_vmo_pin(h->pool_vmo, h->dma, 0, len, h->pool_addr, &h->pool_pin);
    if (st != OK)
        return fail(h, "DMA pool", "pin %u pages: %s", POOL_PAGES, status_str(st));
    h->pool_pinned = true;
    for (unsigned i = 0; i < POOL_PAGES; i++)
        if (h->pool_addr[i] + PAGE > (1ull << 32) && !(h->hcc1 & 1))
            return fail(h, "DMA pool", "page above 4 GiB and the controller has no AC64");
    void *p;
    st = drv_vmo_map(h->pool_vmo, 0, len, VMAR_READ | VMAR_WRITE, &p);
    if (st != OK)
        return fail(h, "DMA pool", "map: %s", status_str(st));
    h->pool = p;
    return 0;
}

int pool_alloc(struct hc *h)
{
    for (int i = 0; i < POOL_PAGES; i++)
        if (!h->pool_used[i]) {
            h->pool_used[i] = 1;
            zero(h->pool + (uint64_t)i * PAGE, PAGE);
            if (++h->pool_inuse > h->pool_peak)
                h->pool_peak = h->pool_inuse;
            return i;
        }
    drv_log("DMA pool: all %u pages in use", POOL_PAGES);
    return -1;
}

void pool_free(struct hc *h, int page)
{
    if (page >= 0 && page < POOL_PAGES && h->pool_used[page]) {
        h->pool_used[page] = 0;
        h->pool_inuse--;
    }
}

void *pool_va(struct hc *h, int page)
{
    return h->pool + (uint64_t)page * PAGE;
}

uint64_t pool_dev(struct hc *h, int page)
{
    return h->pool_addr[page];
}

/* ---- rings ------------------------------------------------------------------------ */

bool ring_init(struct hc *h, struct ring *r)
{
    r->page = pool_alloc(h);
    if (r->page < 0)
        return false;
    r->t = pool_va(h, r->page);
    r->dev = pool_dev(h, r->page);
    r->t[RING_TRBS - 1].d0 = lo32(r->dev);
    r->t[RING_TRBS - 1].d1 = hi32(r->dev);
    r->t[RING_TRBS - 1].d3 = TRB_TYPE(TRB_LINK) | TRB_TC;
    r->enq = 0;
    r->cycle = 1;
    return true;
}

void ring_free(struct hc *h, struct ring *r)
{
    if (r->page >= 0)
        pool_free(h, r->page);
    r->page = -1;
    r->t = NULL;
}

uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    uint32_t i = r->enq;
    volatile struct trb *t = &r->t[i];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->d3 = (d3 & ~TRB_C) | r->cycle;   /* the cycle bit last: now the xHC's */
    uint64_t addr = r->dev + (uint64_t)i * sizeof(struct trb);
    if (++r->enq == RING_TRBS - 1) {
        volatile struct trb *l = &r->t[RING_TRBS - 1];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        l->d3 = (l->d3 & ~TRB_C) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return addr;
}

uint32_t ring_index(const struct ring *r, uint64_t trb_dev)
{
    if (!r->t || trb_dev < r->dev || trb_dev >= r->dev + PAGE || (trb_dev & 15))
        return RING_TRBS;
    return (uint32_t)((trb_dev - r->dev) / sizeof(struct trb));
}

/* ---- memory, run --------------------------------------------------------------------- */

static int setup_memory(struct hc *x)
{
    uint32_t ps = op_rd(x, OP_PAGESIZE) & 0xffff;
    x->pagesize = ps ? (1u << (__builtin_ctz(ps) + 12)) : 0;
    if (x->pagesize != PAGE)
        return fail(x, "PAGESIZE", "controller page size %u (register %x); only 4 KiB is supported",
                    x->pagesize, ps);

    uint32_t sp_array_pages = (x->scratchpads * 8 + PAGE - 1) / PAGE;
    x->ctx_pages = DMA_SPARRAY / PAGE + sp_array_pages;
    uint64_t len = (uint64_t)x->ctx_pages * PAGE;
    status_t st = drv_vmo_create(len, DRV_VMO_CONTIGUOUS | DRV_VMO_DMA32, &x->ctx_vmo);
    if (st != OK)
        return fail(x, "DMA memory", "contiguous DMA32 VMO of %u pages: %s", x->ctx_pages,
                    status_str(st));
    uint64_t addrs[8];
    if (x->ctx_pages > 8)
        return fail(x, "DMA memory", "%u scratchpads need too big an array", x->scratchpads);
    st = drv_vmo_pin(x->ctx_vmo, x->dma, 0, len, addrs, &x->ctx_pin);
    if (st != OK)
        return fail(x, "DMA memory", "pin: %s", status_str(st));
    x->ctx_pinned = true;
    x->ctx_dev = addrs[0];
    for (uint32_t i = 1; i < x->ctx_pages; i++)
        if (addrs[i] != x->ctx_dev + (uint64_t)i * PAGE)
            return fail(x, "DMA memory", "contiguous VMO pinned as scattered pages");
    if (x->ctx_dev + len > (1ull << 32))
        return fail(x, "DMA memory", "DMA32 memory at %lx is above 4 GiB", x->ctx_dev);
    void *p;
    st = drv_vmo_map(x->ctx_vmo, 0, len, VMAR_READ | VMAR_WRITE, &p);
    if (st != OK)
        return fail(x, "DMA memory", "map: %s", status_str(st));
    x->ctx = p;
    zero(x->ctx, len);

    volatile uint64_t *dcbaa = (volatile uint64_t *)(x->ctx + DMA_DCBAA);
    if (x->scratchpads) {
        uint64_t splen = (uint64_t)x->scratchpads * PAGE;
        st = drv_vmo_create(splen, DRV_VMO_DMA32, &x->sp_vmo);
        if (st != OK)
            return fail(x, "scratchpads", "VMO of %u pages: %s", x->scratchpads, status_str(st));
        uint64_t *sp = drv_malloc(x->scratchpads * sizeof(uint64_t));
        if (!sp)
            return fail(x, "scratchpads", "no memory for %u addresses", x->scratchpads);
        st = drv_vmo_pin(x->sp_vmo, x->dma, 0, splen, sp, &x->sp_pin);
        if (st != OK) {
            drv_free(sp);
            return fail(x, "scratchpads", "pin %u pages: %s", x->scratchpads, status_str(st));
        }
        x->sp_pinned = true;
        volatile uint64_t *arr = (volatile uint64_t *)(x->ctx + DMA_SPARRAY);
        for (uint32_t i = 0; i < x->scratchpads; i++)
            arr[i] = sp[i];
        drv_free(sp);
        dcbaa[0] = x->ctx_dev + DMA_SPARRAY;
    }

    struct trb *cr = (struct trb *)(x->ctx + DMA_CMDRING);
    uint64_t cr_dev = x->ctx_dev + DMA_CMDRING;
    cr[RING_TRBS - 1].d0 = lo32(cr_dev);
    cr[RING_TRBS - 1].d1 = hi32(cr_dev);
    cr[RING_TRBS - 1].d3 = TRB_TYPE(TRB_LINK) | TRB_TC;
    x->cmd_enq = 0;
    x->cmd_cycle = 1;

    volatile uint32_t *erst = (volatile uint32_t *)(x->ctx + DMA_ERST);
    uint64_t ev_dev = x->ctx_dev + DMA_EVRING;
    erst[0] = lo32(ev_dev);
    erst[1] = hi32(ev_dev);
    erst[2] = RING_TRBS;
    erst[3] = 0;
    x->ev_deq = 0;
    x->ev_cycle = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Slots: as many as we track (the DCBAA page holds 256 entries). */
    x->max_slots_en = x->slots < MAX_DEVS ? x->slots : MAX_DEVS;
    op_wr(x, OP_CONFIG, (op_rd(x, OP_CONFIG) & ~0xffu) | x->max_slots_en);
    op_wr64(x, OP_DCBAAP, x->ctx_dev + DMA_DCBAA);
    op_wr64(x, OP_CRCR, cr_dev | CRCR_RCS);

    ir_wr(x, IR_ERSTSZ, (ir_rd(x, IR_ERSTSZ) & ~0xffffu) | 1);
    ir_wr64(x, IR_ERDP, ev_dev);
    ir_wr64(x, IR_ERSTBA, x->ctx_dev + DMA_ERST);
    ir_wr(x, IR_IMOD, IMOD_40US);
    ir_wr(x, IR_IMAN, IMAN_IE | IMAN_IP);
    if (x->map_failed)
        return fail(x, "map registers", "offset %x: %s", x->map_fail_off, status_str(x->map_fail_st));
    return 0;
}

static int run(struct hc *x)
{
    op_wr(x, OP_USBSTS, STS_HSE | STS_EINT | STS_PCD);
    op_wr(x, OP_USBCMD, CMD_RS | CMD_INTE | CMD_HSEE);
    uint32_t sts;
    if (!wait_op(x, OP_USBSTS, STS_HCH, 0, 100, &sts))
        return fail(x, "run", "HCH still set 100 ms after RS=1 (USBSTS %08x)", sts);
    x->running = true;
    return 0;
}

/* Port power: with Port Power Control (HCCPARAMS1.PPC) the ports come out
 * of reset unpowered; without it PP is hard-wired on. */
static void power_ports(struct hc *h)
{
    unsigned powered = 0;
    for (uint32_t p = 1; p <= h->ports; p++) {
        uint32_t v = hc_portsc(h, p);
        if (!(v & PS_PP)) {
            hc_portsc_write(h, p, PS_PP);
            powered++;
        }
    }
    if (powered) {
        drv_log("powered %u root port(s)", powered);
        drv_sleep_until(drv_clock_ns() + 20 * NS_PER_MS);
    }
}

void hc_doorbell(struct hc *h, uint32_t slot, uint32_t target)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    hc_wr(h, h->dboff + 4 * slot, target);
}

/* ---- events ------------------------------------------------------------------------ */

static void event(struct hc *h, volatile struct trb *e, uint32_t d3)
{
    uint32_t type = TRB_TYPE_OF(d3);
    uint64_t ptr = e->d0 | (uint64_t)e->d1 << 32;
    uint32_t status = e->d2;
    h->events++;
    switch (type) {
    case TRB_CMD_DONE_EV:
        if (h->cmd.busy && ptr == h->cmd.trb) {
            h->cmd.done = true;
            h->cmd.cc = status >> 24;
            h->cmd.param = status & 0xffffff;
            h->cmd.slot = d3 >> 24;
        } else if ((status >> 24) != CC_RING_STOPPED) {
            h->spurious_events++;
            drv_log("command completion for %lx (cc %u %s): not the one outstanding",
                    (unsigned long)ptr, status >> 24, cc_str(status >> 24));
        }
        break;
    case TRB_TRANSFER_EV:
        usb_transfer_event(h, (uint8_t)(d3 >> 24), (uint8_t)((d3 >> 16) & 0x1f),
                           (d3 & (1u << 2)) ? 0 : ptr, status >> 24, status & 0xffffff);
        break;
    case TRB_PORT_EV: {
        uint32_t port = (uint32_t)(ptr >> 24) & 0xff;
        if (port >= 1 && port <= h->ports && port < 256)
            h->port_changed[port / 32] |= 1u << (port % 32);
        break;
    }
    case TRB_HC_EV:
        drv_log("host controller event: %s (%u)", cc_str(status >> 24), status >> 24);
        break;
    default:
        h->spurious_events++;
        drv_log("event: type %u, pointer %lx, status %08x", type, (unsigned long)ptr, status);
        break;
    }
}

/* Drain the event ring. After an interrupt ERDP is written even when the
 * ring turned out empty (an earlier poll took the events): the write is
 * what clears Event Handler Busy, and while EHB is set the controller
 * raises no interrupt (xHCI 5.5.2.3.3). */
static void poll_events(struct hc *h, bool after_irq)
{
    if (!h->ctx)
        return;
    volatile struct trb *ev = (volatile struct trb *)(h->ctx + DMA_EVRING);
    unsigned n = 0;
    for (; n < RING_TRBS; n++) {
        volatile struct trb *e = &ev[h->ev_deq];
        uint32_t d3 = e->d3;
        if ((d3 & TRB_C) != h->ev_cycle)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        event(h, e, d3);
        if (++h->ev_deq == RING_TRBS) {
            h->ev_deq = 0;
            h->ev_cycle ^= 1;
        }
    }
    if (n || after_irq) {
        uint64_t deq = h->ctx_dev + DMA_EVRING + (uint64_t)h->ev_deq * sizeof(struct trb);
        ir_wr64(h, IR_ERDP, deq | ERDP_EHB);
    }
}

void hc_poll(struct hc *h)
{
    poll_events(h, false);
}

/* HSE / HCE: the controller has stopped itself. */
static void check_status(struct hc *h, uint32_t sts)
{
    if ((sts & (STS_HSE | STS_HCE)) && !h->dead) {
        h->dead = true;
        drv_report("FAILED: host %s error (USBSTS %08x); the controller is stopped",
                   sts & STS_HCE ? "controller" : "system", sts);
    }
}

static void irq(struct hc *h)
{
    h->irqs++;
    /* Re-arm first: a fire from here on is a new packet. */
    drv_interrupt_ack(h->irq);
    ir_wr(h, IR_IMAN, IMAN_IE | IMAN_IP);
    uint32_t sts = op_rd(h, OP_USBSTS);
    op_wr(h, OP_USBSTS, sts & (STS_EINT | STS_PCD));
    check_status(h, sts);
}

static void wait_capped(struct hc *h, uint64_t deadline, uint64_t cap_ms)
{
    uint64_t now = drv_clock_ns(), cap = now + cap_ms * NS_PER_MS;
    struct port_packet pkt;
    status_t st = drv_port_wait(h->port, deadline < cap ? deadline : cap, &pkt);
    bool fired = false;
    if (st == OK) {
        if (pkt.key == KEY_IRQ) {
            irq(h);
            fired = true;
        } else {
            serve_packet(h, &pkt);
        }
    }
    /* An HSE comes through the platform (SERR#), not necessarily as an
     * interrupt, and an HCE may come with none: without one, USBSTS is
     * checked here too, at least every 200 ms in the idle loop (M7 review). */
    if (!fired && h->running)
        check_status(h, op_rd(h, OP_USBSTS));
    poll_events(h, fired);
}

/* Waiting for a completion: poll the event ring at least every 50 ms, so
 * a lost interrupt costs time, not the command. */
void hc_wait(struct hc *h, uint64_t deadline)
{
    wait_capped(h, deadline, 50);
}

/* The main loop with nothing to do: 200 ms. */
void hc_wait_idle(struct hc *h, uint64_t deadline)
{
    wait_capped(h, deadline, 200);
}

void hc_sleep(struct hc *h, uint64_t ms)
{
    uint64_t end = drv_clock_ns() + ms * NS_PER_MS;
    while (drv_clock_ns() < end)
        hc_wait(h, end);
}

/* ---- commands ------------------------------------------------------------------------ */

uint32_t hc_command(struct hc *h, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                    uint32_t *slot_out, uint64_t timeout_ms)
{
    if (h->dead || !h->running)
        return CC_GONE;
    struct trb *cr = (struct trb *)(h->ctx + DMA_CMDRING);
    uint64_t trb = h->ctx_dev + DMA_CMDRING + (uint64_t)h->cmd_enq * sizeof(struct trb);
    volatile struct trb *t = &cr[h->cmd_enq];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->d3 = (d3 & ~TRB_C) | h->cmd_cycle;
    if (++h->cmd_enq == RING_TRBS - 1) {
        volatile struct trb *l = &cr[RING_TRBS - 1];
        l->d3 = (l->d3 & ~TRB_C) | h->cmd_cycle;
        h->cmd_enq = 0;
        h->cmd_cycle ^= 1;
    }
    h->cmd.busy = true;
    h->cmd.done = false;
    h->cmd.trb = trb;
    h->cmd.cc = 0;
    h->cmd.slot = 0;
    hc_doorbell(h, 0, 0);
    uint64_t deadline = drv_clock_ns() + timeout_ms * NS_PER_MS;
    while (!h->cmd.done && !h->dead && drv_clock_ns() < deadline)
        hc_wait(h, deadline);
    if (!h->cmd.done && !h->dead) {
        /* Command Abort: the ring stops, the command completes with
         * Command Aborted (or finishes meanwhile). */
        uint32_t type = TRB_TYPE_OF(d3);
        drv_log("command type %u: no completion in %lu ms; aborting it", type,
                (unsigned long)timeout_ms);
        /* The whole register holds a valid pointer (our enqueue point and
         * cycle), as Linux writes it: if the ring has stopped by the time
         * the high dword lands, some controllers take the 64-bit value as
         * the new Command Ring Pointer -- 0 would send the next command
         * fetch to physical address 0. */
        uint64_t next = h->ctx_dev + DMA_CMDRING + (uint64_t)h->cmd_enq * sizeof(struct trb);
        op_wr64(h, OP_CRCR, next | h->cmd_cycle | CRCR_CA);
        uint64_t end = drv_clock_ns() + 5000 * NS_PER_MS;
        while (drv_clock_ns() < end && (op_rd(h, OP_CRCR) & CRCR_CRR))
            hc_wait(h, drv_clock_ns() + 5 * NS_PER_MS);
        hc_poll(h);
        if (op_rd(h, OP_CRCR) & CRCR_CRR) {
            h->dead = true;
            drv_report("FAILED: the command ring did not stop 5 s after Command Abort");
        } else if (!h->cmd.done) {
            /* Stopped without taking it (never fetched): the next doorbell
             * would run it late, against contexts we free on the timeout.
             * A No Op in its place (what Linux does); its completion is
             * logged as not the outstanding one. */
            t->d3 = TRB_TYPE(TRB_NOOP_CMD) | (t->d3 & TRB_C);
        }
    }
    h->cmd.busy = false;
    if (!h->cmd.done)
        return h->dead ? CC_GONE : CC_TIMEOUT;
    if (slot_out)
        *slot_out = h->cmd.slot;
    return h->cmd.cc;
}

/* ---- bring-up and shutdown -------------------------------------------------------------- */

int hc_bring_up(struct hc *x)
{
    int r;
    if ((r = check_pci(x)) || (r = read_caps(x)) || (r = ext_caps(x)))
        return r;
    uint32_t sts;
    if (!wait_op(x, OP_USBSTS, STS_CNR, 0, 1000, &sts))
        return fail(x, "start", "Controller Not Ready still set after 1 s (USBSTS %08x)", sts);
    if ((r = stop(x, "halt")) || (r = reset(x, "reset")))
        return r;
    /* Quiet now (halted and reset: it holds no DMA pointer of anyone's):
     * bus mastering on, for our DMA and the MSI, before anything is pinned
     * (drv_vmo_pin refuses until then) or DCBAAP/CRCR/ERST are written. */
    status_t bm = drv_dma_bus_master(x->dma, 1);
    if (bm != OK)
        return fail(x, "bus master", "can't turn it on (%s)", status_str(bm));
    if ((r = setup_memory(x)) || (r = pool_setup(x)))
        return r;
    x->ctl_page = pool_alloc(x);
    if (x->ctl_page < 0)
        return fail(x, "DMA pool", "no page for control transfers");
    status_t st = drv_port_bind(x->port, x->irq, KEY_IRQ, SIG_INTERRUPT, PORT_BIND_PERSISTENT);
    if (st != OK)
        return fail(x, "interrupt", "bind DR_IRQ(0): %s", status_str(st));
    drv_interrupt_ack(x->irq);
    if ((r = run(x)))
        return r;
    power_ports(x);
    drv_log("running: %u ports, MaxSlotsEn %u, %u-byte contexts, %s", x->ports, x->max_slots_en,
            x->csz, x->msix ? "MSI-X" : "MSI");
    return 0;
}

int hc_shutdown(struct hc *x)
{
    if (!x->caplen || !x->nmap || x->map[0].page != 0)
        return 0;
    if (x->rtsoff)
        ir_wr(x, IR_IMAN, IMAN_IP);
    x->map_failed = false;
    int r = stop(x, "final halt");
    return r ? r : reset(x, "final reset");
}

void hc_release(struct hc *x, bool quiet)
{
    if (x->pool_pinned && quiet)
        drv_vmo_unpin(x->pool_vmo, x->dma, x->pool_pin);
    if (x->pool)
        drv_vmo_unmap(x->pool, (uint64_t)POOL_PAGES * PAGE);
    if (x->pool_vmo != HANDLE_INVALID)
        drv_handle_close(x->pool_vmo);
    if (x->pool_addr)
        drv_free(x->pool_addr);
    if (x->ctx_pinned && quiet)
        drv_vmo_unpin(x->ctx_vmo, x->dma, x->ctx_pin);
    if (x->sp_pinned && quiet)
        drv_vmo_unpin(x->sp_vmo, x->dma, x->sp_pin);
    if (x->ctx)
        drv_vmo_unmap(x->ctx, (uint64_t)x->ctx_pages * PAGE);
    if (x->ctx_vmo != HANDLE_INVALID)
        drv_handle_close(x->ctx_vmo);
    if (x->sp_vmo != HANDLE_INVALID)
        drv_handle_close(x->sp_vmo);
    for (unsigned i = 0; i < x->nmap; i++)
        drv_vmo_unmap((void *)x->map[i].va, PAGE);
    x->nmap = 0;
}

/* The device context base address array entry for a slot. */
void hc_set_dcbaa(struct hc *h, uint32_t slot, uint64_t addr)
{
    volatile uint64_t *dcbaa = (volatile uint64_t *)(h->ctx + DMA_DCBAA);
    dcbaa[slot] = addr;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
