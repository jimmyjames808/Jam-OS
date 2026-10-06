/* Tests for the VT-d units (kernel/dev/vtd_unit.c, vtd_qi.c, vtd_fault.c).
 *
 * Pure ones run everywhere: every descriptor's exact bits against values
 * worked out by hand from the specification's figures (VT-d 4.1, 6.5.2.1,
 * 6.5.2.3, 6.5.2.7, 6.5.2.8), the new register bits against 11.4, the
 * aligned power-of-two split of page and entry runs (6.5.2.3, Table 19),
 * the fault line and per-device counts, the boot words and kexec's keeping
 * of them, the page-table geometry with CAP.RWBF.
 *
 * The rest need a started unit (QEMU's intel-iommu and the boot word
 * iommu=on: tools/vtd-test.sh runs them with caching mode on and off) and
 * skip themselves without one: every invalidation kind completes, through
 * the vtd_pt and vtd_ir callbacks too; a descriptor the unit refuses is
 * reported (ERR_IO, the IQE count, the fault interrupt) and the queue goes
 * on; the queue wraps many times; every CPU submits at once; the queue is
 * turned off and on again (the takeover path). */
#include <jam/kexec.h>
#include <jam/kexec_handoff.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "../dev/vtd_internal.h"

/* The first started unit, or skip the test (return from it). */
#define NEED_UNIT(u)                                                             \
    struct vtd_unit *u = vtd_unit_get(0);                                        \
    do {                                                                         \
        if (!u) {                                                                \
            kprintf("ktest %s: no started VT-d unit (iommu=on), skipped\n",     \
                    ktest_current);                                              \
            return;                                                              \
        }                                                                        \
    } while (0)

static uint64_t stat(const uint64_t *v)
{
    return __atomic_load_n(v, __ATOMIC_RELAXED);
}

/* ---- pure ------------------------------------------------------------------------ */

KTEST(vtd_unit_desc_exact_bits)
{
    /* Context cache (Figure 6-1): type 1, G 5:4, DID 31:16, SID 47:32, FM 49:48. */
    struct vtd_desc d = vtd_desc_cc(VTD_CC_GLOBAL, 0, 0, 0);
    KT_EQ(d.lo, 0x11);
    KT_EQ(d.hi, 0);
    KT_EQ(vtd_desc_cc(VTD_CC_DOMAIN, 0x1234, 0, 0).lo, 0x12340021);
    d = vtd_desc_cc(VTD_CC_DEVICE, 5, 0x00fb, 3);   /* 00:1f.3, phantom bits 2:0 masked */
    KT_EQ(d.lo, 0x000300fb00050031ull);
    KT_EQ(d.hi, 0);
    /* IOTLB (Figure 6-3): type 2, G 5:4, DW 6, DR 7, DID 31:16; AM 69:64,
     * IH 70, ADDR 127:76. */
    d = vtd_desc_iotlb(VTD_IOTLB_GLOBAL, 0, 3, 0x5000, 4, true);
    KT_EQ(d.lo, 0xd2);
    KT_EQ(d.hi, 0);   /* address, mask and hint only for page-selective */
    d = vtd_desc_iotlb(VTD_IOTLB_DOMAIN, 7, 0, 0, 0, false);
    KT_EQ(d.lo, 0x70022);
    KT_EQ(d.hi, 0);
    d = vtd_desc_iotlb(VTD_IOTLB_PAGE, 9, 1, 0x12345000, 3, true);
    KT_EQ(d.lo, 0x900b2);
    KT_EQ(d.hi, 0x12345043);
    d = vtd_desc_iotlb(VTD_IOTLB_PAGE, 0xffff, 2, 0xfffff000ull << 12, 18, false);
    KT_EQ(d.lo, 0xffff0072ull);
    KT_EQ(d.hi, 0xfffff000000ull | 18);
    /* Interrupt entry cache (Figure 6-7): type 4, G bit 4, IM 31:27, IIDX 47:32. */
    d = vtd_desc_iec(true, 0x55, 3);
    KT_EQ(d.lo, 0x4);
    KT_EQ(d.hi, 0);
    KT_EQ(vtd_desc_iec(false, 0x1234, 2).lo, 0x0000123410000014ull);
    KT_EQ(vtd_desc_iec(false, 0xffff, 15).lo, 0x0000ffff78000014ull);
    /* Wait (Figure 6-8): type 5, IF 4 clear, SW 5, FN 6, data 63:32,
     * address 127:66 (bits 1:0 dropped). */
    d = vtd_desc_wait(0x7bf95008, 0xdeadbeef);
    KT_EQ(d.lo, 0xdeadbeef00000065ull);
    KT_EQ(d.hi, 0x7bf95008);
    KT_EQ(vtd_desc_wait(0x1003, 1).hi, 0x1000);
}

