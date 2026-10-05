/* Tests for VT-d DMA remapping: domains, context entries and the boot
 * (kernel/dev/vtd_domain.c, vtd_boot.c).
 *
 * Pure ones run everywhere: the root and context entries' exact bits
 * against VT-d 4.1's 9.1 and 9.3 written out as literals, pass-through's
 * address width from CAP.SAGAW, the early RMRR reservation on a made-up
 * memory map, the domain-id allocator.
 *
 * The rest need translation on (QEMU's intel-iommu and the boot word
 * iommu=on: tools/vtd-test.sh runs them with caching mode on and off) and
 * skip themselves without it; the ones that drive a device need QEMU's edu
 * (its DMA engine copies between its buffer and any bus address):
 *   - every function has a present context entry, never domain id 0, and
 *     a function nobody drives sits in its home domain;
 *   - edu in the blocking domain: its write to a page doesn't land, and a
 *     fault naming edu is seen;
 *   - edu in the pass-through domain (a driver's, until dma_cap makes one
 *     per cap): its writes land;
 *   - edu in a domain of its own (iommu_domain_create, attach): a mapped
 *     page is reached, an unmapped one is not (and faults), an unmapped
 *     page stops being reached, the entry carries the domain's id and
 *     table, detach puts edu back home;
 *   - edu muted after VTD_FAULT_LOGGED faults: FPD in its entry, no more
 *     faults recorded; a domain attached after that starts its count at 0
 *     and isn't muted at its first fault;
 *   - the handover with translation on (what a boot finds after the
 *     firmware's DMA protection or a kexec that couldn't turn it off): the
 *     unit is pointed at a copy of the tables while it translates, and
 *     devices keep working through it. */
#include <jam/boot.h>
#include <jam/iommu.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/vtd.h>

#include "../dev/vtd_domain.h"
#include "../dev/vtd_internal.h"

/* ---- pure ------------------------------------------------------------------------------- */

KTEST(vtd_domain_entry_bits_literal)
{
    /* 9.1 root entry: P bit 0, CTP 63:12, 127:64 reserved. */
    struct vtd_ctx r = vtd_root_entry(0x12345000);
    KT_EQ(r.lo, 0x12345001);
    KT_EQ(r.hi, 0);
    KT_EQ(vtd_root_entry(0x12345fff).lo, 0x12345001);   /* 11:1 reserved: never set */
    /* 9.3 context entry, second stage: P 0, FPD 1, TT 3:2 = 00, SSPTPTR
     * 63:12; AW 66:64, DID 87:72. */
    struct vtd_ctx c = vtd_ctx_entry(0x1234, false, 0x7bf00000, 2, false);
    KT_EQ(c.lo, 0x7bf00001);
    KT_EQ(c.hi, 0x123402);
    c = vtd_ctx_entry(0xffff, false, 0x000ffffffffff000ull, 1, true);
    KT_EQ(c.lo, 0x000ffffffffff003ull);
    KT_EQ(c.hi, 0xffff01);
    /* Pass-through: TT = 10b, SSPTPTR ignored (written 0). */
    c = vtd_ctx_entry(5, true, 0xdead000, 3, false);
    KT_EQ(c.lo, 0x9);
    KT_EQ(c.hi, 0x503);
    c = vtd_ctx_entry(1, true, 0, 2, true);
    KT_EQ(c.lo, 0xb);
    KT_EQ(c.hi, 0x102);
    KT_EQ(VTD_CTX_DID(0x123402), 0x1234);
    KT_EQ(VTD_CTX_AW(0x123402), 2);
    /* RTADDR's TTM, 11:10 (11.4.5). */
    KT_EQ(VTD_RTADDR_TTM(0x7bf00c00ull), 3);
    KT_EQ(VTD_RTADDR_TTM(0x7bf00400ull), 1);
    KT_EQ(VTD_RTADDR_TTM(0x7bf00000ull), 0);
}

