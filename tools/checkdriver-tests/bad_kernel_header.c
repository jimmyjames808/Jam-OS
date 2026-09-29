/* Must be REJECTED (at compile time): a kernel header is not on a driver's
 * include path. */
#include <jam/driver.h>
#include <jam/process.h>

int driver_main(const struct driver_start *s)
{
    (void)s;
    return process_current() != NULL;
}