KTEST(vtd_unit_register_bits_literal)
{
    /* 11.4.4.1, 11.4.6, 11.4.7.2, 11.4.9 */
    KT_EQ(VTD_GCMD_TE, 0x80000000u);
    KT_EQ(VTD_GCMD_SRTP, 0x40000000u);
    KT_EQ(VTD_GCMD_WBF, 0x08000000u);
    KT_EQ(VTD_GCMD_QIE, 0x04000000u);
    KT_EQ(VTD_GCMD_IRE, 0x02000000u);
    KT_EQ(VTD_GCMD_SIRTP, 0x01000000u);
    KT_EQ(VTD_GCMD_CFI, 0x00800000u);
    KT_EQ(VTD_GSTS_KEEP, 0x96ffffffu);
    /* The one-shot status bits are what KEEP drops. */
    KT_EQ(VTD_GSTS_KEEP & (VTD_GSTS_RTPS | VTD_GSTS_WBFS | VTD_GSTS_IRTPS), 0);
    KT_EQ(VTD_GSTS_KEEP & (VTD_GSTS_TES | VTD_GSTS_QIES | VTD_GSTS_IRES | VTD_GSTS_CFIS),
          VTD_GSTS_TES | VTD_GSTS_QIES | VTD_GSTS_IRES | VTD_GSTS_CFIS);
    KT_EQ(VTD_FECTL_IM, 0x80000000u);
    KT_EQ(VTD_FECTL_IP, 0x40000000u);
    KT_EQ(VTD_CCMD_ICC, 0x8000000000000000ull);
    KT_EQ(VTD_IOTLB_IVT, 0x8000000000000000ull);
    KT_EQ(VTD_IOTLB_REG_OFF, 8);
    KT_EQ(VTD_IQERCD, 0xb0);
    KT_EQ(VTD_IQ_SHIFT, 4);
    KT_EQ(VTD_IQA_DW, 0x800);
    KT_EQ(VTD_IQA_QS(0x12345007), 7);
    KT_EQ(VTD_IQERCD_IQEI(0x1234000000000003ull), 3);
}

/* Is b[0..k) an exact, in-order cover of [first, first + n) by aligned
 * power-of-two blocks of at most 2^max? */
static bool covers(const struct vtd_block *b, int k, uint64_t first, uint64_t n, unsigned max)
{
    uint64_t at = first;
    for (int i = 0; i < k; i++) {
        if (b[i].first != at || b[i].order > max)
            return false;
        if (b[i].first & ((1ull << b[i].order) - 1))
            return false;
        at += 1ull << b[i].order;
    }
    return at == first + n;
}

