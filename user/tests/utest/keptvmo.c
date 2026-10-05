/* utest: kept VMOs (VMO_KEEP_PAGES) and kept mappings (VMAR_KEPT_ONLY)
 * from user space, through the system calls (abi/syscalls.def).
 *
 * t_kept_vmo_refusals: what the calls refuse (bad flags and combinations,
 * missing rights, wrong handle types, a bad pointer), the charge to the
 * creator's job, and a creation the job can't pay for (child mode
 * "kept-big", in a job with a page limit).
 *
 * t_kept_vmo_client_and_compositor: the case the flags exist for. This
 * process is the "compositor"; child mode "kept-client" is a client that
 * makes a kept VMO, stamps its pages, and sends it over a channel. We map
 * it kept and read it; then the client does what it can to take pages
 * away (decommit, shrink: refused) and grows it; we read our mapping
 * again (a page fault we couldn't serve would kill this whole process),
 * map the grown VMO again, and read on after the client has died. The
 * client's job stays charged for the pages until our mapping goes. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define PG     4096ull
#define KMAGIC 0x6b65707470616765ull   /* "keptpage" */
#define KPAGES 64                       /* the client's VMO, then twice that */
#define KWAIT  (10 * NS_PER_S)

/* What the client tells us, as one word after the VMO: each bit an
 * outcome it saw as it should be. */
#define KC_DECOMMIT (1u << 0)   /* vmo_decommit: ERR_BAD_STATE */
#define KC_SHRINK   (1u << 1)   /* vmo_set_size smaller: ERR_BAD_STATE */
#define KC_GROW     (1u << 2)   /* vmo_set_size to twice: OK, new pages stamped */
#define KC_ALL      (KC_DECOMMIT | KC_SHRINK | KC_GROW)

/* Page i (of KPAGES * 2) starts with KMAGIC ^ i. */
static status_t kstamp(handle_t v, uint64_t first, uint64_t end)
{
    status_t st = OK;
    for (uint64_t i = first; i < end && st == OK; i++) {
        uint64_t w = KMAGIC ^ i;
        st = jam_vmo_write(v, i * PG, &w, sizeof(w));
    }
    return st;
}

/* Pages [0, n) at addr carry their stamps. */
static bool kread(uint64_t addr, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint64_t w = *(volatile const uint64_t *)(uintptr_t)(addr + i * PG);
        if (w != (KMAGIC ^ i))
            FAIL("page %lu reads %lx", (unsigned long)i, (unsigned long)w);
    }
    return true;
}

/* One message on ch: a word and up to one handle. */
static status_t ksend(handle_t ch, uint32_t word, handle_t h)
{
    return jam_channel_write(ch, &word, sizeof(word), &h, h ? 1 : 0);
}

