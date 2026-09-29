/* Must be REJECTED: kernel data counts too, not just functions. */
#include <jam/driver.h>

extern volatile uint64_t user_faults;

int driver_main(const struct driver_start *s)
{
    (void)s;
    return (int)user_faults;
}