KTEST(vtd_unit_split_aligned)
{
    struct vtd_block b[64];
    /* 0x1001 + 7 pages: 1 @ 0x1001, 2 @ 0x1002, 4 @ 0x1004. */
    KT_EQ(vtd_split_aligned(0x1001, 7, 18, b, 64), 3);
    KT_ASSERT(b[0].first == 0x1001 && b[0].order == 0);
    KT_ASSERT(b[1].first == 0x1002 && b[1].order == 1);
    KT_ASSERT(b[2].first == 0x1004 && b[2].order == 2);
    KT_EQ(vtd_split_aligned(0, 8, 18, b, 64), 1);
    KT_EQ(b[0].order, 3);
    KT_EQ(vtd_split_aligned(0, 1ull << 20, 18, b, 64), 4);   /* capped at 2^MAMV */
    KT_ASSERT(covers(b, 4, 0, 1ull << 20, 18));
    KT_EQ(vtd_split_aligned(5, 0, 18, b, 64), 0);
    KT_EQ(vtd_split_aligned(3, 5, 0, b, 64), 5);             /* MAMV 0: page by page */
    KT_EQ(vtd_split_aligned(1, 1000, 0, b, 8), -1);          /* too many: the caller widens */
    KT_EQ(vtd_split_aligned(0x7ff, 2, 18, b, 64), 2);        /* across a big boundary */
    KT_ASSERT(covers(b, 2, 0x7ff, 2, 18));
    /* Random runs: always an exact aligned cover, never more than
     * 2 * log2 blocks per order range. */
    uint64_t rng = 0x5eed;
    for (int i = 0; i < 2000; i++) {
        uint64_t first = kt_rng(&rng) & 0xfffff, n = 1 + (kt_rng(&rng) & 0x3fff);
        unsigned max = (unsigned)(kt_rng(&rng) % 20);
        int k = vtd_split_aligned(first, n, max, b, 64);
        if (k < 0)
            continue;   /* MAMV small and the run long: fine, widened */
        KT_ASSERT(covers(b, k, first, n, max));
    }
}

KTEST(vtd_unit_fault_line_and_counts)
{
    char buf[192];
    /* HD Audio (00:1f.3) writing a page that isn't mapped: reason 5. */
    uint64_t hi = VTD_FRCD_F | 5ull << 32 | 0x00fb, lo = 0x12345678;
    vtd_fault_line(buf, sizeof(buf), 0, lo, hi);
    KT_ASSERT(!strcmp(buf, "vtd: fault: unit 0: 00:1f.3 write at 12345000, reason 5: "
                           "write to a page not mapped writable"));
    /* A read (T1 = 1), reason 6. */
    vtd_fault_line(buf, sizeof(buf), 1, 0x1000, VTD_FRCD_F | 1ull << 62 | 6ull << 32 | 0x0018);
    KT_ASSERT(!strcmp(buf, "vtd: fault: unit 1: 00:03.0 read at 1000, reason 6: "
                           "read from a page not mapped readable"));
    /* An interrupt from the wrong requester: the index in 63:48. */
    vtd_fault_line(buf, sizeof(buf), 0, 7ull << 48, VTD_FRCD_F | 0x26ull << 32 | 0x0020);
    KT_ASSERT(!strcmp(buf, "vtd: fault: unit 0: 00:04.0 interrupt, index 7, reason 26: "
                           "interrupt from a requester the entry doesn't allow"));
    /* A reason Jam OS can't cause: the hex alone. */
    vtd_fault_line(buf, sizeof(buf), 0, 0x2000, VTD_FRCD_F | 0x99ull << 32 | 0x0020);
    KT_ASSERT(!strcmp(buf, "vtd: fault: unit 0: 00:04.0 write at 2000, reason 99"));
    KT_ASSERT(vtd_fault_reason_words(0x25) != NULL);
    KT_ASSERT(vtd_fault_reason_words(0) == NULL);

    static struct vtd_fault_counts c;
    memset(&c, 0, sizeof(c));
    KT_EQ(vtd_fault_counts_add(&c, 0, 0x00fb), 1);
    KT_EQ(vtd_fault_counts_add(&c, 0, 0x00fb), 2);
    KT_EQ(vtd_fault_counts_add(&c, 1, 0x00fb), 1);   /* another unit: another device */
    KT_EQ(vtd_fault_counts_add(&c, 0, 0x0018), 1);
    for (uint32_t i = 3; i < VTD_FAULT_DEVS; i++)
        KT_EQ(vtd_fault_counts_add(&c, 2, (uint16_t)i), 1);
    KT_EQ(c.ndev, VTD_FAULT_DEVS);
    KT_EQ(vtd_fault_counts_add(&c, 3, 1), 0);        /* full: counted together */
    KT_EQ(c.other, 1);
    KT_EQ(vtd_fault_counts_add(&c, 0, 0x00fb), 3);   /* known ones still counted */
}

