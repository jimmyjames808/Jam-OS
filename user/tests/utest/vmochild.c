/* utest: copy-on-write child VMOs (vmo_create_child, abi/syscalls.def 63)
 * from user space, through the system calls.
 *
 * t_vmo_child_shares: a child of a VMO this process wrote, both mapped
 * writable here: each side's writes, through its mapping and through
 * vmo_write, never show in the other; the child's charge to our job (its
 * shared pages and its tables at the call, nothing more for the writes);
 * a child of the child; the new handle's rights.
 *
 * t_vmo_child_refusals: the flags, the offset, the size, the rights, a
 * wrong handle type and a bad pointer (nothing made, nothing charged);
 * and in a job too small for it (child mode "cow-big"): a child of a
 * whole VMO refused up front, a smaller one made and every page of it
 * written through a mapping without a fault failing. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define PG    4096ull
#define CPAGES 32
#define SNAP  VMO_CHILD_SNAPSHOT
#define ALOW  VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE
#define CHILD_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_RESIZE)

static uint64_t pages_used(void)
{
    struct job_info ji;
    if (jam_job_get_info(own_job(), &ji) != OK)
        return UINT64_MAX;
    return ji.used[JOB_LIMIT_PAGES];
}

/* The word at the start of page i of VMO v. */
static uint64_t vword(handle_t v, uint64_t i)
{
    uint64_t w = ~0ull;
    (void)jam_vmo_read(v, i * PG, &w, sizeof(w));
    return w;
}

static uint64_t mword(uint64_t addr, uint64_t i)
{
    return *(volatile const uint64_t *)(uintptr_t)(addr + i * PG);
}

static void mwrite(uint64_t addr, uint64_t i, uint64_t w)
{
    *(volatile uint64_t *)(uintptr_t)(addr + i * PG) = w;
}

bool t_vmo_child_shares(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), p, c, g;
    CHECK_ST(jam_vmo_create(CPAGES * PG, 0, &p), OK);
    for (uint64_t i = 0; i < CPAGES; i++) {
        uint64_t w = 0xc0de0000 + i;
        CHECK_ST(jam_vmo_write(p, i * PG, &w, sizeof(w)), OK);
    }
    uint64_t before = pages_used();
    CHECK_ST(jam_vmo_create_child(p, SNAP, 0, CPAGES * PG, &c), OK);
    CHECK_EQ(pages_used() - before, CPAGES + 2);   /* its pages, a mid and a leaf table */
    struct handle_info hi;
    CHECK_ST(jam_handle_info(c, &hi), OK);
    CHECK_EQ(hi.type, OBJ_VMO);
    CHECK_EQ(hi.rights, CHILD_RIGHTS);

    uint64_t pa = 0, ca = 0;
    CHECK_ST(jam_vmar_map(vmar, p, 0, CPAGES * PG, VMAR_READ | VMAR_WRITE, &pa), OK);
    CHECK_ST(jam_vmar_map(vmar, c, 0, CPAGES * PG, VMAR_READ | VMAR_WRITE, &ca), OK);
    for (uint64_t i = 0; i < CPAGES; i++) {
        CHECK_EQ(mword(ca, i), 0xc0de0000 + i);
        CHECK_EQ(mword(pa, i), 0xc0de0000 + i);
    }
    uint64_t mapped = pages_used();   /* + the mappings' structs and page tables */
    /* Even pages written by the parent's side, odd by the child's: through
     * the mappings and with vmo_write. */
    for (uint64_t i = 0; i < CPAGES; i++) {
        uint64_t w = (i & 1 ? 0xc1000000 : 0xa1000000) + i;
        if (i % 4 < 2)
            mwrite(i & 1 ? ca : pa, i, w);
        else
            CHECK_ST(jam_vmo_write(i & 1 ? c : p, i * PG, &w, sizeof(w)), OK);
    }
    for (uint64_t i = 0; i < CPAGES; i++) {
        uint64_t old = 0xc0de0000 + i;
        CHECK_EQ(mword(pa, i), i & 1 ? old : 0xa1000000 + i);
        CHECK_EQ(mword(ca, i), i & 1 ? 0xc1000000 + i : old);
        CHECK_EQ(vword(p, i), mword(pa, i));
        CHECK_EQ(vword(c, i), mword(ca, i));
    }
    CHECK_EQ(pages_used(), mapped);   /* separations charge nothing */

    /* A child of the child: the child's bytes as they are now. */
    CHECK_ST(jam_vmo_create_child(c, ALOW, 2 * PG, 4 * PG, &g), OK);
    for (uint64_t i = 0; i < 4; i++)
        CHECK_EQ(vword(g, i), mword(ca, i + 2));
    mwrite(ca, 2, 7);
    CHECK_EQ(vword(g, 0), 0xc0de0002);
    CHECK_ST(jam_handle_close(g), OK);

    CHECK_ST(jam_vmar_unmap(vmar, ca, CPAGES * PG), OK);
    CHECK_ST(jam_vmar_unmap(vmar, pa, CPAGES * PG), OK);
    CHECK_ST(jam_handle_close(c), OK);
    CHECK_EQ(pages_used(), before);
    CHECK_ST(jam_handle_close(p), OK);
    return true;
}