KTEST(vtd_domain_pass_aw)
{
    /* 9.3: pass-through's AW is the largest SAGAW (CAP 12:8) offers. */
    KT_EQ(vtd_pass_aw(0xd2008c222f0686ull), 2);   /* QEMU: 3 and 4 levels */
    KT_EQ(vtd_pass_aw(0xd2008c40660462ull), 2);   /* Alder Lake: 4 levels */
    KT_EQ(vtd_pass_aw(1ull << 9), 1);
    KT_EQ(vtd_pass_aw(0xeull << 8), 3);
    KT_EQ(vtd_pass_aw(0), 0);
    KT_EQ(vtd_pass_aw(1ull << 8), 0);   /* bit 0 is reserved */
}

KTEST(vtd_domain_rmrr_carve)
{
    struct boot_mem_region map[8] = {
        { 0x00100000, 0x07f00000, BOOT_MEM_USABLE },          /* to 0x8000000 */
        { 0x08000000, 0x01000000, BOOT_MEM_RESERVED },
        { 0x09000000, 0x07000000, BOOT_MEM_USABLE },          /* to 0x10000000 */
        { 0x10000000, 0x00100000, BOOT_MEM_LOADER_RECLAIMABLE },
        { 0x10100000, 0x00100000, BOOT_MEM_ACPI_NVS },
    };
    size_t n = 5;
    uint64_t bytes;
    /* Across a reserved hole: the usable tail and head go, the hole stays. */
    KT_EQ(vtd_rmrr_carve(map, &n, 8, 0x07ff0000, 0x0900ffff, &bytes), OK);
    KT_EQ(bytes, 0x20000);
    KT_EQ(n, 7);
    KT_EQ(map[0].base + map[0].length, 0x07ff0000);
    KT_EQ(map[1].base, 0x07ff0000);
    KT_EQ(map[1].length, 0x10000);
    KT_EQ(map[1].type, BOOT_MEM_RESERVED);
    KT_EQ(map[3].base, 0x09000000);
    KT_EQ(map[3].type, BOOT_MEM_RESERVED);
    KT_EQ(map[4].base, 0x09010000);
    KT_EQ(map[4].type, BOOT_MEM_USABLE);
    /* Loader-reclaimable is RAM the allocator gets later: taken too (a
     * part not page-aligned is widened to whole pages). */
    KT_EQ(vtd_rmrr_carve(map, &n, 8, 0x100ff800, 0x10100fff, &bytes), OK);
    KT_EQ(bytes, 0x1000);
    /* ACPI NVS, reserved: not the allocator's, left as they are. */
    size_t before = n;
    KT_EQ(vtd_rmrr_carve(map, &n, 8, 0x10101000, 0x10101fff, &bytes), OK);
    KT_EQ(bytes, 0);
    KT_EQ(n, before);
    /* Nonsense and a full map. */
    KT_EQ(vtd_rmrr_carve(map, &n, 8, 0x2000, 0x1fff, &bytes), ERR_INVALID_ARGS);
    KT_EQ(vtd_rmrr_carve(map, &n, n, 0x00200000, 0x00200fff, &bytes), ERR_NO_RESOURCES);
}

KTEST(vtd_domain_did_alloc)
{
    /* A unit with 16 ids (CAP.ND 0): 1 to 15 are handed out, never 0. */
    uint64_t used[1] = { 1 };
    struct vtd_ctl ctl = { .did_used = used, .ndid = 16 };
    for (uint16_t want = 1; want < 16; want++)
        KT_EQ(vtd_did_alloc(&ctl), want);
    KT_EQ(vtd_did_alloc(&ctl), 0);   /* none left */
    KT_EQ(vtd_did_free(&ctl, 7), OK);   /* not live: no invalidation */
    KT_EQ(vtd_did_alloc(&ctl), 7);
    KT_EQ(vtd_did_alloc(&ctl), 0);
}

/* ---- live: translation on --------------------------------------------------------------- */