KTEST(vtd_unit_boot_words)
{
    KT_ASSERT(vtd_iommu_wanted(""));                   /* on by default */
    KT_ASSERT(vtd_iommu_wanted("shell verbose"));
    KT_ASSERT(vtd_iommu_wanted("iommu=on"));
    KT_ASSERT(vtd_iommu_wanted("shell iommu=on verbose"));
    KT_ASSERT(!vtd_iommu_wanted("iommu=off"));
    KT_ASSERT(!vtd_iommu_wanted("iommu=on iommu=off"));  /* off wins */
    KT_ASSERT(!vtd_iommu_wanted("iommu=off iommu=on"));
    /* Only the whole word iommu=off turns it off. */
    KT_ASSERT(vtd_iommu_wanted("iommu=offx"));
    KT_ASSERT(vtd_iommu_wanted("xiommu=off"));
    KT_ASSERT(vtd_iommu_wanted("iommu"));
    KT_ASSERT(vtd_iommu_wanted("iommu=of"));
    KT_ASSERT(!vtd_iommu_wanted("shell iommu=off verbose"));
    /* kexec keeps either word: a reboot comes back in the same mode. */
    char buf[KEXEC_CMDLINE];
    kexec_next_cmdline("ktest=vtd iommu=on init", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "iommu=on"));
    kexec_next_cmdline("shell iommu=off", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "shell iommu=off"));
    kexec_next_cmdline("iommu= iommux=on", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, ""));
}

KTEST(vtd_unit_geometry_and_rwbf)
{
    /* The PC's expected unit (cap d2008c40660462 ecap f050da): 4 levels,
     * 39 bits, CM 0, RWBF 0. */
    struct vtd_unit u;
    memset(&u, 0, sizeof(u));
    u.cap = 0xd2008c40660462ull;
    u.ecap = 0xf050da;
    u.haw = 39;
    struct vtd_pt_geom g;
    KT_EQ(vtd_unit_pt_geom(&u, &g), OK);
    KT_EQ(g.levels, 4);
    KT_EQ(g.addr_bits, 39);
    KT_ASSERT(!g.caching_mode);
    KT_ASSERT(!g.coherent);
    KT_ASSERT(g.snoop);
    /* The same with RWBF: new mappings get invalidated (the implicit
     * write-buffer flush, VT-d 6.8). */
    u.cap |= 1ull << 4;
    KT_EQ(vtd_unit_pt_geom(&u, &g), OK);
    KT_ASSERT(g.caching_mode);
}

/* ---- with a started unit ---------------------------------------------------- */

KTEST(vtd_unit_registers_kept)
{
    NEED_UNIT(u);
    KT_ASSERT(vtd_regs_overlap(u->base, 1));
    KT_ASSERT(vtd_regs_overlap(u->base + PAGE_SIZE - 1, 16));
    KT_ASSERT(vtd_regs_overlap(u->base - PAGE_SIZE, 2 * PAGE_SIZE));
    KT_ASSERT(!vtd_regs_overlap(u->base - PAGE_SIZE, PAGE_SIZE));
    KT_EQ(resource_phys_mappable(u->base, PAGE_SIZE), ERR_ACCESS_DENIED);
}

