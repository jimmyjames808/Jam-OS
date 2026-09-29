/* Must be REJECTED: a driver calling into another driver (here the kernel
 * build's null driver entry). */
#include <jam/driver.h>

int driver_main__null(const struct driver_start *s);

int driver_main(const struct driver_start *s)
{
    return driver_main__null(s);
}
