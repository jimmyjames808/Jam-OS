/* utest: random numbers from user space: the random_get system call and
 * libos's os_random / os_random_u32 (<os.h>). The generator itself is the
 * kernel's ktests' (kernel/test/test_random.c). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "utest.h"

/* Read-only: rodata is mapped without write. */
static const uint8_t read_only[64] = "random_get may not write here";

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

bool t_random_get(void)
{
    uint8_t a[RANDOM_GET_MAX + 1], b[RANDOM_GET_MAX];
    memset(a, 0, sizeof(a));
    CHECK_ST(jam_random_get(a, RANDOM_GET_MAX), OK);
    CHECK(a[RANDOM_GET_MAX] == 0);   /* not a byte past len */
    CHECK_ST(jam_random_get(b, RANDOM_GET_MAX), OK);
    CHECK(!all_zero(a, RANDOM_GET_MAX) && memcmp(a, b, RANDOM_GET_MAX));
    CHECK_ST(jam_random_get(NULL, 0), OK);   /* nothing asked, nothing touched */

    /* Refused: too long, and memory that isn't ours to write. */
    CHECK_ST(jam_random_get(a, RANDOM_GET_MAX + 1), ERR_INVALID_ARGS);
    CHECK_ST(jam_random_get(a, UINT64_MAX), ERR_INVALID_ARGS);
    CHECK_ST(jam_random_get(NULL, 8), ERR_INVALID_ARGS);
    CHECK_ST(jam_random_get((void *)16, 8), ERR_INVALID_ARGS);
    CHECK_ST(jam_random_get((void *)0xffff800000001000ull, 8), ERR_INVALID_ARGS);
    CHECK_ST(jam_random_get((void *)read_only, 8), ERR_INVALID_ARGS);
    CHECK(!memcmp(read_only, "random_get may not write here", 30));
    return true;
}

bool t_os_random(void)
{
    /* More than one call's worth, and an odd length. */
    size_t n = 3 * RANDOM_GET_MAX + 17;
    uint8_t *p = malloc(n + 1), *q = malloc(n);
    CHECK(p && q);
    p[n] = 0x5a;
    os_random(p, n);
    os_random(q, n);
    CHECK(p[n] == 0x5a);
    CHECK(!all_zero(p + n - 17, 17) && memcmp(p, q, n));
    os_random(p, 0);
    /* 32 words: every bit is set in one and clear in another (a working
     * generator misses with odds of about 64 in 2^32). */
    uint32_t any = 0, all = UINT32_MAX;
    for (unsigned i = 0; i < 32; i++) {
        uint32_t v = os_random_u32();
        any |= v;
        all &= v;
    }
    CHECK(any == UINT32_MAX && all == 0);
    free(p);
    free(q);
    return true;
}
