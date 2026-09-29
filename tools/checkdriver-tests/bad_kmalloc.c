/* Must be REJECTED (by tools/checkdriver.py): declaring a kernel function
 * yourself gets past the include path, not past the symbol check. */
#include <jam/driver.h>

void *kmalloc(size_t n);

int driver_main(const struct driver_start *s)
{
    (void)s;
    return kmalloc(64) != NULL;
}
