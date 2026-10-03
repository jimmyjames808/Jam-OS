/* Tests for the DMAR parser (kernel/acpi/dmar.c) and the VT-d register
 * decoding (kernel/dev/vtd_probe.c), on tables built here: a table shaped
 * like the real PC's is expected to be (a Raptor Lake desktop: the iGPU's
 * unit, the unit for everything else with the PCH's I/O APIC and HPET, an
 * RMRR for the xHCI and one for the iGPU, a SATC; the PC's boot log will
 * say what its firmware really has), and broken ones: lengths that lie,
 * caps overrun, random bytes. The parser reads firmware input, so it must
 * keep inside the table and its own arrays whatever the bytes say. */
#include <jam/dmar.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vtd.h>

/* A table under construction. */
struct tb {
    uint8_t  b[1024];   /* the bytes */
    uint32_t len;       /* used so far */
    uint32_t open;      /* offset of the structure being built (its length grows) */
};

static void put8(struct tb *t, uint8_t v)
{
    if (t->len < sizeof(t->b))
        t->b[t->len++] = v;
}

static void put16(struct tb *t, uint16_t v)
{
    put8(t, (uint8_t)v);
    put8(t, (uint8_t)(v >> 8));
}

static void put32(struct tb *t, uint32_t v)
{
    put16(t, (uint16_t)v);
    put16(t, (uint16_t)(v >> 16));
}

static void put64(struct tb *t, uint64_t v)
{
    put32(t, (uint32_t)v);
    put32(t, (uint32_t)(v >> 32));
}

static void set16(struct tb *t, uint32_t off, uint16_t v)
{
    t->b[off] = (uint8_t)v;
    t->b[off + 1] = (uint8_t)(v >> 8);
}

/* The ACPI header and the DMAR's own fields: 48 bytes. */
static void tb_header(struct tb *t, uint8_t haw_bits, uint8_t flags)
{
    memset(t, 0, sizeof(*t));
    memcpy(t->b, "DMAR", 4);
    t->b[8] = 1;                      /* revision */
    memcpy(t->b + 10, "ALASKA", 6);   /* OEM id, as AMI firmware says it */
    memcpy(t->b + 16, "A M I   ", 8);
    t->len = 36;
    put8(t, (uint8_t)(haw_bits - 1));
    put8(t, flags);
    for (int i = 0; i < 10; i++)
        put8(t, 0);
}

/* Start a structure: its type, a length patched by every put after it. */
static void tb_open(struct tb *t, uint16_t type)
{
    t->open = t->len;
    put16(t, type);
    put16(t, 4);
}

static void tb_close(struct tb *t)
{
    set16(t, t->open + 2, (uint16_t)(t->len - t->open));
}

static void tb_scope(struct tb *t, uint8_t type, uint8_t id, uint8_t bus, uint8_t dev, uint8_t fn)
{
    put8(t, type);
    put8(t, 8);   /* one path step */
    put16(t, 0);
    put8(t, id);
    put8(t, bus);
    put8(t, dev);
    put8(t, fn);
}

static void tb_drhd(struct tb *t, uint8_t flags, uint64_t base)
{
    tb_open(t, DMAR_TYPE_DRHD);
    put8(t, flags);
    put8(t, 0);    /* size: one page */
    put16(t, 0);   /* segment */
    put64(t, base);
}

static void tb_rmrr(struct tb *t, uint64_t base, uint64_t limit)
{
    tb_open(t, DMAR_TYPE_RMRR);
    put16(t, 0);
    put16(t, 0);
    put64(t, base);
    put64(t, limit);
}

/* Set the table's length and checksum: done. */
static void tb_finish(struct tb *t)
{
    t->b[4] = (uint8_t)t->len;
    t->b[5] = (uint8_t)(t->len >> 8);
    t->b[6] = t->b[7] = 0;
    t->b[9] = 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < t->len; i++)
        sum += t->b[i];
    t->b[9] = (uint8_t)-sum;
}

