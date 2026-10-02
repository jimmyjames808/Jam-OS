/* rtl8125: the receive ring and the count of frames by tag (drv/rtl8125).
 *
 * The ring (256 descriptors of 32 bytes) and the tally dump share one
 * contiguous VMO; the 2 KiB buffers (two per page) are an ordinary VMO,
 * pinned page by page. Neither asks for memory below 4 GiB, so on the PC
 * they normally land above it: main.c logs where, which is the test of
 * the chip's 64-bit DMA.
 *
 * Of each frame the driver reads its length (from the descriptor) and
 * bytes 12-17 (copied once out of the buffer), and nothing else: the
 * count holds tags, VLAN ids and EtherTypes, never an address or a
 * payload byte (netframe.h). */
#include "rtl8125.h"

status_t ring_setup(struct rtl *t)
{
    status_t st = drv_vmo_create(RING_VMO, DRV_VMO_CONTIGUOUS, &t->ring_vmo);
    if (st == OK)
        st = drv_vmo_map(t->ring_vmo, 0, RING_VMO, VMAR_READ | VMAR_WRITE, (void **)&t->ring);
    uint64_t addrs[RING_VMO / 4096];
    if (st == OK && (st = drv_vmo_pin(t->ring_vmo, t->dma, 0, RING_VMO, addrs, &t->ring_pin)) == OK)
        t->ring_pinned = true;
    if (st == OK)
        st = drv_vmo_create(BUF_BYTES, 0, &t->buf_vmo);
    if (st == OK)
        st = drv_vmo_map(t->buf_vmo, 0, BUF_BYTES, VMAR_READ | VMAR_WRITE, (void **)&t->bufs);
    if (st == OK && (st = drv_vmo_pin(t->buf_vmo, t->dma, 0, BUF_BYTES, t->buf_addr,
                                      &t->buf_pin)) == OK)
        t->buf_pinned = true;
    if (st != OK) {
        drv_log("ring: can't set it up (%s)", status_str(st));
        return st;
    }
    t->ring_addr = addrs[0];
    for (unsigned i = 0; i < RX_DESCS; i++) {
        volatile uint8_t *d = t->ring + i * RX_DESC_SIZE;
        uint64_t a = t->buf_addr[i / 2] + (i % 2) * RX_BUF;
        *(volatile uint64_t *)(d + RX_DESC_ADDR) = a;
        *(volatile uint32_t *)(d + RX_DESC_EXTSTS) = 0;
        *(volatile uint32_t *)(d + RX_DESC_CMDSTS) =
            RX_OWN | RX_BUF | (i == RX_DESCS - 1 ? RX_EOR : 0);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the chip reads them once RXENB is set */
    uint64_t lo = t->buf_addr[0], hi = t->buf_addr[0];
    for (unsigned i = 1; i < BUF_PAGES; i++) {
        lo = t->buf_addr[i] < lo ? t->buf_addr[i] : lo;
        hi = t->buf_addr[i] > hi ? t->buf_addr[i] : hi;
    }
    drv_log("ring: %u descriptors at %#lx, tally at %#lx, buffers %#lx-%#lx: %s", RX_DESCS,
            (unsigned long)t->ring_addr, (unsigned long)(t->ring_addr + TALLY_OFF),
            (unsigned long)lo, (unsigned long)(hi + 4095),
            t->ring_addr >= (1ull << 32) && lo >= (1ull << 32) ? "all above 4 GiB (64-bit DMA)"
            : t->ring_addr >= (1ull << 32) || hi >= (1ull << 32) ? "partly above 4 GiB"
            : "all below 4 GiB (the 64-bit DMA question stays open)");
    return OK;
}

void ring_free(struct rtl *t)
{
    if (t->buf_pinned && drv_vmo_unpin(t->buf_vmo, t->dma, t->buf_pin) == OK)
        t->buf_pinned = false;
    if (t->ring_pinned && drv_vmo_unpin(t->ring_vmo, t->dma, t->ring_pin) == OK)
        t->ring_pinned = false;
    if (t->bufs)
        (void)drv_vmo_unmap(t->bufs, BUF_BYTES);   /* nothing to do if it fails: we exit */
    if (t->ring)
        (void)drv_vmo_unmap(t->ring, RING_VMO);
    t->bufs = t->ring = NULL;
    if (t->buf_vmo)
        drv_handle_close(t->buf_vmo);
    if (t->ring_vmo)
        drv_handle_close(t->ring_vmo);
    t->buf_vmo = t->ring_vmo = HANDLE_INVALID;
}

static struct group *group_of(struct census *c, const struct netframe_class *f)
{
    for (unsigned i = 0; i < c->ng; i++)
        if (c->g[i].kind == f->kind && c->g[i].tpid == f->tpid && c->g[i].vid == f->vid)
            return &c->g[i];
    if (c->ng == CENSUS_GROUPS)
        return NULL;
    struct group *g = &c->g[c->ng++];
    *g = (struct group){ .kind = (uint8_t)f->kind, .tpid = f->tpid, .vid = f->vid };
    return g;
}

void census_count(struct census *c, const struct netframe_class *f)
{
    if (f->kind == NETFRAME_RUNT) {
        c->runts++;
        return;
    }
    c->frames++;
    struct group *g = group_of(c, f);
    if (!g) {
        c->lost++;
        return;
    }
    g->frames++;
    for (unsigned i = 0; i < CENSUS_TYPES; i++) {
        if (g->type_n[i] && g->type[i] != f->ethertype)
            continue;
        g->type[i] = f->ethertype;
        g->type_n[i]++;
        return;
    }
    g->other_types++;
}

/* Descriptor i handed back to the chip, with its buffer as it was. */
static void rearm(struct rtl *t, unsigned i)
{
    volatile uint8_t *d = t->ring + i * RX_DESC_SIZE;
    *(volatile uint32_t *)(d + RX_DESC_EXTSTS) = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the rest before the ownership bit */
    *(volatile uint32_t *)(d + RX_DESC_CMDSTS) =
        RX_OWN | RX_BUF | (i == RX_DESCS - 1 ? RX_EOR : 0);
}

unsigned census_harvest(struct rtl *t, bool by_irq)
{
    unsigned n = 0;
    for (; n < RX_DESCS; n++) {
        unsigned i = t->next;
        volatile uint8_t *d = t->ring + i * RX_DESC_SIZE;
        uint32_t st = *(volatile uint32_t *)(d + RX_DESC_CMDSTS);
        if (st & RX_OWN)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the buffer after the status */
        uint32_t len = st & RX_LEN;
        if (st & RX_ERRSUM) {
            t->c.errors++;
        } else if ((st & (RX_SOF | RX_EOF)) != (RX_SOF | RX_EOF) || len > RX_BUF) {
            t->c.split++;
        } else {
            /* Bytes 12-17 only, copied once; the rest of the frame is never read. */
            uint8_t head[NETFRAME_TAGGED] = { 0 };
            const volatile uint8_t *b = t->bufs + (size_t)i * RX_BUF;
            for (unsigned k = 12; k < NETFRAME_TAGGED && k < len; k++)
                head[k] = b[k];
            struct netframe_class f = netframe_classify(head, len);
            census_count(&t->c, &f);
            if (f.kind != NETFRAME_RUNT) {
                if (by_irq)
                    t->c.by_irq++;
                else
                    t->c.by_poll++;
            }
        }
        rearm(t, i);
        t->next = (i + 1) % RX_DESCS;
    }
    return n;
}

static const char *group_name(const struct group *g, char *buf, size_t size)
{
    if (g->kind == NETFRAME_UNTAGGED)
        drv_snprintf(buf, size, "untagged");
    else if (g->kind == NETFRAME_PRIORITY)
        drv_snprintf(buf, size, "priority-tagged (vlan 0)");
    else if (g->kind == NETFRAME_VLAN)
        drv_snprintf(buf, size, "vlan %u", g->vid);
    else
        drv_snprintf(buf, size, "outer tag %04x vlan %u", g->tpid, g->vid);
    return buf;
}

void census_log(const struct census *c)
{
    for (unsigned i = 0; i < c->ng; i++) {
        const struct group *g = &c->g[i];
        char name[40], types[160];
        size_t len = 0;
        types[0] = 0;
        for (unsigned k = 0; k < CENSUS_TYPES && g->type_n[k] && len < sizeof(types); k++)
            len += (size_t)drv_snprintf(types + len, sizeof(types) - len, "%s%04x x %u",
                                        k ? ", " : "", g->type[k], g->type_n[k]);
        if (g->other_types && len < sizeof(types))
            drv_snprintf(types + len, sizeof(types) - len, ", other x %u", g->other_types);
        drv_log("census: %s: %u frame(s): %s", group_name(g, name, sizeof(name)), g->frames,
                types);
    }
    drv_log("census: %u frame(s) counted (%u found after an interrupt, %u at a 1 s poll); "
            "%u runt(s), %u with the error bit, %u split, %u past %u kinds of tag", c->frames,
            c->by_irq, c->by_poll, c->runts, c->errors, c->split, c->lost, CENSUS_GROUPS);
}

uint32_t census_vlan(const struct census *c, uint16_t v)
{
    for (unsigned i = 0; i < c->ng; i++)
        if (c->g[i].kind == NETFRAME_VLAN && c->g[i].vid == v)
            return c->g[i].frames;
    return 0;
}

uint32_t census_kind(const struct census *c, enum netframe_kind k)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < c->ng; i++)
        if (c->g[i].kind == k)
            n += c->g[i].frames;
    return n;
}

/* docs/M9-PLAN.md "The first PC stage: listen only", reading the count. */
const char *census_verdict(const struct census *c)
{
    uint32_t tagged = census_kind(c, NETFRAME_VLAN) + census_kind(c, NETFRAME_OUTER);
    if (census_vlan(c, PROBE_VLAN))
        return census_kind(c, NETFRAME_UNTAGGED) ? "trunk carrying 21 (untagged too: a native "
                                                    "VLAN)" : "trunk carrying 21";
    if (tagged)
        return "tagged frames, but none on 21";
    if (census_kind(c, NETFRAME_UNTAGGED) || census_kind(c, NETFRAME_PRIORITY))
        return "only untagged frames: an access port";
    return "nothing heard";
}
