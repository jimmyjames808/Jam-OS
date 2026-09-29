/* Must be REJECTED: a driver defining a <jam/driver.h> function itself (in
 * the process build it would shadow libos's). */
#include <jam/driver.h>

void *drv_malloc(size_t n)
{
    (void)n;
    return NULL;
}

int driver_main(const struct driver_start *s)
{
    (void)s;
    return drv_malloc(1) != NULL;
}