/* The table the PC is expected to have. */
static void tb_pc(struct tb *t)
{
    tb_header(t, 39, DMAR_F_INTR_REMAP | DMAR_F_DMA_CTRL_OPT_IN);
    tb_drhd(t, 0, 0xfed90000);                                   /* the iGPU's unit */
    tb_scope(t, DMAR_SCOPE_ENDPOINT, 0, 0, 0x02, 0);
    tb_close(t);
    tb_drhd(t, DMAR_DRHD_INCLUDE_PCI_ALL, 0xfed91000);           /* everything else */
    tb_scope(t, DMAR_SCOPE_IOAPIC, 2, 0, 0x1e, 7);
    tb_scope(t, DMAR_SCOPE_HPET, 0, 0, 0x1e, 6);
    tb_close(t);
    tb_rmrr(t, 0x3e2e0000, 0x3e2fffff);                          /* the xHCI's */
    tb_scope(t, DMAR_SCOPE_ENDPOINT, 0, 0, 0x14, 0);
    tb_close(t);
    tb_rmrr(t, 0x3c800000, 0x3e7fffff);                          /* the iGPU's stolen memory */
    tb_scope(t, DMAR_SCOPE_ENDPOINT, 0, 0, 0x02, 0);
    tb_close(t);
    tb_open(t, DMAR_TYPE_SATC);
    put8(t, DMAR_SATC_ATC_REQUIRED);
    put8(t, 0);
    put16(t, 0);
    tb_scope(t, DMAR_SCOPE_ENDPOINT, 0, 0, 0x02, 0);
    tb_close(t);
    tb_finish(t);
}

static struct tb *new_tb(void)
{
    struct tb *t = kzalloc(sizeof(struct tb));
    KT_ASSERT(t);
    return t;
}

static struct dmar_info *new_info(void)
{
    struct dmar_info *i = kzalloc(sizeof(struct dmar_info));
    KT_ASSERT(i);
    return i;
}

static void check_scope(const struct dmar_scope *s, uint8_t type, uint8_t id, uint8_t dev,
                        uint8_t fn)
{
    KT_EQ(s->type, type);
    KT_EQ(s->enum_id, id);
    KT_EQ(s->start_bus, 0);
    KT_EQ(s->path_len, 1);
    KT_EQ(s->path_full, 1);
    KT_EQ(s->path[0].dev, dev);
    KT_EQ(s->path[0].fn, fn);
}

KTEST(dmar_parse_pc_table)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    tb_pc(t);
    KT_EQ(dmar_parse(t->b, t->len, d), OK);
    KT_EQ(d->haw, 39);
    KT_EQ(d->flags, DMAR_F_INTR_REMAP | DMAR_F_DMA_CTRL_OPT_IN);
    KT_EQ(d->malformed, 0);
    KT_EQ(d->unknown, 0);
    KT_EQ(d->dropped, 0);
    KT_EQ(d->nunits, 2);
    KT_EQ(d->units[0].base, 0xfed90000);
    KT_EQ(d->units[0].flags, 0);
    KT_EQ(d->units[0].scopes.count, 1);
    check_scope(&d->scopes[d->units[0].scopes.first], DMAR_SCOPE_ENDPOINT, 0, 0x02, 0);
    KT_EQ(d->units[1].base, 0xfed91000);
    KT_EQ(d->units[1].flags, DMAR_DRHD_INCLUDE_PCI_ALL);
    KT_EQ(d->units[1].scopes.count, 2);
    check_scope(&d->scopes[d->units[1].scopes.first], DMAR_SCOPE_IOAPIC, 2, 0x1e, 7);
    check_scope(&d->scopes[d->units[1].scopes.first + 1], DMAR_SCOPE_HPET, 0, 0x1e, 6);
    KT_EQ(d->nrmrrs, 2);
    KT_EQ(d->rmrrs[0].base, 0x3e2e0000);
    KT_EQ(d->rmrrs[0].limit, 0x3e2fffff);
    check_scope(&d->scopes[d->rmrrs[0].scopes.first], DMAR_SCOPE_ENDPOINT, 0, 0x14, 0);
    KT_EQ(d->rmrrs[1].scopes.count, 1);
    KT_EQ(d->nsatcs, 1);
    KT_EQ(d->satcs[0].flags, DMAR_SATC_ATC_REQUIRED);
    check_scope(&d->scopes[d->satcs[0].scopes.first], DMAR_SCOPE_ENDPOINT, 0, 0x02, 0);
    KT_EQ(d->nscopes, 6);
    kfree(d);
    kfree(t);
}

