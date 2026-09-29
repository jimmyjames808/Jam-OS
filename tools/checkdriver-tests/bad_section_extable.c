/* Must be REJECTED: a user-copy fixup entry in __ex_table makes a kernel
 * page fault at the driver's own instruction resume instead of panicking
 * (a probe for arbitrary kernel memory). */
#include <stdint.h>
#include <jam/driver.h>

int driver_main(const struct driver_start *s)
{
    uint64_t v = 0;
    __asm__ volatile("1: movq (%1), %0\n2:\n"
                     ".pushsection __ex_table, \"a\"\n.quad 1b, 2b\n.popsection"
                     : "+r"(v)
                     : "r"((uint64_t)s->nhandles << 40));
    return (int)v;
}
