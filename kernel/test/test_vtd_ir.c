/* Tests for the VT-d interrupt remapping entries (kernel/dev/vtd_ir.c):
 * every encoder's exact bits against values worked out by hand from the
 * specification's figures (VT-d 4.1, 5.1.2.2, 5.1.5 and 9.10), its
 * refusals, and the table: allocation, entry 0 never handed out, the
 * index reused only after a completed invalidation, and each change
 * flushed before it is invalidated. As in test_vtd_pt.c the test's flush
 * copies lines into a second copy of the table, "what the unit sees". */
#include <jam/dmar.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "../dev/vtd_ir.h"

/* ---- the encoders --------------------------------------------------------------- */

static struct vtd_irte enc(const struct vtd_irte_spec *s, bool eim)
{
    struct vtd_irte e = { 0, 0 };
    KT_EQ(vtd_irte_encode(s, eim, &e), OK);
    return e;
}

KTEST(vtd_ir_irte_exact_bits)
{
    /* HD Audio at 00:1f.3 (requester id 0x00fb), vector 0x41 to x2APIC id
     * 0x12345678: P, vector at 23:16, destination at 63:32; SID with
     * SVT 01 at 83:82 (bit 18 of the upper half). */
    struct vtd_irte_spec s = {
        .dest = 0x12345678, .vector = 0x41, .src = vtd_ir_source_device(0, 0x1f, 3),
    };
    struct vtd_irte e = enc(&s, true);
    KT_EQ(e.lo, 0x1234567800410001ull);
    KT_EQ(e.hi, 0x00000000000400fbull);
    /* xAPIC format: the 8-bit id at 47:40, nothing else above 31. */
    s.dest = 0x05;
    e = enc(&s, false);
    KT_EQ(e.lo, 0x0000050000410001ull);
    s.dest = 0xff;
    KT_EQ(enc(&s, false).lo, 0x0000ff0000410001ull);
    /* Level-triggered (an I/O APIC pin): TM, bit 4. Vector 0xff. */
    s.dest = 27;
    s.vector = 0xff;
    s.level = true;
    KT_EQ(enc(&s, true).lo, 0x0000001b00ff0011ull);
    /* The I/O APIC behind 00:1e.7 on bus 0xf0, every SQ; then a bus range. */
    s.level = false;
    s.vector = 0x30;
    s.src = (struct vtd_ir_source){ vtd_ir_rid(0xf0, 0x1e, 7), VTD_IR_SVT_RID, VTD_IR_SQ_ALL };
    KT_EQ(enc(&s, true).hi, 0x40000 | 0xf0f7);
    s.src.sq = VTD_IR_SQ_NOT_F2;
    KT_EQ(enc(&s, true).hi, 0x50000 | 0xf0f7);
    s.src.sq = VTD_IR_SQ_NOT_F21;
    KT_EQ(enc(&s, true).hi, 0x60000 | 0xf0f7);
    s.src.sq = VTD_IR_SQ_NOT_F;
    KT_EQ(enc(&s, true).hi, 0x70000 | 0xf0f7);
    s.src = (struct vtd_ir_source){ 0x0305, VTD_IR_SVT_BUS, 0 };   /* buses 3 to 5 */
    KT_EQ(enc(&s, true).hi, 0x80000 | 0x0305);
    s.src = (struct vtd_ir_source){ 0x0707, VTD_IR_SVT_BUS, 0 };   /* bus 7 alone */
    KT_EQ(enc(&s, true).hi, 0x80000 | 0x0707);
    KT_EQ(vtd_ir_rid(0x12, 0x1f, 7), 0x12ff);
    KT_EQ(vtd_ir_rid(0, 1, 0), 0x0008);
}