KTEST(dmar_parse_refuses_bad_headers)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    tb_pc(t);
    KT_EQ(dmar_parse(t->b, t->len - 1, d), ERR_INVALID_ARGS);   /* longer than the buffer */
    KT_EQ(d->nunits, 0);
    KT_EQ(dmar_parse(t->b, 40, d), ERR_INVALID_ARGS);           /* shorter than a header */
    KT_EQ(dmar_parse(NULL, 100, d), ERR_INVALID_ARGS);
    t->b[20] ^= 1;                                               /* the checksum */
    KT_EQ(dmar_parse(t->b, t->len, d), ERR_INVALID_ARGS);
    tb_pc(t);
    t->b[0] = 'X';                                               /* the signature */
    KT_EQ(dmar_parse(t->b, t->len, d), ERR_INVALID_ARGS);
    tb_pc(t);
    t->b[4] = 47;                                                /* says 47 bytes */
    t->b[5] = 0;
    KT_EQ(dmar_parse(t->b, t->len, d), ERR_INVALID_ARGS);
    kfree(d);
    kfree(t);
}

/* A structure whose length is impossible ends the walk; what came before
 * it is kept. */
KTEST(dmar_parse_stops_at_a_bad_length)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    static const uint16_t bad[] = { 0, 3, 15, 0xffff };   /* < header, < DRHD's fixed part, too long */
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        tb_pc(t);
        uint32_t second = 48 + t->b[48 + 2];   /* the second DRHD */
        set16(t, second + 2, bad[i]);
        tb_finish(t);
        KT_EQ(dmar_parse(t->b, t->len, d), OK);
        KT_EQ(d->malformed, 1);
        KT_EQ(d->malformed_at, second);
        KT_EQ(d->nunits, 1);
        KT_EQ(d->nrmrrs, 0);
    }
    kfree(d);
    kfree(t);
}

/* Scopes that don't fit their structure, and paths too long to keep. */
KTEST(dmar_parse_bounds_scopes)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    tb_header(t, 39, 0);
    tb_drhd(t, 0, 0xfed90000);
    put8(t, DMAR_SCOPE_BRIDGE);   /* a 6-step path: kept as 4 */
    put8(t, 6 + 12);
    put16(t, 0);
    put8(t, 0);
    put8(t, 0);
    for (int i = 0; i < 6; i++)
        put16(t, (uint16_t)(0x1c + i));
    put8(t, DMAR_SCOPE_ENDPOINT);   /* says 40 bytes; 6 are left */
    put8(t, 40);
    put32(t, 0);
    tb_close(t);
    tb_drhd(t, 0, 0xfed91000);
    put8(t, DMAR_SCOPE_ENDPOINT);   /* an odd path length */
    put8(t, 7);
    put32(t, 0);
    put8(t, 0x14);
    tb_close(t);
    tb_finish(t);
    KT_EQ(dmar_parse(t->b, t->len, d), OK);
    KT_EQ(d->malformed, 0);
    KT_EQ(d->nunits, 2);
    KT_EQ(d->units[0].scopes.count, 1);
    const struct dmar_scope *s = &d->scopes[d->units[0].scopes.first];
    KT_EQ(s->path_full, 6);
    KT_EQ(s->path_len, DMAR_MAX_PATH);
    KT_EQ(s->path[3].dev, 0x1c + 3);   /* put16's low byte is the device, its high the function */
    KT_EQ(s->path[3].fn, 0);
    KT_EQ(d->units[1].scopes.count, 0);
    KT_EQ(d->dropped, 3);   /* the cut path, the scope too long, the odd one */
    kfree(d);
    kfree(t);
}

/* More units and scopes than the arrays hold: counted, not written past. */
KTEST(dmar_parse_caps_overrun)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    tb_header(t, 39, 0);
    for (int u = 0; u < DMAR_MAX_UNITS + 2; u++) {
        tb_drhd(t, 0, 0xfed90000 + 0x1000ull * u);
        for (int s = 0; s < 8; s++)
            tb_scope(t, DMAR_SCOPE_ENDPOINT, 0, 0, (uint8_t)s, 0);
        tb_close(t);
    }
    tb_open(t, 0x77);   /* a type from the future */
    put32(t, 0);
    tb_close(t);
    tb_rmrr(t, 0x1000, 0x1fff);
    tb_close(t);
    tb_finish(t);
    KT_EQ(dmar_parse(t->b, t->len, d), OK);
    KT_EQ(d->nunits, DMAR_MAX_UNITS);
    KT_EQ(d->nscopes, DMAR_MAX_SCOPES);
    KT_EQ(d->dropped, 2 + (DMAR_MAX_UNITS * 8 - DMAR_MAX_SCOPES));
    KT_EQ(d->unknown, 1);
    KT_EQ(d->nrmrrs, 1);   /* the walk went on past all of it */
    for (uint32_t i = 0; i < d->nunits; i++)
        KT_ASSERT(d->units[i].scopes.first + d->units[i].scopes.count <= DMAR_MAX_SCOPES);
    kfree(d);
    kfree(t);
}

