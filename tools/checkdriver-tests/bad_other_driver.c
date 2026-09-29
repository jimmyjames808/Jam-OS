/* Must be REJECTED: a driver calling into another driver (here a function
 * of the null driver's, declared by hand). */
#include <jam/driver.h>

int null_serve(const struct driver_start *s);

int driver_main(const struct driver_start *s)
{
    return null_serve(s);
}
