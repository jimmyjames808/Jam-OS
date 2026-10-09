/* Hostile sequences of pins, unpins and closes (kernel/object/dma_cap.c,
 * vmo.c's pin paths, kernel/dev/vtd_domain.c and vtd_pt.c underneath), in
 * the M12.1 check's style: what a driver can throw at them, checked
 * against a model after every step. QEMU only (edu); both modes: with the
 * IOMMU translating edu the domain's leaves are checked too.
 *
 *   dma_pin_fuzz
 *       Random pins through the system call layer (sys_vmo_pin_flags,
 *       sys_vmo_unpin, sys_vmo_decommit) on three VMOs, with a writable and
 *       a read-only handle each: good and bad offsets and lengths (past the
 *       end, unaligned, zero), unknown flags, read-only pins with either
 *       handle, pins overlapping others (one page pinned many times, and
 *       both ways), unpins of live, dead, made-up and other VMOs' ids, the
 *       cap closed now and then with pins held and a new one made. After
 *       every step: the call's result is what the model says (an error
 *       changes nothing), the cap's pin count is the model's, a pinned page
 *       can't be decommitted, and with the IOMMU every page of every VMO is
 *       mapped in the cap's domain exactly as often as live pins cover it,
 *       writable exactly when they are writable pins, and not at all
 *       otherwise. Fixed seeds and one from the clock (printed).
 *   dma_pin_races_close
 *       A driver thread pins and unpins a page over and over while its
 *       cap's last handle closes (a kill), at varied moments: every pin
 *       either fails or lands in the close (freed with the domain, or
 *       quarantined), none is left on the cap, the function is back home
 *       (its context entry no longer names the dead cap's domain) and every
 *       page and table comes back (the harness's leak check). */
#include <jam/aspace_vmo.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/vmo.h>
#include <jam/x86.h>

#include "../dev/vtd_domain.h"
#include "../dev/vtd_pt.h"

#define PG    PAGE_SIZE
#define NV    3     /* VMOs */
#define VPG   4     /* pages each */
#define MAXP  48    /* live pins at most (the model's table) */
#define STEPS 600   /* per seed */

/* ---- the model ------------------------------------------------------------------------- */

struct mpin {
    bool     live;
    unsigned v, first, n;   /* VMO, its pages [first, first + n) */
    bool     ro;
    uint64_t id;
};

struct fuzz {
    struct handle_table t;
    struct job         *job;
    struct pci_dev     *dev;
    handle_t            devh, jobh, cap;
    handle_t            vh[NV], vro[NV];
    struct vmo         *v[NV];   /* (references of ours, to look at the pages) */
    struct kobject     *capobj;  /* the current cap (a reference of ours) */
    bool                tr;      /* the cap has a domain */
    struct mpin         pins[MAXP];
    uint64_t            dead_ids[16];   /* ids of pins gone (unpinned or closed) */
    unsigned            ndead;
    uint64_t            rng;
    unsigned            ok, refused, closes;
};

static unsigned live_pins(const struct fuzz *f)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAXP; i++)
        n += f->pins[i].live;
    return n;
}

/* Live pins covering page p of VMO v, and how many of them are read only. */
static unsigned cover(const struct fuzz *f, unsigned v, unsigned p, unsigned *ro)
{
    unsigned n = 0;
    *ro = 0;
    for (unsigned i = 0; i < MAXP; i++) {
        const struct mpin *m = &f->pins[i];
        if (m->live && m->v == v && p >= m->first && p < m->first + m->n) {
            n++;
            *ro += m->ro;
        }
    }
    return n;
}

static void forget(struct fuzz *f, struct mpin *m)
{
    m->live = false;
    f->dead_ids[f->ndead++ % 16] = m->id;
}

/* ---- the cap --------------------------------------------------------------------------- */

static void new_cap(struct fuzz *f)
{
    KT_EQ(sys_dma_cap_create(&f->t, f->devh, f->jobh, &f->cap), OK);
    KT_EQ(handle_get(&f->t, f->cap, OBJ_DMA_CAP, 0, &f->capobj, NULL), OK);
    f->tr = dma_cap_translated(f->capobj);
    KT_EQ(sys_dma_cap_bus_master(&f->t, f->cap, 1), OK);
}

/* The cap's last handle closes with whatever is pinned (a driver killed):
 * every pin goes with it (freed with the domain, or quarantined and then
 * released here). */