KTEST(vtd_unit_every_invalidation_completes)
{
    NEED_UNIT(u);
    uint64_t s0 = stat(&u->stats.submissions), d0 = stat(&u->stats.descriptors);
    uint64_t r0 = stat(&u->stats.refused), t0 = stat(&u->stats.timeouts);
    KT_EQ(vtd_inv_context_global(u), OK);
    KT_EQ(vtd_inv_context_domain(u, 1), OK);
    KT_EQ(vtd_inv_context_device(u, 1, 0x0018, 0), OK);
    KT_EQ(vtd_inv_context_device(u, 1, 0x0018, 4), ERR_INVALID_ARGS);
    KT_EQ(vtd_inv_iotlb_global(u), OK);
    KT_EQ(vtd_inv_iotlb_domain(u, 1), OK);
    KT_EQ(vtd_inv_iec_global(u), OK);
    KT_EQ(stat(&u->stats.submissions) - s0, 6);
    KT_EQ(stat(&u->stats.descriptors) - d0, 12);   /* each with its wait */
    /* Page-selective: 7 pages from 0x1001000 are runs of 1, 2 and 4. */
    d0 = stat(&u->stats.descriptors);
    KT_EQ(vtd_inv_iotlb_pages(u, 1, 0x1001000, 7, true), OK);
    KT_EQ(stat(&u->stats.descriptors) - d0, VTD_CAP_PSI(u->cap) ? 4 : 2);
    KT_EQ(vtd_inv_iotlb_pages(u, 1, 0x1000000, 0, false), OK);   /* nothing to do */
    KT_EQ(vtd_inv_iotlb_pages(u, 1, 0, 1ull << 30, false), OK);  /* widened to the domain */
    /* Interrupt entries, runs too. */
    KT_EQ(vtd_inv_iec_index(u, 5, 3), OK);
    KT_EQ(vtd_inv_iec_index(u, 0xffff, 1), OK);
    KT_EQ(vtd_inv_iec_index(u, 0xffff, 2), ERR_OUT_OF_RANGE);
    KT_EQ(vtd_inv_iec_index(u, 1, 0), ERR_OUT_OF_RANGE);
    KT_EQ(vtd_unit_flush_write_buffer(u), OK);
    uint64_t domains = 1ull << (4 + 2 * VTD_CAP_ND(u->cap));
    if (domains <= 0xffff)
        KT_EQ(vtd_inv_iotlb_domain(u, (uint16_t)domains), ERR_OUT_OF_RANGE);
    KT_EQ(stat(&u->stats.refused), r0);
    KT_EQ(stat(&u->stats.timeouts), t0);
}

/* A test page "pinned" for a pretend device. */
static uint64_t test_page(void)
{
    uint64_t pa = pmm_alloc_page_phys(0);
    KT_ASSERT(pa);
    return pa;
}