bool t_vmo_child_refusals(void)
{
    handle_t p, x, ev, noread, nodup;
    CHECK_ST(jam_vmo_create(4 * PG, 0, &p), OK);
    CHECK_ST(jam_vmo_commit(p, 0, 4 * PG), OK);
    uint64_t before = pages_used();
    CHECK_ST(jam_vmo_create_child(p, 0, 0, PG, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_create_child(p, SNAP | ALOW, 0, PG, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_create_child(p, SNAP | (1u << 9), 0, PG, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_create_child(p, SNAP, 100, PG, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_create_child(p, SNAP, 0, VMO_MAX_SIZE + 1, &x), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_vmo_create_child(p, SNAP, VMO_MAX_SIZE + PG, PG, &x), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_vmo_create_child(p, SNAP, 0, PG, (handle_t *)8), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_duplicate(p, RIGHTS_BASIC | RIGHT_MAP, &noread), OK);
    CHECK_ST(jam_handle_duplicate(p, RIGHT_READ | RIGHT_MAP, &nodup), OK);
    CHECK_ST(jam_vmo_create_child(noread, SNAP, 0, PG, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_create_child(nodup, SNAP, 0, PG, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_vmo_create_child(ev, SNAP, 0, PG, &x), ERR_WRONG_TYPE);
    CHECK_ST(jam_vmo_create_child(HANDLE_INVALID, SNAP, 0, PG, &x), ERR_BAD_HANDLE);
    CHECK_EQ(pages_used(), before);   /* nothing made, nothing charged */
    CHECK_ST(jam_handle_close(ev), OK);
    CHECK_ST(jam_handle_close(noread), OK);
    CHECK_ST(jam_handle_close(nodup), OK);
    CHECK_ST(jam_handle_close(p), OK);

    struct process_info info;
    if (!run_child("cow-big", JOB_LIMIT_PAGES, 1024, &info))
        return false;
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    return true;
}

/* "utest cow-big", in a job with a page limit: a VMO of two thirds of what
 * the job has left, committed; a child of all of it is refused
 * (ERR_NO_MEMORY), a child of 16 of its pages is not, and every one of
 * those pages written through a mapping (a separation each) works. Exit 0
 * if so. */
int vmochild_child(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct job_info ji;
    if (jam_job_get_info(own_job(), &ji) != OK)
        return 1;
    uint64_t left = ji.limit[JOB_LIMIT_PAGES] - ji.used[JOB_LIMIT_PAGES];
    uint64_t n = left * 2 / 3;
    if (n < 64)
        return 2;   /* the limit doesn't leave room for the test */
    handle_t p, c, vmar = startup_handle(SR_SELF_VMAR);
    if (jam_vmo_create(n * PG, 0, &p) != OK || jam_vmo_commit(p, 0, n * PG) != OK)
        return 3;
    if (jam_vmo_create_child(p, SNAP, 0, n * PG, &c) != ERR_NO_MEMORY)
        return 4;
    if (jam_vmo_create_child(p, SNAP, PG, 16 * PG, &c) != OK)
        return 5;
    uint64_t a = 0;
    if (jam_vmar_map(vmar, c, 0, 16 * PG, VMAR_READ | VMAR_WRITE, &a) != OK)
        return 6;
    for (uint64_t i = 0; i < 16; i++)
        mwrite(a, i, i + 1);   /* a fault the job couldn't pay for would kill us */
    for (uint64_t i = 0; i < 16; i++)
        if (vword(p, i + 1) != 0 || vword(c, i) != i + 1)
            return 7;
    return 0;
}