/* Wait for one message on ch: its word, and its handle (or 0). */
static status_t krecv(handle_t ch, uint32_t *word, handle_t *h)
{
    signals_t seen = 0;
    status_t st = jam_object_wait_one(ch, SIG_READABLE, now() + KWAIT, &seen);
    if (st != OK)
        return st;
    uint32_t nb = 0, nh = 0;
    *h = HANDLE_INVALID;
    struct channel_read_args r = {
        .h = ch, .bytes_cap = sizeof(*word), .bytes = (uint64_t)(uintptr_t)word,
        .actual_bytes = (uint64_t)(uintptr_t)&nb, .handles = (uint64_t)(uintptr_t)h,
        .handles_cap = 1, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    st = jam_channel_read(&r);
    return st == OK && nb != sizeof(*word) ? ERR_INVALID_ARGS : st;
}

/* job's JOB_LIMIT_PAGES use reaches `want` within a second (a dead
 * process's address space may still be going back on another CPU). */
static bool kjob_pages(handle_t job, uint64_t want)
{
    struct job_info ji;
    uint64_t end = now() + NS_PER_S;
    do {
        CHECK_ST(jam_job_get_info(job, &ji), OK);
        if (ji.used[JOB_LIMIT_PAGES] == want)
            return true;
        jam_nanosleep(now() + NS_PER_MS);
    } while (now() < end);
    FAIL("the job uses %lu pages, not %lu", (unsigned long)ji.used[JOB_LIMIT_PAGES],
         (unsigned long)want);
}

/* ---- the refusals ------------------------------------------------------------ */

static bool kept_create_refusals(void)
{
    handle_t v;
    struct job_info a, b;
    CHECK_ST(jam_vmo_create(PG, VMO_KEEP_PAGES | (1u << 7), HANDLE_INVALID, &v),
             ERR_INVALID_ARGS);
    /* The drivers' bits need a DMA capability first, kept or not. */
    CHECK_ST(jam_vmo_create(PG, VMO_KEEP_PAGES | 1u, HANDLE_INVALID, &v), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create(PG, VMO_KEEP_PAGES, HANDLE_INVALID, (handle_t *)8), ERR_INVALID_ARGS);

    /* All its pages are charged at once: 10 pages, a mid and a leaf table. */
    CHECK_ST(jam_job_get_info(own_job(), &a), OK);
    CHECK_ST(jam_vmo_create(10 * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_job_get_info(own_job(), &b), OK);
    CHECK_EQ(b.used[JOB_LIMIT_PAGES] - a.used[JOB_LIMIT_PAGES], 12);
    CHECK_ST(jam_vmo_decommit(v, 0, PG), ERR_BAD_STATE);
    CHECK_ST(jam_vmo_set_size(v, 9 * PG), ERR_BAD_STATE);
    CHECK_ST(jam_vmo_set_size(v, 12 * PG), OK);
    CHECK_ST(jam_job_get_info(own_job(), &b), OK);
    CHECK_EQ(b.used[JOB_LIMIT_PAGES] - a.used[JOB_LIMIT_PAGES], 14);
    uint64_t size = 0;
    CHECK_ST(jam_vmo_get_size(v, &size), OK);
    CHECK_EQ(size, 12 * PG);
    CHECK_ST(jam_handle_close(v), OK);
    CHECK_ST(jam_job_get_info(own_job(), &b), OK);
    CHECK_EQ(b.used[JOB_LIMIT_PAGES], a.used[JOB_LIMIT_PAGES]);

    /* Beyond the job's limit: refused, nothing kept (run_child checks). */
    struct process_info info;
    if (!run_child("kept-big", JOB_LIMIT_PAGES, 256, &info))
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    return true;
}

static bool kept_map_refusals(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), kv, pv, nomap, ev;
    uint64_t addr = 0;
    CHECK_ST(jam_vmo_create(2 * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &kv), OK);
    CHECK_ST(jam_vmo_create(2 * PG, 0, HANDLE_INVALID, &pv), OK);
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_handle_duplicate(kv, RIGHTS_BASIC | RIGHT_READ, &nomap), OK);
    const uint32_t r = VMAR_READ | VMAR_KEPT_ONLY;
    CHECK_ST(jam_vmar_map(vmar, pv, 0, PG, r, &addr), ERR_WRONG_TYPE);       /* not kept */
    CHECK_ST(jam_vmar_map(vmar, ev, 0, PG, r, &addr), ERR_WRONG_TYPE);       /* not a VMO */
    CHECK_ST(jam_vmar_map(vmar, nomap, 0, PG, r, &addr), ERR_ACCESS_DENIED); /* no RIGHT_MAP */
    CHECK_ST(jam_vmar_map(vmar, kv, 0, PG, r | VMAR_WRITE, &addr), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_map(vmar, kv, 0, PG, VMAR_KEPT_ONLY, &addr), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_map(vmar, kv, 0, PG, r | (1u << 6), &addr), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_map(vmar, kv, 0, 3 * PG, r, &addr), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_vmar_map(vmar, kv, 0, PG, r, (uint64_t *)8), ERR_INVALID_ARGS);
    /* Made: its permissions never change. */
    CHECK_ST(jam_vmar_map(vmar, kv, 0, 2 * PG, r, &addr), OK);
    CHECK_ST(jam_vmar_protect(vmar, addr, PG, VMAR_READ | VMAR_WRITE), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_protect(vmar, addr, 2 * PG, 0), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_protect(vmar, addr, 2 * PG, VMAR_READ), OK);
    CHECK_EQ(*(volatile const uint64_t *)(uintptr_t)addr, 0);
    CHECK_ST(jam_vmar_unmap(vmar, addr, 2 * PG), OK);
    CHECK_ST(jam_handle_close(nomap), OK);
    CHECK_ST(jam_handle_close(ev), OK);
    CHECK_ST(jam_handle_close(pv), OK);
    CHECK_ST(jam_handle_close(kv), OK);
    return true;
}