KTEST(vtd_unit_callbacks_invalidate)
{
    NEED_UNIT(u);
    uint64_t t0 = stat(&u->stats.timeouts), r0 = stat(&u->stats.refused);
    /* vtd_pt on the unit's callbacks: a domain no context entry names
     * (translation is off), so only the invalidations reach the unit. */
    struct vtd_pt_geom geom;
    KT_EQ(vtd_unit_pt_geom(u, &geom), OK);
    struct vtd_unit_domain dom = { u, 3 };
    static struct vtd_pt pt;
    KT_EQ(vtd_pt_init(&pt, &geom, &vtd_unit_pt_ops, &dom, NULL, 16), OK);
    uint64_t pages[4];
    for (int i = 0; i < 4; i++)
        pages[i] = test_page();
    struct vtd_pt_gather g;
    uint64_t s0 = stat(&u->stats.submissions);
    for (int i = 0; i < 4; i++) {
        vtd_pt_gather_init(&g);
        KT_EQ(vtd_pt_map(&pt, pages[i], 1, &g), OK);
        KT_EQ(vtd_pt_gather_finish(&pt, &g), OK);
    }
    /* With CM (or RWBF) every new mapping is invalidated; without, none. */
    KT_EQ(stat(&u->stats.submissions) - s0, geom.caching_mode ? 4 : 0);
    s0 = stat(&u->stats.submissions);
    vtd_pt_gather_init(&g);
    for (int i = 0; i < 4; i++)
        KT_EQ(vtd_pt_unmap(&pt, pages[i], 1, &g), OK);
    KT_ASSERT(g.ntables > 0);   /* the tables emptied: invalidated with IH = 0 */
    KT_EQ(vtd_pt_gather_finish(&pt, &g), OK);
    KT_EQ(stat(&u->stats.submissions) - s0, 1);
    KT_EQ(vtd_pt_destroy(&pt), OK);
    for (int i = 0; i < 4; i++)
        pmm_free_page_phys(pages[i]);
    /* vtd_ir on the unit's callbacks: the table isn't the unit's (IRTA is
     * not set), the entry cache invalidations still go through. */
    static struct vtd_ir_table t;
    KT_EQ(vtd_ir_table_init(&t, 256, VTD_ECAP_EIM(u->ecap) != 0, &vtd_unit_ir_ops, u), OK);
    uint32_t idx;
    KT_EQ(vtd_ir_alloc(&t, &idx), OK);
    struct vtd_irte_spec s = { .dest = 0, .vector = 0x40, .src = vtd_ir_source_device(0, 4, 0) };
    s0 = stat(&u->stats.submissions);
    KT_EQ(vtd_ir_set(&t, idx, &s), OK);
    KT_EQ(vtd_ir_free(&t, idx), OK);
    KT_EQ(stat(&u->stats.submissions) - s0, 2);
    vtd_ir_table_destroy(&t);
    KT_EQ(stat(&u->stats.timeouts), t0);
    KT_EQ(stat(&u->stats.refused), r0);
}