KTEST(vtd_ir_irte_refuses)
{
    struct vtd_irte e = { 0x1111, 0x2222 };
    struct vtd_irte_spec s = { .dest = 1, .vector = 0x30, .src = vtd_ir_source_device(0, 2, 0) };
    s.vector = 15;
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);   /* an exception vector */
    s.vector = 0;
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    s.vector = 16;
    KT_EQ(vtd_irte_encode(&s, true, &e), OK);
    s.dest = 256;
    KT_EQ(vtd_irte_encode(&s, false, &e), ERR_OUT_OF_RANGE);  /* doesn't fit xAPIC's 8 bits */
    KT_EQ(vtd_irte_encode(&s, true, &e), OK);
    s.dest = 1;
    e = (struct vtd_irte){ 0x1111, 0x2222 };
    s.src.svt = 0;   /* no validation: refused */
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    s.src.svt = 3;   /* reserved */
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    s.src = vtd_ir_source_device(0, 2, 0);
    s.src.sq = 4;
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    s.src = (struct vtd_ir_source){ 0x0503, VTD_IR_SVT_BUS, 0 };   /* 5 to 3: empty */
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    s.src = (struct vtd_ir_source){ 0x0305, VTD_IR_SVT_BUS, 1 };   /* SQ means nothing here */
    KT_EQ(vtd_irte_encode(&s, true, &e), ERR_INVALID_ARGS);
    KT_EQ(e.lo, 0x1111);   /* untouched on failure */
    KT_EQ(e.hi, 0x2222);
}

