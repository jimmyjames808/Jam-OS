/* utest: libos's heap (user/lib/heap.c) in a long-running program.
 *
 * A program that asks for the same big buffer again and again, with small
 * allocations made in between that outlive it (the shell: `ls` takes
 * 68 KiB each time, history and variables stay), must get the old space
 * back instead of growing: the shell's heap grew by the buffer's size on
 * every command until its 16 MiB ran out. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define ROUNDS 400           /* 400 x 64 KiB is more than the whole heap */
#define BIG    (64u << 10)
#define SMALL  48u

bool t_heap_reuses_freed_space(void)
{
    static void *keep[ROUNDS];
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    for (unsigned i = 0; i < ROUNDS; i++) {
        uint8_t *big = malloc(BIG);
        if (!big)
            printf("utest: %s: no %u KiB at round %u\n", utest_cur, BIG >> 10, i);
        CHECK(big != NULL);
        big[0] = big[BIG - 1] = (uint8_t)i;   /* its first and last pages are used */
        free(big);
        keep[i] = malloc(SMALL);              /* outlives the big one */
        CHECK(keep[i] != NULL);
    }
    CHECK_ST(info_of(own_job(), &after), OK);
    uint64_t grew = after.used[JOB_LIMIT_PAGES] - before.used[JOB_LIMIT_PAGES];
    printf("utest: %s: %u rounds of a %u KiB buffer: the heap grew %lu pages\n", utest_cur,
           ROUNDS, BIG >> 10, (unsigned long)grew);
    /* One buffer's pages and the small blocks': 16 + 400 x 64 bytes. */
    CHECK(grew <= BIG / PAGE_SIZE + (ROUNDS * (SMALL + 16)) / PAGE_SIZE + 2);
    for (unsigned i = 0; i < ROUNDS; i++)
        free(keep[i]);
    return true;
}

/* The heap reserves 1 GiB of address space (M12 Q16) and commits only what
 * is touched: a 256 MiB block whose first and last pages are used costs
 * two pages of the job, and when freed it goes back to the bump pointer, so
 * the next one is at the same place. Three of them fit; the fourth (and
 * anything over the whole heap) is NULL, not a crash, and freeing makes
 * room again. */
bool t_heap_large_alloc(void)
{
    const size_t big = 256u << 20;
    CHECK(HEAP_SIZE >= (1u << 30));
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    uint8_t *a = malloc(big);
    CHECK(a != NULL);
    a[0] = a[big - 1] = 0x5a;
    CHECK(a[0] == 0x5a && a[big - 1] == 0x5a);
    CHECK_ST(info_of(own_job(), &after), OK);
    uint64_t grew = after.used[JOB_LIMIT_PAGES] - before.used[JOB_LIMIT_PAGES];
    CHECK(grew <= 16);   /* two pages and their tables, not 65536 */
    free(a);

    uint8_t *b = malloc(big);
    CHECK(b == a);   /* the space went back to the bump pointer */
    uint8_t *c = malloc(big), *d = malloc(big);
    CHECK(c != NULL && d != NULL);
    c[big / 2] = d[big - 1] = 1;
    CHECK(malloc(big) == NULL);                  /* the fourth: 4 x 256 MiB and headers > 1 GiB */
    CHECK(malloc((size_t)HEAP_SIZE + 1) == NULL);   /* more than the whole heap */
    free(d);
    free(b);
    free(c);
    uint8_t *e = malloc(HEAP_SIZE / 2);          /* room again */
    CHECK(e != NULL);
    free(e);
    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK(after.used[JOB_LIMIT_PAGES] - before.used[JOB_LIMIT_PAGES] <= 64);
    return true;
}