/* The first unit's tables, live, or skip the test (return from it). */
#define NEED_LIVE(ctl)                                                           \
    struct vtd_ctl *ctl = vtd_ctl_get(0);                                        \
    do {                                                                         \
        if (!ctl || !__atomic_load_n(&ctl->live, __ATOMIC_ACQUIRE)) {           \
            kprintf("ktest %s: VT-d translation is off (iommu=on), skipped\n",  \
                    ktest_current);                                              \
            return;                                                              \
        }                                                                        \
    } while (0)

KTEST(vtd_domain_translation_on)
{
    NEED_LIVE(ctl);
    struct vtd_unit *u = ctl->unit;
    KT_ASSERT(vtd_rd32(u, VTD_GSTS) & VTD_GSTS_TES);
    KT_EQ(vtd_rd64(u, VTD_RTADDR) & ~0xfffull, ctl->root_phys);
    KT_ASSERT(iommu_translating());
    KT_EQ(ctl->blocking->pt.mapped, 0);   /* the blocking domain maps nothing */
    uint32_t covered = 0;
    mutex_lock(&ctl->lock);
    for (uint32_t i = 0; vtd_fn_at(i); i++) {
        struct vtd_fn *f = vtd_fn_at(i);
        if (f->ctl != ctl)
            continue;
        covered++;
        struct vtd_ctx e = vtd_fn_read(f);
        KT_ASSERT(e.lo & VTD_CTX_P);
        KT_ASSERT(VTD_CTX_DID(e.hi) != 0);
        KT_EQ(VTD_CTX_DID(e.hi), f->cur->ud.did);
        KT_ASSERT(ctl->root[f->sid >> 8].lo & VTD_ROOT_P);
        if (!pci_in_use(f->dev) && !f->dev->driver_managed && f->cur != ctl->pass)
            KT_ASSERT(f->cur == f->home);
    }
    mutex_unlock(&ctl->lock);
    KT_EQ(covered, ctl->nfn);
    KT_ASSERT(covered > 0);
}

/* ---- live: edu ---------------------------------------------------------------------------- */

#define LEN 64u   /* bytes per transfer */

/* edu, its registers mapped, bus mastering on, and its function's state
 * (f->cur saved in *saved, the mute and counts reset); NULL to skip. */
struct edu {
    struct pci_dev  *d;
    volatile uint8_t *r;
    struct vtd_fn   *f;
    struct vtd_dom  *saved;   /* its domain before the test */
};

static bool edu_open(struct edu *e)
{
    e->d = kt_edu();
    if (!e->d)
        return false;
    e->f = vtd_fn_of(e->d);
    if (!e->f) {
        kprintf("ktest %s: edu is not under translation, skipped\n", ktest_current);
        return false;
    }
    e->r = kt_edu_regs(e->d);
    uint64_t cf = pci_cmd_lock();
    status_t st = pci_set_bus_master(e->d, true);
    pci_cmd_unlock(cf);
    mutex_lock(&e->f->ctl->lock);
    e->saved = e->f->cur;
    e->f->muted = false;
    e->f->dma_faults = 0;
    mutex_unlock(&e->f->ctl->lock);
    return st == OK;
}

static void edu_close(struct edu *e)
{
    uint64_t cf = pci_cmd_lock();
    (void)pci_set_bus_master(e->d, false);
    pci_cmd_unlock(cf);
    mutex_lock(&e->f->ctl->lock);
    e->f->muted = false;
    e->f->dma_faults = 0;
    (void)vtd_fn_switch_locked(e->f, e->saved);
    mutex_unlock(&e->f->ctl->lock);
}

/* One transfer of LEN bytes, waited for: RAM -> edu's buffer, or back. */
static bool edu_in(struct edu *e, uint64_t src)
{
    return kt_edu_dma(e->r, src, 0, LEN, false);
}

static bool edu_out(struct edu *e, uint64_t dst)
{
    return kt_edu_dma(e->r, 0, dst, LEN, true);
}

/* Wait (bounded) until edu's DMA faults reach n: the fault interrupt, then
 * the log thread, count them. */
static bool faults_reach(const struct edu *e, uint32_t n)
{
    uint64_t end = uptime_ns() + kt_patience_ms(1000) * NS_PER_MS;
    while (__atomic_load_n(&e->f->dma_faults, __ATOMIC_RELAXED) < n) {
        if (uptime_ns() > end)
            return false;
        thread_sleep_ms(2);
    }
    return true;
}