static void close_cap(struct fuzz *f)
{
    kobject_unref(f->capobj);
    f->capobj = NULL;
    KT_EQ(handle_close(&f->t, f->cap), OK);
    dma_quarantine_flush(f->dev);
    for (unsigned i = 0; i < MAXP; i++)
        if (f->pins[i].live)
            forget(f, &f->pins[i]);
    f->closes++;
}

/* ---- the checks ------------------------------------------------------------------------ */

static void check(struct fuzz *f, unsigned step)
{
    unsigned n = live_pins(f);
    uint64_t on_cap = dma_cap_pin_count(f->capobj);
    if (on_cap != n)
        kprintf("ktest %s: step %u: %lu pin(s) on the cap, the model has %u\n", ktest_current,
                step, on_cap, n);
    KT_EQ(on_cap, n);
    if (!f->tr)
        return;
    struct vtd_dom *dom = dma_cap_from_kobject(f->capobj)->dom->dom;
    for (unsigned v = 0; v < NV; v++)
        for (unsigned p = 0; p < VPG; p++) {
            unsigned ro, want = cover(f, v, p, &ro);
            uint64_t pa = vmo_page_phys(f->v[v], (uint64_t)p * PG);
            if (!pa) {
                KT_EQ(want, 0);   /* a pinned page is committed */
                continue;
            }
            uint32_t pins = 0;
            bool w = false;
            mutex_lock(&dom->lock);
            status_t st = vtd_pt_lookup_perm(&dom->pt, pa, &pins, &w);
            mutex_unlock(&dom->lock);
            bool good = want ? st == OK && pins == want && w == (ro == 0) : st == ERR_NOT_FOUND;
            if (!good)
                kprintf("ktest %s: step %u: VMO %u page %u (%lx): %u pin(s) (%u read only) "
                        "in the model; the domain: %s, %u pin(s), %s\n", ktest_current, step,
                        v, p, pa, want, ro, status_str(st), pins, w ? "writable" : "read only");
            KT_ASSERT(good);
            KT_ASSERT(ro == 0 || ro == want);   /* the model never mixes (the kernel refuses) */
        }
}

/* ---- the steps ------------------------------------------------------------------------- */

static uint64_t pick(struct fuzz *f, uint64_t n)
{
    return kt_rng(&f->rng) % n;
}

static void step_pin(struct fuzz *f)
{
    unsigned v = (unsigned)pick(f, NV);
    bool ro_handle = pick(f, 4) == 0;
    uint32_t flags = pick(f, 3) == 0 ? VMO_PIN_READ_ONLY : 0;
    if (pick(f, 25) == 0)
        flags |= 1u << (1 + pick(f, 31));   /* an unknown flag */
    /* Mostly a range inside the VMO (so pins pile up and overlap), now and
     * then one that isn't. */
    uint64_t off = pick(f, VPG) * PG, len = (1 + pick(f, VPG - off / PG)) * PG;
    if (pick(f, 8) == 0)
        off = pick(f, VPG + 2) * PG, len = pick(f, VPG + 1) * PG;
    if (pick(f, 20) == 0)
        off += 1 + pick(f, PG - 1);   /* unaligned */
    if (pick(f, 20) == 0)
        len += 1 + pick(f, PG - 1);
    if (pick(f, 40) == 0)
        off = ~0ull - PG * pick(f, 3);   /* far past the end (and past 2^64 with len) */

    /* What the model expects: OK, or some error that changes nothing. */
    bool ro = flags & VMO_PIN_READ_ONLY;
    bool expect = !(flags & ~VMO_PIN_READ_ONLY) && len && !(off % PG) && !(len % PG) &&
                  off <= (uint64_t)VPG * PG && len <= (uint64_t)VPG * PG - off &&
                  (!ro_handle || (ro && f->tr));
    unsigned first = (unsigned)(off / PG), n = (unsigned)(len / PG);
    if (expect && f->tr)   /* one permission per page in the domain */
        for (unsigned p = first; p < first + n; p++) {
            unsigned cro, c = cover(f, v, p, &cro);
            if (c && (cro != 0) != ro)
                expect = false;
        }
    unsigned slot = MAXP;
    for (unsigned i = 0; i < MAXP && slot == MAXP; i++)
        if (!f->pins[i].live)
            slot = i;
    if (slot == MAXP)
        return;   /* the model is full: an unpin will come */

    uint64_t pa[VPG + 1], id = 0;
    status_t st = sys_vmo_pin_flags(&f->t, ro_handle ? f->vro[v] : f->vh[v], f->cap, off, len,
                                    flags, pa, &id);
    if ((st == OK) != expect)
        kprintf("ktest %s: pin of VMO %u [%lx, +%lx) flags %x with its %s handle: %s, the "
                "model expected %s\n", ktest_current, v, off, len, flags,
                ro_handle ? "read-only" : "writable", status_str(st), expect ? "OK" : "an error");
    KT_EQ(st == OK, expect);
    if (st != OK) {
        f->refused++;
        return;
    }
    f->ok++;
    for (unsigned i = 0; i < n; i++)   /* the addresses are the pages' own */
        KT_EQ(pa[i], vmo_page_phys(f->v[v], off + (uint64_t)i * PG));
    f->pins[slot] = (struct mpin){ true, v, first, n, ro, id };
}

