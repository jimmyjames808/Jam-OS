/* rtl8125: the listen-only probe's count of frames by tag (drv/rtl8125).
 *
 * Of each frame the probe reads its length (from the descriptor) and
 * bytes 12-17 (copied once out of the buffer), and nothing else: the
 * count holds tags, VLAN ids and EtherTypes, never an address or a
 * payload byte (netframe.h). The probe accepts every frame and keeps
 * every tag, so the count shows what the switch port carries. */
#include "rtl8125.h"

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

unsigned census_harvest(struct rtl *t, bool by_irq)
{
    unsigned n = 0;
    struct rx_slot s;
    for (; n < RX_DESCS && ring_rx_peek(t, &s); n++) {
        if (s.status & RX_ERRSUM) {
            t->c.errors++;
        } else if (!s.whole) {
            t->c.split++;
        } else {
            /* Bytes 12-17 only, copied once; the rest of the frame is never read. */
            uint8_t head[NETFRAME_TAGGED] = { 0 };
            for (unsigned k = 12; k < NETFRAME_TAGGED && k < s.len; k++)
                head[k] = s.buf[k];
            struct netframe_class f = netframe_classify(head, s.len);
            census_count(&t->c, &f);
            if (f.kind != NETFRAME_RUNT) {
                if (by_irq)
                    t->c.by_irq++;
                else
                    t->c.by_poll++;
            }
        }
        ring_rx_done(t);
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
const char *census_verdict(const struct census *c, uint16_t v)
{
    uint32_t tagged = census_kind(c, NETFRAME_VLAN) + census_kind(c, NETFRAME_OUTER);
    if (!v)
        return "no vlan= given: counts only";
    if (census_vlan(c, v))
        return census_kind(c, NETFRAME_UNTAGGED) ? "trunk carrying our VLAN (untagged too: a "
                                                    "native VLAN)" : "trunk carrying our VLAN";
    if (tagged)
        return "tagged frames, but none on our VLAN";
    if (census_kind(c, NETFRAME_UNTAGGED) || census_kind(c, NETFRAME_PRIORITY))
        return "only untagged frames: an access port";
    return "nothing heard";
}