/* A page below 4 GiB (edu's DMA mask) filled with `byte`. */
static uint64_t page_of(uint8_t byte)
{
    uint64_t pa = pmm_alloc_page_phys(PMM_DMA32);
    if (pa)
        memset(phys_to_virt(pa), byte, PAGE_SIZE);
    return pa;
}

static bool page_is(uint64_t pa, uint8_t byte)
{
    const uint8_t *p = phys_to_virt(pa);
    for (uint32_t i = 0; i < LEN; i++)
        if (p[i] != byte)
            return false;
    return true;
}

static status_t to_home(struct edu *e)
{
    return vtd_fn_switch(e->f, e->f->home);
}

KTEST(vtd_domain_blocked_dma_faults)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    uint64_t pa = page_of(0x5a);
    KT_ASSERT(pa);
    KT_EQ(to_home(&e), OK);
    KT_ASSERT(e.f->home == ctl->blocking);   /* QEMU has no RMRR */
    KT_ASSERT(edu_out(&e, pa));
    bool kept = page_is(pa, 0x5a);
    bool seen = faults_reach(&e, 1);
    uint32_t sid = e.f->sid;
    edu_close(&e);
    pmm_free_page_phys(pa);
    KT_ASSERT(kept);   /* blocked: the page never changed */
    KT_ASSERT(seen);   /* and a fault named edu */
    KT_EQ(sid, (uint16_t)(e.d->info.bus << 8 | e.d->info.dev << 3 | e.d->info.fn));
}

KTEST(vtd_domain_pass_dma_lands)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    uint64_t a = page_of(0xa5), b = page_of(0);
    KT_ASSERT(a && b);
    KT_EQ(iommu_device_driven(e.d), OK);
    KT_ASSERT(e.f->cur == ctl->pass);
    bool ok = edu_in(&e, a) && edu_out(&e, b);
    bool landed = page_is(b, 0xa5);
    uint32_t faults = e.f->dma_faults;
    edu_close(&e);
    pmm_free_page_phys(a);
    pmm_free_page_phys(b);
    KT_ASSERT(ok);
    KT_ASSERT(landed);
    KT_EQ(faults, 0);
}

KTEST(vtd_domain_own_domain)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    uint64_t a = page_of(0xc3), b = page_of(0), c = page_of(0x11);
    KT_ASSERT(a && b && c);
    struct iommu_domain *dom;
    KT_EQ(iommu_domain_create(e.d, NULL, &dom), OK);
    uint64_t two[2] = { a, b };
    KT_EQ(iommu_map(dom, two, 2), OK);
    uint64_t odd = a + 8;
    KT_EQ(iommu_map(dom, &odd, 1), ERR_INVALID_ARGS);
    KT_EQ(iommu_unmap(dom, &c, 1), ERR_NOT_FOUND);
    KT_EQ(iommu_attach(dom), OK);
    KT_EQ(iommu_domain_destroy(dom), ERR_BAD_STATE);   /* attached */
    /* The entry names the domain: its id and its table, in one write. */
    struct vtd_ctx ent = vtd_fn_read(e.f);
    KT_EQ(VTD_CTX_DID(ent.hi), e.f->cur->ud.did);
    KT_EQ(ent.lo & VTD_CTX_SSPTPTR, vtd_pt_root(&e.f->cur->pt));
    KT_EQ((ent.lo >> VTD_CTX_TT_SHIFT) & 3, VTD_CTX_TT_SS);
    /* Mapped: a -> edu -> b lands. Not mapped: c stays. */
    bool ok = edu_in(&e, a) && edu_out(&e, b);
    bool landed = page_is(b, 0xc3);
    ok = ok && edu_out(&e, c);
    bool c_kept = page_is(c, 0x11), c_fault = faults_reach(&e, 1);
    /* Unmapped: b is no longer reached. */
    KT_EQ(iommu_unmap(dom, &b, 1), OK);
    memset(phys_to_virt(b), 0x22, PAGE_SIZE);
    ok = ok && edu_out(&e, b);
    bool b_kept = page_is(b, 0x22), b_fault = faults_reach(&e, 2);
    KT_EQ(iommu_detach(dom), OK);
    KT_ASSERT(e.f->cur == e.f->home);
    KT_EQ(iommu_detach(dom), ERR_BAD_STATE);
    KT_EQ(iommu_unmap(dom, &a, 1), OK);
    KT_EQ(iommu_domain_destroy(dom), OK);
    edu_close(&e);
    pmm_free_page_phys(a);
    pmm_free_page_phys(b);
    pmm_free_page_phys(c);
    KT_ASSERT(ok);
    KT_ASSERT(landed);
    KT_ASSERT(c_kept && c_fault);
    KT_ASSERT(b_kept && b_fault);
}