static void step_unpin(struct fuzz *f)
{
    unsigned n = live_pins(f);
    unsigned v = (unsigned)pick(f, NV);
    uint64_t id;
    struct mpin *m = NULL;
    unsigned how = (unsigned)pick(f, 10);
    if (n && how < 6) {   /* a live one, through either handle */
        unsigned k = (unsigned)pick(f, n);
        for (unsigned i = 0; i < MAXP && !m; i++)
            if (f->pins[i].live && !k--)
                m = &f->pins[i];
        v = m->v;
        id = m->id;
    } else if (how < 8 && f->ndead) {   /* one gone already */
        id = f->dead_ids[pick(f, f->ndead < 16 ? f->ndead : 16)];
    } else {   /* made up, or another VMO's */
        id = how == 8 ? kt_rng(&f->rng) : pick(f, 64);
    }
    /* Is it a live pin of VMO v after all (made-up ids may hit one)? */
    for (unsigned i = 0; i < MAXP && !m; i++)
        if (f->pins[i].live && f->pins[i].v == v && f->pins[i].id == id)
            m = &f->pins[i];
    status_t st = sys_vmo_unpin(&f->t, pick(f, 2) ? f->vro[v] : f->vh[v], f->cap, id);
    if ((st == OK) != (m != NULL))
        kprintf("ktest %s: unpin of id %lu on VMO %u: %s, the model expected %s\n",
                ktest_current, id, v, status_str(st), m ? "OK" : "an error");
    KT_EQ(st == OK, m != NULL);
    if (m)
        forget(f, m);
}

static void step_decommit(struct fuzz *f)
{
    unsigned v = (unsigned)pick(f, NV), p = (unsigned)pick(f, VPG), ro;
    bool pinned = cover(f, v, p, &ro) != 0;
    status_t st = sys_vmo_decommit(&f->t, f->vh[v], (uint64_t)p * PG, PG);
    KT_EQ(st, pinned ? ERR_BAD_STATE : OK);
}

static void fuzz_run(uint64_t seed)
{
    static struct fuzz f;
    memset(&f, 0, sizeof(f));
    f.rng = seed;
    f.dev = kt_edu();
    f.job = kt_fresh_job();
    handle_table_init(&f.t);
    f.t.job = f.job;
    struct khandle kh = khandle_from_new(kt_pci_dev_res(f.dev), RES_RIGHTS);
    KT_EQ(handle_insert(&f.t, &kh, &f.devh), OK);
    f.jobh = kt_job_handle(&f.t, f.job);
    for (unsigned v = 0; v < NV; v++) {
        KT_EQ(sys_vmo_create(&f.t, VPG * PG, 0, &f.vh[v]), OK);
        KT_EQ(handle_duplicate(&f.t, f.vh[v], RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP, &f.vro[v]),
              OK);
        struct kobject *vo;
        KT_EQ(handle_get(&f.t, f.vh[v], OBJ_VMO, 0, &vo, NULL), OK);
        f.v[v] = vmo_from_kobject(vo);
    }
    new_cap(&f);
    for (unsigned s = 0; s < STEPS; s++) {
        unsigned what = (unsigned)pick(&f, 100);
        if (what < 55)
            step_pin(&f);
        else if (what < 82)
            step_unpin(&f);
        else if (what < 98)
            step_decommit(&f);
        else {
            close_cap(&f);
            new_cap(&f);
        }
        check(&f, s);
    }
    kprintf("ktest %s: seed %lx: %s; %u pin(s) made, %u refused, %u close(s) with pins held, "
            "%u live at the end\n", ktest_current, seed, f.tr ? "translated" : "not translated",
            f.ok, f.refused, f.closes, live_pins(&f));
    close_cap(&f);
    for (unsigned v = 0; v < NV; v++)
        kobject_unref(vmo_kobject(f.v[v]));
    handle_table_destroy(&f.t);
    dma_quarantine_flush(f.dev);
    kt_job_is_empty(f.job);
    job_unref(f.job);
}