/* Random bytes after a valid header: whatever they say, the parser stays
 * inside the table and its arrays. */
KTEST(dmar_parse_random_bytes)
{
    struct tb *t = new_tb();
    struct dmar_info *d = new_info();
    uint64_t rng = 0x5eed0d3a5ull;
    for (int round = 0; round < 300; round++) {
        tb_header(t, 39, 0);
        uint32_t n = 48 + (uint32_t)(kt_rng(&rng) % 400);
        while (t->len < n) {
            uint64_t r = kt_rng(&rng);
            /* Mostly small numbers, so lengths and types are often plausible. */
            put8(t, (r & 3) ? (uint8_t)((r >> 8) % 24) : (uint8_t)(r >> 16));
        }
        tb_finish(t);
        KT_EQ(dmar_parse(t->b, t->len, d), OK);
        KT_ASSERT(d->nunits <= DMAR_MAX_UNITS && d->nrmrrs <= DMAR_MAX_RMRRS);
        KT_ASSERT(d->natsrs <= DMAR_MAX_ATSRS && d->nsatcs <= DMAR_MAX_SATCS);
        KT_ASSERT(d->nandds <= DMAR_MAX_ANDDS && d->nscopes <= DMAR_MAX_SCOPES);
        KT_ASSERT(!d->malformed || (d->malformed_at >= 48 && d->malformed_at < t->len));
        for (uint32_t i = 0; i < d->nscopes; i++)
            KT_ASSERT(d->scopes[i].path_len <= DMAR_MAX_PATH);
        for (uint32_t i = 0; i < d->nandds; i++)
            KT_ASSERT(strlen(d->andds[i].name) < DMAR_NAME_MAX);
    }
    kfree(d);
    kfree(t);
}

/* The decode of an Alder Lake client unit's registers as Linux logs them
 * ("cap d2008c40660462 ecap f050da"): the shape the PC's are expected to
 * have. Every field the IOMMU's design rests on, at its value. */
KTEST(vtd_describe_decodes_caps)
{
    char buf[400];
    vtd_describe_caps(buf, sizeof(buf), 0xd2008c40660462ull, 0xf050daull);
    KT_ASSERT(!strcmp(buf,
        "domains 256, levels 4(48-bit), mgaw 39 bits, cm 0, rwbf 0, superpages 2M+1G, "
        "psi 1 (mamv 18), fault records 1 at 400, drain r1 w1, plmr 1 phmr 1, pi 0; "
        "coherent 0, qi 1, ir 1, eim 1, pt 1, sc 1, dt 0, iotlb at 500, scalable 0, "
        "pasid 0, nest 0"));
    /* QEMU's intel-iommu (caching mode on, 3- and 4-level). */
    vtd_describe_caps(buf, sizeof(buf), 0xd2008c222f0686ull, 0xf00f4aull);
    KT_ASSERT(!strcmp(buf,
        "domains 65536, levels 3(39-bit) 4(48-bit), mgaw 48 bits, cm 1, rwbf 0, "
        "superpages 2M+1G, psi 1 (mamv 18), fault records 1 at 220, drain r1 w1, "
        "plmr 0 phmr 0, pi 0; coherent 0, qi 1, ir 1, eim 0, pt 1, sc 0, dt 0, "
        "iotlb at f0, scalable 0, pasid 0, nest 0"));
    /* A short buffer is cut, never overrun. */
    char small[16];
    memset(small, 'x', sizeof(small));
    vtd_describe_caps(small, 10, 0xd2008c40660462ull, 0xf050daull);
    KT_EQ(strlen(small), 9);
    KT_EQ(small[10], 'x');
}