KTEST(vtd_domain_mute_after_faults)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    uint64_t pa = page_of(0x77);
    KT_ASSERT(pa);
    KT_EQ(to_home(&e), OK);
    bool ok = true, counted = true;
    for (uint32_t i = 1; i <= VTD_FAULT_LOGGED && ok && counted; i++) {
        ok = edu_out(&e, pa);
        counted = faults_reach(&e, i);
    }
    mutex_lock(&ctl->lock);   /* the log thread mutes under it, after counting */
    bool fpd = vtd_fn_read(e.f).lo & VTD_CTX_FPD;
    bool muted = e.f->muted;
    mutex_unlock(&ctl->lock);
    /* Muted: the next blocked write records nothing (and still doesn't land). */
    uint64_t before = __atomic_load_n(&ctl->unit->stats.faults, __ATOMIC_RELAXED);
    ok = ok && edu_out(&e, pa);
    thread_sleep_ms(50);
    uint64_t after = __atomic_load_n(&ctl->unit->stats.faults, __ATOMIC_RELAXED);
    bool kept = page_is(pa, 0x77);
    edu_close(&e);   /* unmuted */
    bool unmuted = !(vtd_fn_read(e.f).lo & VTD_CTX_FPD);
    pmm_free_page_phys(pa);
    KT_ASSERT(ok);
    KT_ASSERT(counted);
    KT_ASSERT(fpd && muted);
    KT_IDLE_EQ(after, before);   /* another device may fault meanwhile */
    KT_ASSERT(kept);
    KT_ASSERT(unmuted);
}

/* A driver attached after its function was muted starts with a count of
 * its own: muted at home after VTD_FAULT_LOGGED faults, then attached to a
 * domain of its own, edu has 0 faults and FPD clear, and its next blocked
 * write is recorded and counted 1 without muting it again (with the old
 * count it would have been muted at its first fault). */
KTEST(vtd_domain_new_driver_fresh_count)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    uint64_t pa = page_of(0x66);
    KT_ASSERT(pa);
    KT_EQ(to_home(&e), OK);
    bool ok = true, counted = true;
    for (uint32_t i = 1; i <= VTD_FAULT_LOGGED && ok && counted; i++) {
        ok = edu_out(&e, pa);
        counted = faults_reach(&e, i);
    }
    mutex_lock(&ctl->lock);
    bool muted = e.f->muted;
    mutex_unlock(&ctl->lock);
    struct iommu_domain *dom = NULL;
    status_t made = iommu_domain_create(e.d, NULL, &dom);
    status_t attached = made == OK ? iommu_attach(dom) : made;
    mutex_lock(&ctl->lock);
    uint32_t after_attach = e.f->dma_faults;
    bool fpd_after_attach = vtd_fn_read(e.f).lo & VTD_CTX_FPD;
    mutex_unlock(&ctl->lock);
    /* pa isn't mapped in the new domain: blocked, and recorded this time. */
    ok = ok && attached == OK && edu_out(&e, pa);
    bool seen = attached == OK && faults_reach(&e, 1);
    thread_sleep_ms(20);   /* the log thread has decided on the mute by now */
    mutex_lock(&ctl->lock);
    uint32_t faults = e.f->dma_faults;
    bool muted_again = e.f->muted || (vtd_fn_read(e.f).lo & VTD_CTX_FPD);
    mutex_unlock(&ctl->lock);
    bool kept = page_is(pa, 0x66);
    if (attached == OK)
        (void)iommu_detach(dom);
    if (made == OK)
        (void)iommu_domain_destroy(dom);
    edu_close(&e);
    pmm_free_page_phys(pa);
    KT_ASSERT(ok);
    KT_ASSERT(counted && muted);
    KT_EQ(made, OK);
    KT_EQ(attached, OK);
    KT_EQ(after_attach, 0);
    KT_ASSERT(!fpd_after_attach);
    KT_ASSERT(seen);
    KT_EQ(faults, 1);
    KT_ASSERT(!muted_again);
    KT_ASSERT(kept);
}