KTEST(dma_pin_fuzz)
{
    if (!kt_edu())
        return;
    static const uint64_t seeds[] = { 0x5eed1, 0x5eed2, 0xd1ce, 0xfeed5 };
    for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        fuzz_run(seeds[i]);
    fuzz_run(rdtsc() | 1);   /* one the clock picks: its seed is printed */
}

/* ---- a pin racing the close --------------------------------------------------------------- */

static struct {
    struct kobject *cap;   /* the racer's reference */
    struct vmo     *v;
    volatile bool   stop;
    unsigned        made, failed, unpinned;
} race;

static void racer(void *arg)
{
    (void)arg;
    while (!race.stop) {
        uint64_t pa[2], id;
        status_t st = vmo_pin(race.v, race.cap, 0, 2 * PG, pa, 2, &id);
        if (st != OK) {
            race.failed++;
            thread_yield();
            continue;
        }
        race.made++;
        if (vmo_unpin(race.v, race.cap, id) == OK)   /* (fails once the close took it) */
            race.unpinned++;
    }
}

KTEST(dma_pin_races_close)
{
    struct pci_dev *d = kt_edu();
    if (!d)
        return;
    dma_quarantine_flush(d);
    struct dma_quarantine_stats q0, q;
    dma_quarantine_stats(d, &q0);
    struct vmo *v;
    KT_EQ(vmo_create(2 * PG, 0, &v), OK);
    KT_EQ(vmo_commit(v, 0, 2 * PG), OK);
    bool tr = false;
    unsigned rounds = 24, home = 0, made = 0, failed = 0;
    uint64_t rng = rdtsc() | 1;
    for (unsigned r = 0; r < rounds; r++) {
        struct kobject *cap;
        KT_EQ(dma_cap_create_for(d, NULL, &cap), OK);
        tr = dma_cap_translated(cap);
        struct vtd_fn *fn = tr ? vtd_fn_of(d) : NULL;
        uint16_t did = fn ? VTD_CTX_DID(vtd_fn_read(fn).hi) : 0;
        KT_EQ(dma_cap_bus_master(cap, true), OK);
        race = (typeof(race)){ .cap = cap, .v = v };
        kobject_ref(cap);   /* the racer's: the cap object outlives its handle */
        struct thread *th = thread_create("dma-racer", racer, NULL, PRIO_DEFAULT);
        /* Let it run a little, a different while each round, then close. */
        uint64_t spin = kt_rng(&rng) % 2000;
        for (uint64_t i = 0; i < spin; i++)
            cpu_relax();
        thread_sleep_ms((unsigned)(kt_rng(&rng) % 3));
        struct khandle kh = khandle_from_new(cap, DMA_CAP_RIGHTS);
        khandle_release(&kh);   /* its last handle: closed (a kill) */
        thread_sleep_ms(1);
        race.stop = true;
        thread_join(th);
        dma_quarantine_flush(d);
        KT_EQ(dma_cap_pin_count(cap), 0);
        if (fn)
            home += VTD_CTX_DID(vtd_fn_read(fn).hi) != did;
        made += race.made;
        failed += race.failed;
        kobject_unref(cap);
    }
    dma_quarantine_flush(d);
    dma_quarantine_stats(d, &q);
    kobject_unref(vmo_kobject(v));
    kprintf("ktest %s: %s: %u round(s), %u pin(s) made, %u refused; the function back home "
            "%u time(s)\n", ktest_current, tr ? "translated" : "not translated", rounds, made,
            failed, home);
    KT_EQ(q.pins, q0.pins);    /* every batch finished */
    KT_EQ(q.pages, q0.pages);
    if (tr)
        KT_EQ(home, rounds);   /* the dead cap's domain never stays */
}
