/* Must be REJECTED: a constructor lands in .init_array. Nothing runs it
 * today, but no section outside .text/.rodata/.data/.bss has any business
 * in a driver object. */
#include <jam/driver.h>

static int hits;
__attribute__((constructor)) static void ctor(void) { hits++; }

int driver_main(const struct driver_start *s)
{
    (void)s;
    return hits;
}
