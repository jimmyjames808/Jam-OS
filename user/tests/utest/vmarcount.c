/* utest: process_get_info's mapping count, and the kernel's merging of
 * neighbouring mappings as a program sees it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define PG PAGE_SIZE
#define RW (VMAR_READ | VMAR_WRITE)

static uint32_t mappings(void)
{
    struct process_info pi;
    if (jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) != OK ||
        (pi.flags & ~PROCESS_FLAG_JIT))
        return 0;
    return pi.mappings;
}

/* Our own count is there; a protect in the middle of a mapping makes it
 * three and the protect back one again; a piece unmapped and mapped back
 * at its offset merges with the rest. */
bool t_vmar_mappings(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), v;
    uint32_t n0 = mappings();
    CHECK(n0 > 0);
    CHECK_ST(jam_vmo_create(8 * PG, 0, &v), OK);
    uint64_t a = 0;
    CHECK_ST(jam_vmar_map(vmar, v, 0, 8 * PG, RW, &a), OK);
    uint32_t n1 = mappings();
    CHECK_EQ(n1, n0 + 1);   /* a new VMO: nothing to merge with */
    CHECK_ST(jam_vmar_protect(vmar, a + 2 * PG, 2 * PG, VMAR_READ), OK);
    CHECK_EQ(mappings(), n1 + 2);
    CHECK_ST(jam_vmar_protect(vmar, a + 2 * PG, 2 * PG, RW), OK);
    CHECK_EQ(mappings(), n1);
    CHECK_ST(jam_vmar_unmap(vmar, a + 4 * PG, 4 * PG), OK);
    CHECK_EQ(mappings(), n1);
    uint64_t b = a + 4 * PG;
    CHECK_ST(jam_vmar_map(vmar, v, 4 * PG, 4 * PG, RW | VMAR_FIXED, &b), OK);
    CHECK_EQ(mappings(), n1);
    ((volatile uint8_t *)(uintptr_t)a)[7 * PG] = 1;   /* the merged mapping works */
    CHECK_ST(jam_vmar_unmap(vmar, a, 8 * PG), OK);
    CHECK_EQ(mappings(), n0);
    jam_handle_close(v);
    return true;
}