KTEST(vtd_ir_msi_exact_bits)
{
    /* 0xfee00000, handle 14:0 at 19:5, format bit 4, SHV 0, handle 15 at
     * bit 2; data 0. */
    static const struct { uint32_t index; uint32_t address; } cases[] = {
        { 0, 0xfee00010 },      { 1, 0xfee00030 },      { 0x1234, 0xfee24690 },
        { 0x7fff, 0xfeeffff0 }, { 0x8000, 0xfee00014 }, { 0x8001, 0xfee00034 },
        { 0xffff, 0xfeeffff4 }, { 1023, 0xfee07ff0 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct vtd_ir_msi m = { 1, 1 };
        KT_EQ(vtd_ir_msi_encode(cases[i].index, &m), OK);
        KT_EQ(m.address, cases[i].address);
        KT_EQ(m.data, 0);
        /* Inside the interrupt window, with the remappable format bit. */
        KT_EQ(m.address >> 20, 0xfee);
        KT_ASSERT(m.address & VTD_IR_MSI_FORMAT);
    }
    struct vtd_ir_msi m = { 7, 7 };
    KT_EQ(vtd_ir_msi_encode(0x10000, &m), ERR_OUT_OF_RANGE);
    KT_EQ(m.address, 7);
}

KTEST(vtd_ir_rte_exact_bits)
{
    /* Vector 7:0, index 15 at bit 11, polarity 13, trigger 15, mask 16,
     * format 48, index 14:0 at 63:49. */
    struct vtd_ir_rte_spec s = { .index = 0x8005, .vector = 0x24, .masked = true };
    uint64_t v = 0;
    KT_EQ(vtd_ir_rte_encode(&s, &v), OK);
    KT_EQ(v, 0x000b000000010824ull);
    s = (struct vtd_ir_rte_spec){ .index = 4, .vector = 0x31 };   /* COM1: edge, active high */
    KT_EQ(vtd_ir_rte_encode(&s, &v), OK);
    KT_EQ(v, 0x0009000000000031ull);
    s = (struct vtd_ir_rte_spec){ .index = 0x7fff, .vector = 0xfe, .level = true,
                                  .active_low = true };
    KT_EQ(vtd_ir_rte_encode(&s, &v), OK);
    KT_EQ(v, 0xffff00000000a0feull);
    s.index = 0xffff;
    KT_EQ(vtd_ir_rte_encode(&s, &v), OK);
    KT_EQ(v, 0xffff00000000a8feull);
    v = 9;
    s.index = 0x10000;
    KT_EQ(vtd_ir_rte_encode(&s, &v), ERR_OUT_OF_RANGE);
    s.index = 1;
    s.vector = 15;
    KT_EQ(vtd_ir_rte_encode(&s, &v), ERR_INVALID_ARGS);
    KT_EQ(v, 9);
}

KTEST(vtd_ir_source_from_scope)
{
    struct dmar_scope s = {
        .type = DMAR_SCOPE_IOAPIC, .enum_id = 2, .start_bus = 0, .path_len = 1, .path_full = 1,
    };
    s.path[0].dev = 0x1e;
    s.path[0].fn = 7;
    struct vtd_ir_source src = { 0, 0, 0 };
    KT_EQ(vtd_ir_source_from_scope(&s, &src), OK);
    KT_EQ(src.sid, 0x00f7);
    KT_EQ(src.svt, VTD_IR_SVT_RID);
    KT_EQ(src.sq, VTD_IR_SQ_ALL);
    s.type = DMAR_SCOPE_HPET;
    s.start_bus = 0xf0;
    s.path[0].fn = 6;
    KT_EQ(vtd_ir_source_from_scope(&s, &src), OK);
    KT_EQ(src.sid, 0xf0f6);
    s.path_len = s.path_full = 2;   /* behind a bridge */
    KT_EQ(vtd_ir_source_from_scope(&s, &src), ERR_NOT_SUPPORTED);
    s.path_len = s.path_full = 1;
    s.type = DMAR_SCOPE_ENDPOINT;
    KT_EQ(vtd_ir_source_from_scope(&s, &src), ERR_INVALID_ARGS);
    s.type = DMAR_SCOPE_IOAPIC;
    s.path[0].dev = 32;   /* the table said so; no such device */
    KT_EQ(vtd_ir_source_from_scope(&s, &src), ERR_INVALID_ARGS);
    KT_EQ(src.sid, 0xf0f6);
}

/* ---- the table ------------------------------------------------------------------ */

struct ictx {
    struct vtd_ir_table t;
    struct vtd_irte    *dev;          /* the unit's view (a copy, changed by flushes) */
    unsigned            dev_order;    /* its pages, as an order */
    unsigned            flushes, invals;
    uint32_t            last_index;   /* the last invalidation's range */
    uint32_t            last_count;
    status_t            inval_result; /* the next invalidate fails with this, once */
};

static void i_flush(void *c, const void *va, size_t len)
{
    struct ictx *x = c;
    uint64_t off = (uint64_t)((const char *)va - (const char *)x->t.entries);
    KT_ASSERT(off < (uint64_t)x->t.size * sizeof(struct vtd_irte) &&
              len <= x->t.size * sizeof(struct vtd_irte) - off);
    uint64_t lo = ALIGN_DOWN(off, 64), hi = ALIGN_UP(off + len, 64);
    memcpy((char *)x->dev + lo, (char *)x->t.entries + lo, hi - lo);
    x->flushes++;
}

/* The unit must see the entries as they are before it is told to drop
 * its cached copies. */
static status_t i_inval(void *c, uint32_t index, uint32_t count)
{
    struct ictx *x = c;
    KT_ASSERT(index + count <= x->t.size);
    for (uint32_t i = index; i < index + count; i++) {
        KT_EQ(x->dev[i].lo, x->t.entries[i].lo);
        KT_EQ(x->dev[i].hi, x->t.entries[i].hi);
    }
    x->last_index = index;
    x->last_count = count;
    if (x->inval_result != OK) {
        status_t st = x->inval_result;
        x->inval_result = OK;
        return st;
    }
    x->invals++;
    return OK;
}

static const struct vtd_ir_ops i_ops = { .flush = i_flush, .invalidate = i_inval };

/* A table whose unit's view starts as garbage: the creation's flush must
 * replace it with zeros. */
static struct ictx *new_table(uint32_t entries, bool eim)
{
    struct ictx *x = kzalloc(sizeof(*x));
    KT_ASSERT(x);
    x->dev_order = 0;
    while ((PAGE_SIZE << x->dev_order) < entries * sizeof(struct vtd_irte))
        x->dev_order++;
    struct page *pg = pmm_alloc_pages(x->dev_order, 0);
    KT_ASSERT(pg);
    x->dev = page_to_virt(pg);
    memset(x->dev, 0xa5, PAGE_SIZE << x->dev_order);
    KT_EQ(vtd_ir_table_init(&x->t, entries, eim, &i_ops, x), OK);
    for (uint32_t i = 0; i < entries; i++)
        KT_ASSERT(x->dev[i].lo == 0 && x->dev[i].hi == 0);
    return x;
}

static void drop_table(struct ictx *x)
{
    vtd_ir_table_destroy(&x->t);
    pmm_free_pages(virt_to_page(x->dev), x->dev_order);
    kfree(x);
}

KTEST(vtd_ir_table_shape)
{
    struct ictx *x = new_table(1024, true);
    KT_EQ(x->t.phys & (PAGE_SIZE - 1), 0);
    KT_EQ(vtd_ir_irta(&x->t), x->t.phys | VTD_IRTA_EIME | 9);   /* 2^(9 + 1) = 1024 */
    KT_EQ(VTD_IRTA_S(vtd_ir_irta(&x->t)), 9);
    for (uint32_t i = 0; i < 1024; i++)
        KT_ASSERT(x->t.entries[i].lo == 0 && x->t.entries[i].hi == 0);
    drop_table(x);
    x = new_table(256, false);
    KT_EQ(vtd_ir_irta(&x->t), x->t.phys | 7);
    drop_table(x);
    x = new_table(65536, true);
    KT_EQ(vtd_ir_irta(&x->t) & 0xfff, VTD_IRTA_EIME | 15);
    drop_table(x);
    struct vtd_ir_table t;
    static const uint32_t bad[] = { 0, 128, 300, 1000, 131072 };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        KT_EQ(vtd_ir_table_init(&t, bad[i], true, &i_ops, NULL), ERR_INVALID_ARGS);
    static const struct vtd_ir_ops no_inval = { .flush = i_flush };
    KT_EQ(vtd_ir_table_init(&t, 256, true, &no_inval, NULL), ERR_INVALID_ARGS);
}

KTEST(vtd_ir_table_alloc_and_free)
{
    struct ictx *x = new_table(256, true);
    uint32_t idx[255];
    for (uint32_t i = 0; i < 255; i++) {
        KT_EQ(vtd_ir_alloc(&x->t, &idx[i]), OK);
        KT_EQ(idx[i], i + 1);   /* entry 0 is never handed out */
    }
    uint32_t more = 77;
    KT_EQ(vtd_ir_alloc(&x->t, &more), ERR_NO_RESOURCES);
    KT_EQ(more, 77);
    KT_EQ(x->t.nused, 255);
    /* Free one: the index comes back only after its invalidation. */
    KT_EQ(vtd_ir_free(&x->t, 100), OK);
    KT_EQ(x->last_index, 100);
    KT_EQ(x->last_count, 1);
    KT_EQ(vtd_ir_alloc(&x->t, &more), OK);
    KT_EQ(more, 100);
    /* A failed invalidation keeps the index taken for good. */
    x->inval_result = ERR_TIMED_OUT;
    KT_EQ(vtd_ir_free(&x->t, 200), ERR_TIMED_OUT);
    KT_EQ(vtd_ir_alloc(&x->t, &more), ERR_NO_RESOURCES);
    KT_EQ(x->t.nused, 255);
    /* Indices that aren't allocated. */
    KT_EQ(vtd_ir_free(&x->t, 0), ERR_INVALID_ARGS);
    KT_EQ(vtd_ir_free(&x->t, 256), ERR_INVALID_ARGS);
    KT_EQ(vtd_ir_free(&x->t, 5), OK);
    KT_EQ(vtd_ir_free(&x->t, 5), ERR_INVALID_ARGS);
    /* The search goes on from the last one handed out and wraps. */
    KT_EQ(vtd_ir_free(&x->t, 3), OK);
    KT_EQ(vtd_ir_alloc(&x->t, &more), OK);
    KT_EQ(more, 3);   /* 101..255 are taken: wrapped past 0 to 3 */
    KT_EQ(vtd_ir_alloc(&x->t, &more), OK);
    KT_EQ(more, 5);
    drop_table(x);
}

KTEST(vtd_ir_table_set_entries)
{
    struct ictx *x = new_table(256, true);
    uint32_t a, b;
    KT_EQ(vtd_ir_alloc(&x->t, &a), OK);
    KT_EQ(vtd_ir_alloc(&x->t, &b), OK);
    struct vtd_irte_spec s = {
        .dest = 3, .vector = 0x40, .src = vtd_ir_source_device(1, 0, 0),
    };
    KT_EQ(vtd_ir_set(&x->t, a, &s), OK);   /* i_inval checks the unit already sees it */
    KT_EQ(x->invals, 1);
    KT_EQ(x->last_index, a);
    KT_EQ(x->dev[a].lo, 0x0000000300400001ull);
    KT_EQ(x->dev[a].hi, 0x40100);
    KT_EQ(x->dev[b].lo, 0);   /* its neighbour untouched */
    /* Move it to another CPU and vector: same source, lower half only. */
    s.dest = 27;
    s.vector = 0x41;
    KT_EQ(vtd_ir_set(&x->t, a, &s), OK);
    KT_EQ(x->dev[a].lo, 0x0000001b00410001ull);
    KT_EQ(x->invals, 2);
    /* Another source while present: refused, nothing written. */
    s.src = vtd_ir_source_device(2, 0, 0);
    KT_EQ(vtd_ir_set(&x->t, a, &s), ERR_BAD_STATE);
    KT_EQ(x->dev[a].hi, 0x40100);
    KT_EQ(x->invals, 2);
    /* Not allocated, or a bad spec: refused before anything is written. */
    KT_EQ(vtd_ir_set(&x->t, 0, &s), ERR_INVALID_ARGS);
    KT_EQ(vtd_ir_set(&x->t, b + 1, &s), ERR_INVALID_ARGS);
    s.vector = 3;
    KT_EQ(vtd_ir_set(&x->t, b, &s), ERR_INVALID_ARGS);
    KT_EQ(x->dev[b].lo, 0);
    /* An error from the invalidation comes back; the entry is written. */
    s.vector = 0x50;
    x->inval_result = ERR_TIMED_OUT;
    KT_EQ(vtd_ir_set(&x->t, b, &s), ERR_TIMED_OUT);
    KT_EQ(x->dev[b].lo & VTD_IRTE_P, VTD_IRTE_P);
    /* Freed: cleared in the unit's view before the invalidation. */
    KT_EQ(vtd_ir_free(&x->t, a), OK);
    KT_EQ(x->dev[a].lo, 0);
    KT_EQ(x->dev[a].hi, 0);
    KT_EQ(x->last_index, a);
    /* Taken again, the entry can have a new source. */
    uint32_t c;
    KT_EQ(vtd_ir_alloc(&x->t, &c), OK);
    KT_EQ(vtd_ir_free(&x->t, c), OK);
    KT_EQ(vtd_ir_free(&x->t, b), OK);
    drop_table(x);

    /* xAPIC table: an id over 255 can't be written. */
    x = new_table(256, false);
    KT_EQ(vtd_ir_alloc(&x->t, &a), OK);
    s = (struct vtd_irte_spec){ .dest = 256, .vector = 0x40, .src = vtd_ir_source_device(1, 0, 0) };
    KT_EQ(vtd_ir_set(&x->t, a, &s), ERR_OUT_OF_RANGE);
    s.dest = 200;
    KT_EQ(vtd_ir_set(&x->t, a, &s), OK);
    KT_EQ(x->dev[a].lo, 0x0000c80000400001ull);
    KT_EQ(vtd_ir_free(&x->t, a), OK);
    drop_table(x);
}

/* A coherent unit (no flush op) works the same, with no flushes. */
KTEST(vtd_ir_table_coherent)
{
    static const struct vtd_ir_ops ops = { .invalidate = i_inval };
    struct ictx *x = kzalloc(sizeof(*x));
    KT_ASSERT(x);
    KT_EQ(vtd_ir_table_init(&x->t, 256, true, &ops, x), OK);
    x->dev = x->t.entries;   /* the unit reads the memory itself */
    uint32_t a;
    struct vtd_irte_spec s = { .dest = 1, .vector = 0x40, .src = vtd_ir_source_device(0, 3, 0) };
    KT_EQ(vtd_ir_alloc(&x->t, &a), OK);
    KT_EQ(vtd_ir_set(&x->t, a, &s), OK);
    KT_EQ(vtd_ir_free(&x->t, a), OK);
    KT_EQ(x->flushes, 0);
    KT_EQ(x->invals, 2);
    vtd_ir_table_destroy(&x->t);
    kfree(x);
}