/* A copy of ctl's root and context tables in fresh pages (the same
 * entries): its root's physical address, or 0. */
static uint64_t copy_tables(struct vtd_ctl *ctl, uint64_t pages[257], uint32_t *n)
{
    *n = 0;
    uint64_t root = pmm_alloc_page_phys(PMM_ZERO);
    if (!root)
        return 0;
    pages[(*n)++] = root;
    struct vtd_ctx *r = phys_to_virt(root);
    for (uint32_t bus = 0; bus < 256; bus++) {
        if (!ctl->ctx[bus])
            continue;
        uint64_t t = pmm_alloc_page_phys(0);
        if (!t)
            return 0;
        pages[(*n)++] = t;
        memcpy(phys_to_virt(t), ctl->ctx[bus], PAGE_SIZE);
        r[bus] = vtd_root_entry(t);
    }
    for (uint32_t i = 0; i < *n; i++)
        vtd_flush_lines(phys_to_virt(pages[i]), PAGE_SIZE);
    return root;
}

KTEST(vtd_domain_handover_while_on)
{
    NEED_LIVE(ctl);
    struct edu e;
    if (!edu_open(&e))
        return;
    struct vtd_unit *u = ctl->unit;
    uint64_t a = page_of(0x3c), b = page_of(0), c = page_of(0x44);
    static uint64_t pages[257];
    uint32_t n;
    KT_ASSERT(a && b && c);
    KT_EQ(iommu_device_driven(e.d), OK);
    /* Nothing may change the tables while the copy is in use. */
    mutex_lock(&ctl->lock);
    uint64_t copy = copy_tables(ctl, pages, &n);
    status_t st = copy ? vtd_boot_handover(u, copy) : ERR_NO_MEMORY;
    bool on = vtd_rd32(u, VTD_GSTS) & VTD_GSTS_TES;
    uint64_t rt = vtd_rd64(u, VTD_RTADDR) & ~0xfffull;
    /* Through the copy: edu (pass-through) reaches RAM. */
    bool ok = st == OK && edu_in(&e, a) && edu_out(&e, b);
    bool landed = page_is(b, 0x3c);
    status_t back = vtd_boot_handover(u, ctl->root_phys);
    mutex_unlock(&ctl->lock);
    /* And back on the real tables: blocked again once home. */
    KT_EQ(to_home(&e), OK);
    ok = ok && edu_out(&e, c);
    bool kept = page_is(c, 0x44);
    edu_close(&e);
    for (uint32_t i = 0; i < n; i++)
        pmm_free_page_phys(pages[i]);
    pmm_free_page_phys(a);
    pmm_free_page_phys(b);
    pmm_free_page_phys(c);
    KT_EQ(st, OK);
    KT_ASSERT(on);
    KT_EQ(rt, copy);
    KT_EQ(back, OK);
    KT_EQ(vtd_rd64(u, VTD_RTADDR) & ~0xfffull, ctl->root_phys);
    KT_ASSERT(vtd_rd32(u, VTD_GSTS) & VTD_GSTS_TES);
    KT_ASSERT(ok);
    KT_ASSERT(landed);
    KT_ASSERT(kept);
}