bool t_kept_vmo_refusals(void)
{
    return kept_create_refusals() && kept_map_refusals();
}

/* ---- client and compositor ------------------------------------------------------ */

/* "utest kept-big": in a job allowed 256 pages, a kept VMO of 512 pages is
 * refused and one of 4 is not. Exit 0 if so. */
static int kept_big(void)
{
    handle_t v;
    if (jam_vmo_create(512 * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &v) != ERR_NO_MEMORY)
        return 1;
    if (jam_vmo_create(4 * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &v) != OK)
        return 2;
    jam_handle_close(v);
    return 0;
}

/* "utest kept-client": SR_USER is our end of the channel. Exit 0 when
 * every step went as it should (the outcome word says which did). */
static int kept_client(void)
{
    handle_t ch = startup_handle(SR_USER), v, mine;
    uint32_t word;
    handle_t none;
    if (jam_vmo_create(KPAGES * PG, VMO_KEEP_PAGES, HANDLE_INVALID, &v) != OK ||
        kstamp(v, 0, KPAGES) != OK || jam_handle_duplicate(v, RIGHT_SAME, &mine) != OK)
        return 1;
    if (ksend(ch, 0, v) != OK || krecv(ch, &word, &none) != OK)   /* until it is mapped */
        return 2;
    uint32_t got = 0;
    if (jam_vmo_decommit(mine, 0, KPAGES * PG) == ERR_BAD_STATE)
        got |= KC_DECOMMIT;
    if (jam_vmo_set_size(mine, PG) == ERR_BAD_STATE)
        got |= KC_SHRINK;
    if (jam_vmo_set_size(mine, 2 * KPAGES * PG) == OK && kstamp(mine, KPAGES, 2 * KPAGES) == OK)
        got |= KC_GROW;
    if (ksend(ch, got, HANDLE_INVALID) != OK)
        return 3;
    return got == KC_ALL ? 0 : 4;   /* and our handle goes with us */
}

int kept_child(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "kept-big"))
        return kept_big();
    return kept_client();
}

bool t_kept_vmo_client_and_compositor(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), job, proc, ours, theirs, v;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_channel_create(&ours, &theirs), OK);
    CHECK_ST(child("kept-client", NULL, job, theirs, &proc), OK);
    uint32_t word = 0;
    CHECK_ST(krecv(ours, &word, &v), OK);
    CHECK(v != HANDLE_INVALID);

    uint64_t a = 0, b = 0;
    CHECK_ST(jam_vmar_map(vmar, v, 0, KPAGES * PG, VMAR_READ | VMAR_KEPT_ONLY, &a), OK);
    if (!kread(a, KPAGES))
        return false;
    CHECK_ST(ksend(ours, 1, HANDLE_INVALID), OK);   /* mapped: now do your worst */
    handle_t none;
    CHECK_ST(krecv(ours, &word, &none), OK);
    CHECK_EQ(word, KC_ALL);
    if (!kread(a, KPAGES))   /* still every page, no fault */
        return false;

    /* The resize: map the grown VMO, drop the old mapping. */
    CHECK_ST(jam_vmar_map(vmar, v, 0, 2 * KPAGES * PG, VMAR_READ | VMAR_KEPT_ONLY, &b), OK);
    CHECK_ST(jam_vmar_unmap(vmar, a, KPAGES * PG), OK);
    CHECK_ST(jam_handle_close(v), OK);   /* the mapping keeps the VMO */

    struct process_info info;
    CHECK_ST(spawn_wait(proc, KWAIT, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    if (!kread(b, 2 * KPAGES))   /* the client is gone; its pages stay */
        return false;
    if (!kjob_pages(job, 2 * KPAGES + 2))   /* its pages and tables, still */
        return false;
    CHECK_ST(jam_vmar_unmap(vmar, b, 2 * KPAGES * PG), OK);
    if (!kjob_pages(job, 0))
        return false;
    struct job_info ji;
    CHECK_ST(jam_job_get_info(job, &ji), OK);
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        if (ji.used[k])
            FAIL("the client's job keeps %lu units of kind %u", (unsigned long)ji.used[k], k);
    CHECK_ST(jam_handle_close(ours), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}