KTEST(vtd_describe_status_says_what_is_on)
{
    char buf[300];
    vtd_describe_status(buf, sizeof(buf), 0, 0, 0);
    KT_ASSERT(!strcmp(buf,
        "translation off, interrupt remapping off, queued invalidation off, root table not "
        "set, irq table not set, compat irqs -, protected memory off, faults none"));
    vtd_describe_status(buf, sizeof(buf),
                        VTD_GSTS_TES | VTD_GSTS_RTPS | VTD_GSTS_IRES | VTD_GSTS_IRTPS |
                            VTD_GSTS_QIES,
                        VTD_PMEN_PRS, VTD_FSTS_PPF | VTD_FSTS_ITE);
    KT_ASSERT(!strcmp(buf,
        "translation ON, interrupt remapping ON, queued invalidation ON, root table set, "
        "irq table set, compat irqs -, protected memory ON, faults RECORDED, invalidation "
        "errors 40"));
}

/* A fault recording register decoded as VT-d 11.4.7.6 lays it out (F 127,
 * T1 126, FR 103:96, T2 92, SID 79:64, FI 63:12), with the interrupt
 * index of an interrupt-remapping fault in FI 63:48 (5.1.4.1). The values
 * are written as the spec's bits, not through vtd.h's macros. */
KTEST(vtd_describe_fault_per_spec)
{
    char buf[96];
    uint64_t f = 1ull << 63, t1 = 1ull << 62, t2 = 1ull << 28;
    uint64_t hda = 0x00fb;   /* 00:1f.3 */
    /* DMA faults: the page address, the low 12 bits left out. */
    vtd_describe_fault(buf, sizeof(buf), 0x12345abcull, f | 0x05ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 write at 12345000, reason 5"));
    vtd_describe_fault(buf, sizeof(buf), 0x7f000ull, f | t1 | 0x06ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 read at 7f000, reason 6"));
    vtd_describe_fault(buf, sizeof(buf), 0x7f000ull, f | t2 | 0x05ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 page request at 7f000, reason 5"));
    vtd_describe_fault(buf, sizeof(buf), 0x7f000ull, f | t1 | t2 | 0x06ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 atomic at 7f000, reason 6"));
    /* Interrupt-remapping faults: the index, never read or write. */
    vtd_describe_fault(buf, sizeof(buf), 0x1234ull << 48, f | t1 | 0x22ull << 32 | 0xf0f8);
    KT_ASSERT(!strcmp(buf, "f0:1f.0 interrupt, index 1234, reason 22"));
    vtd_describe_fault(buf, sizeof(buf), 0xffffull << 48, f | 0x26ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 interrupt, index ffff, reason 26"));
    vtd_describe_fault(buf, sizeof(buf), 0xdeadbeef000ull, f | 0x25ull << 32 | hda);
    KT_ASSERT(!strcmp(buf, "00:1f.3 interrupt in compatibility format, reason 25"));
}

/* Every register offset and field of <jam/vtd.h> against VT-d 4.1 chapter
 * 11 (the Register Descriptions table, Figures 11-1 to 11-30), with the
 * spec's bit numbers written as literals: a wrong constant can't pass by
 * agreeing with itself. FIELD(m, lo, n): m extracts n bits from lo. */
#define FIELD(m, lo, n)                                                       \
    do {                                                                      \
        uint64_t in_ = (((uint64_t)1 << (n)) - 1) << (lo);                    \
        KT_EQ(m(in_), ((uint64_t)1 << (n)) - 1);                              \
        KT_EQ(m(~in_), 0);                                                    \
    } while (0)

KTEST(vtd_spec_bits_literal)
{
    KT_EQ(VTD_VER, 0x000);
    KT_EQ(VTD_CAP, 0x008);
    KT_EQ(VTD_ECAP, 0x010);
    KT_EQ(VTD_GCMD, 0x018);
    KT_EQ(VTD_GSTS, 0x01c);
    KT_EQ(VTD_RTADDR, 0x020);
    KT_EQ(VTD_CCMD, 0x028);
    KT_EQ(VTD_FSTS, 0x034);
    KT_EQ(VTD_FECTL, 0x038);
    KT_EQ(VTD_FEDATA, 0x03c);
    KT_EQ(VTD_FEADDR, 0x040);
    KT_EQ(VTD_FEUADDR, 0x044);
    KT_EQ(VTD_PMEN, 0x064);
    KT_EQ(VTD_PLMBASE, 0x068);
    KT_EQ(VTD_PLMLIMIT, 0x06c);
    KT_EQ(VTD_PHMBASE, 0x070);
    KT_EQ(VTD_PHMLIMIT, 0x078);
    KT_EQ(VTD_IQH, 0x080);
    KT_EQ(VTD_IQT, 0x088);
    KT_EQ(VTD_IQA, 0x090);
    KT_EQ(VTD_ICS, 0x09c);
    KT_EQ(VTD_IRTA, 0x0b8);
    /* Capability Register (11.4.2). */
    FIELD(VTD_CAP_ND, 0, 3);
    FIELD(VTD_CAP_RWBF, 4, 1);
    FIELD(VTD_CAP_PLMR, 5, 1);
    FIELD(VTD_CAP_PHMR, 6, 1);
    FIELD(VTD_CAP_CM, 7, 1);
    FIELD(VTD_CAP_SAGAW, 8, 5);
    FIELD(VTD_CAP_MGAW, 16, 6);
    FIELD(VTD_CAP_ZLR, 22, 1);
    FIELD(VTD_CAP_FRO, 24, 10);
    FIELD(VTD_CAP_SLLPS, 34, 4);
    FIELD(VTD_CAP_PSI, 39, 1);
    FIELD(VTD_CAP_NFR, 40, 8);
    FIELD(VTD_CAP_MAMV, 48, 6);
    FIELD(VTD_CAP_DWD, 54, 1);
    FIELD(VTD_CAP_DRD, 55, 1);
    FIELD(VTD_CAP_FL1GP, 56, 1);
    FIELD(VTD_CAP_PI, 59, 1);
    FIELD(VTD_CAP_FL5LP, 60, 1);
    FIELD(VTD_CAP_ESIRTPS, 62, 1);
    FIELD(VTD_CAP_ESRTPS, 63, 1);
    /* Extended Capability Register (11.4.3). */
    FIELD(VTD_ECAP_C, 0, 1);
    FIELD(VTD_ECAP_QI, 1, 1);
    FIELD(VTD_ECAP_DT, 2, 1);
    FIELD(VTD_ECAP_IR, 3, 1);
    FIELD(VTD_ECAP_EIM, 4, 1);
    FIELD(VTD_ECAP_PT, 6, 1);
    FIELD(VTD_ECAP_SC, 7, 1);
    FIELD(VTD_ECAP_IRO, 8, 10);
    FIELD(VTD_ECAP_MHMV, 20, 4);
    FIELD(VTD_ECAP_NEST, 26, 1);
    FIELD(VTD_ECAP_PRS, 29, 1);
    FIELD(VTD_ECAP_PASID, 40, 1);
    FIELD(VTD_ECAP_SMTS, 43, 1);
    FIELD(VTD_ECAP_SLTS, 46, 1);
    /* Global Status (11.4.4.2), Fault Status (11.4.7.1), PMEN (11.4.8.1),
     * IRTA (11.4.10). */
    KT_EQ(VTD_GSTS_TES, 0x80000000u);
    KT_EQ(VTD_GSTS_RTPS, 0x40000000u);
    KT_EQ(VTD_GSTS_WBFS, 0x08000000u);
    KT_EQ(VTD_GSTS_QIES, 0x04000000u);
    KT_EQ(VTD_GSTS_IRES, 0x02000000u);
    KT_EQ(VTD_GSTS_IRTPS, 0x01000000u);
    KT_EQ(VTD_GSTS_CFIS, 0x00800000u);
    KT_EQ(VTD_FSTS_PFO, 0x01);
    KT_EQ(VTD_FSTS_PPF, 0x02);
    KT_EQ(VTD_FSTS_IQE, 0x10);
    KT_EQ(VTD_FSTS_ICE, 0x20);
    KT_EQ(VTD_FSTS_ITE, 0x40);
    FIELD(VTD_FSTS_FRI, 8, 8);
    KT_EQ(VTD_PMEN_EPM, 0x80000000u);
    KT_EQ(VTD_PMEN_PRS, 0x1);
    KT_EQ(VTD_IRTA_EIME, 0x800);
    FIELD(VTD_IRTA_S, 0, 4);
    /* A Fault Recording Register's upper half (bits 127:64, 11.4.7.6):
     * F 127, T1 126, FR 103:96, T2 92, SID 79:64; lower half FI 63:12 with
     * an interrupt index in 63:48. */
    KT_EQ(VTD_FRCD_F, 0x8000000000000000ull);
    FIELD(VTD_FRCD_TYPE1, 62, 1);
    FIELD(VTD_FRCD_REASON, 32, 8);
    FIELD(VTD_FRCD_TYPE2, 28, 1);
    FIELD(VTD_FRCD_SID, 0, 16);
    FIELD(VTD_FRCD_INDEX, 48, 16);
}