KTEST(vtd_unit_refused_descriptor_reported)
{
    NEED_UNIT(u);
    uint64_t r0 = stat(&u->stats.refused), f0 = stat(&u->stats.fault_irqs);
    uint64_t t0 = stat(&u->stats.timeouts);
    /* A valid descriptor, one of type 0xf (Table 23: legacy mode takes
     * 1 to 5), another valid one: the batch fails, the rest still runs. */
    struct vtd_desc d[3] = {
        vtd_desc_cc(VTD_CC_GLOBAL, 0, 0, 0),
        { 0xf, 0 },
        vtd_desc_iotlb(VTD_IOTLB_GLOBAL, 0, 0, 0, 0, false),
    };
    KT_EQ(vtd_qi_submit(u, d, 3), ERR_IO);
    KT_EQ(stat(&u->stats.refused), r0 + 1);
    KT_ASSERT(u->last_iqei == 0 || u->last_iqei == 3 || u->last_iqei == 4);
    KT_EQ(vtd_rd32(u, VTD_FSTS) & VTD_FSTS_IQE, 0);
    /* The queue goes on. */
    KT_EQ(vtd_inv_iotlb_global(u), OK);
    /* A reserved bit in a valid type is refused too (6.5.2: RsvdZ). */
    struct vtd_desc bad = vtd_desc_cc(VTD_CC_GLOBAL, 0, 0, 0);
    bad.hi = 1;
    KT_EQ(vtd_qi_submit(u, &bad, 1), ERR_IO);
    KT_EQ(stat(&u->stats.refused), r0 + 2);
    KT_EQ(vtd_inv_context_global(u), OK);
    /* The error raised the fault event interrupt (7.3). */
    uint64_t deadline = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (stat(&u->stats.fault_irqs) == f0 && uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_ASSERT(stat(&u->stats.fault_irqs) > f0);
    KT_EQ(stat(&u->stats.timeouts), t0);
    KT_EQ(vtd_qi_submit(u, d, 0), ERR_INVALID_ARGS);
    KT_EQ(vtd_qi_submit(u, d, VTD_QI_BATCH_MAX + 1), ERR_INVALID_ARGS);
}

KTEST(vtd_unit_queue_wraps)
{
    NEED_UNIT(u);
    uint64_t w0 = stat(&u->stats.wraps), t0 = stat(&u->stats.timeouts);
    struct vtd_desc d[VTD_QI_BATCH_MAX];
    for (int i = 0; i < VTD_QI_BATCH_MAX; i++)
        d[i] = i & 1 ? vtd_desc_iec(false, (uint32_t)i, 0) : vtd_desc_iotlb(VTD_IOTLB_DOMAIN,
                                                                            2, 0, 0, 0, false);
    /* Batches of every size, 1 to the most, many times round. */
    uint64_t rng = 0x1a, written = 0;
    while (written < 12 * VTD_QI_ENTRIES) {
        uint32_t n = 1 + (uint32_t)(kt_rng(&rng) % VTD_QI_BATCH_MAX);
        KT_EQ(vtd_qi_submit(u, d, n), OK);
        written += n + 1;
    }
    KT_ASSERT(stat(&u->stats.wraps) - w0 >= 11);
    KT_EQ(stat(&u->stats.timeouts), t0);
    /* Software's tail and the unit's agree, and the queue is empty. */
    spin_lock(&u->qlock);
    uint32_t tail = u->tail;
    uint64_t iqt = vtd_rd64(u, VTD_IQT), iqh = vtd_rd64(u, VTD_IQH);
    spin_unlock(&u->qlock);
    KT_EQ(iqt >> VTD_IQ_SHIFT, tail);
    KT_EQ(iqh, iqt);
}

#define MANY_ROUNDS 300
static uint64_t many_failed;   /* atomic */

static void many_worker(void *arg)
{
    struct vtd_unit *u = arg;
    uint64_t rng = 1 + (uint64_t)kt_cur_cpu();
    for (int i = 0; i < MANY_ROUNDS; i++) {
        status_t st;
        switch (kt_rng(&rng) % 4) {
        case 0: st = vtd_inv_iotlb_pages(u, 4, (kt_rng(&rng) & 0xffff) << 12, 1 + i % 9, true); break;
        case 1: st = vtd_inv_iec_index(u, 1 + (uint32_t)(kt_rng(&rng) & 0xff), 1 + i % 5); break;
        case 2: st = vtd_inv_context_domain(u, 4); break;
        default: st = vtd_inv_iotlb_domain(u, 4); break;
        }
        if (st != OK)
            __atomic_add_fetch(&many_failed, 1, __ATOMIC_RELAXED);
    }
}

KTEST(vtd_unit_many_cpus_at_once)
{
    NEED_UNIT(u);
    uint64_t t0 = stat(&u->stats.timeouts), s0 = stat(&u->stats.submissions);
    __atomic_store_n(&many_failed, 0, __ATOMIC_RELAXED);
    static struct thread *t[MAX_CPUS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < cpu_count; c++) {
        if (!cpus[c] || !cpu_online(cpus[c]))
            continue;
        cpumask_t m;
        cpumask_one(&m, c);
        t[n++] = thread_create_on("vtd many", many_worker, u, PRIO_DEFAULT, &m);
    }
    for (uint32_t i = 0; i < n; i++)
        thread_join(t[i]);
    KT_EQ(__atomic_load_n(&many_failed, __ATOMIC_RELAXED), 0);
    KT_EQ(stat(&u->stats.timeouts), t0);
    KT_ASSERT(stat(&u->stats.submissions) - s0 >= (uint64_t)n * MANY_ROUNDS);
}

/* A storm of fault events (each refused descriptor raises one, 7.3: the
 * fastest real source QEMU has; a device's blocked DMA or interrupt
 * writes are the same interrupt) doesn't take the fault interrupt's CPU:
 * past VTD_STORM_IRQS in a window the interrupt is masked, the log thread
 * polls the unit instead, and unmasks it once the faults stop. Review
 * finding 2. */
#define STORM_EVENTS 1500

KTEST(vtd_unit_fault_storm_masked)
{
    NEED_UNIT(u);
    if (!__atomic_load_n(&u->fault_on, __ATOMIC_ACQUIRE)) {
        kprintf("ktest %s: no fault interrupt, skipped\n", ktest_current);
        return;
    }
    struct vtd_desc bad = { 0xf, 0 };   /* type 0xf: refused (Table 23) */
    uint64_t i0 = stat(&u->stats.fault_irqs), r0 = stat(&u->stats.refused);
    uint64_t t0 = uptime_ns();
    uint32_t io = 0;
    for (uint32_t i = 0; i < STORM_EVENTS; i++)
        io += vtd_qi_submit(u, &bad, 1) == ERR_IO;
    uint64_t ms = (uptime_ns() - t0) / NS_PER_MS;
    uint64_t irqs = stat(&u->stats.fault_irqs) - i0;
    /* Quiet again: unmasked within a few of the log thread's looks. */
    uint64_t end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while ((vtd_rd32(u, VTD_FECTL) & VTD_FECTL_IM) && uptime_ns() < end)
        thread_sleep_ms(2);
    bool unmasked = !(vtd_rd32(u, VTD_FECTL) & VTD_FECTL_IM);
    /* And a single event is an interrupt again. */
    uint64_t i1 = stat(&u->stats.fault_irqs);
    KT_EQ(vtd_qi_submit(u, &bad, 1), ERR_IO);
    end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (stat(&u->stats.fault_irqs) == i1 && uptime_ns() < end)
        thread_sleep_ms(1);
    bool heard = stat(&u->stats.fault_irqs) > i1;
    kprintf("ktest %s: %u refused descriptors in %lu ms: %lu fault interrupt(s); unmasked "
            "after: %s; the next event heard: %s\n", ktest_current, STORM_EVENTS, ms, irqs,
            unmasked ? "yes" : "NO", heard ? "yes" : "NO");
    KT_EQ(io, STORM_EVENTS);
    KT_EQ(stat(&u->stats.refused), r0 + STORM_EVENTS + 1);
    /* Without the guard nearly every event is an interrupt. With it, at
     * most VTD_STORM_IRQS per window, and a masked unit stays masked for at least the poll. */
    KT_ASSERT(irqs < STORM_EVENTS / 2);
    KT_ASSERT(unmasked);
    KT_ASSERT(heard);
}

KTEST(vtd_unit_queue_off_and_on)
{
    KT_SKIP_LIVE("turns the unit's queue off for a moment");
    NEED_UNIT(u);
    /* The takeover path: drained, QI off (the head goes back to 0), on
     * again from a zero tail (6.5.2). */
    KT_EQ(vtd_inv_iotlb_global(u), OK);
    KT_EQ(vtd_qi_disable(u), OK);
    KT_EQ(vtd_rd32(u, VTD_GSTS) & VTD_GSTS_QIES, 0);
    KT_EQ(vtd_rd64(u, VTD_IQH), 0);
    KT_EQ(vtd_inv_iotlb_global(u), ERR_BAD_STATE);
    KT_EQ(vtd_qi_enable(u), OK);
    KT_ASSERT(vtd_rd32(u, VTD_GSTS) & VTD_GSTS_QIES);
    KT_EQ(vtd_inv_iotlb_global(u), OK);
    KT_EQ(vtd_inv_context_global(u), OK);
    KT_EQ(vtd_rd64(u, VTD_IQT), 4ull << VTD_IQ_SHIFT);
}
