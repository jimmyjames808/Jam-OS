/* Must be REJECTED: 128-bit division needs a libgcc helper (__udivti3),
 * which is not part of the driver surface; only memcpy/memmove/memset/
 * memcmp are allowed compiler helpers. */
#include <jam/driver.h>

int driver_main(const struct driver_start *s)
{
    unsigned __int128 x = (unsigned __int128)drv_clock_ns() << 64;
    return (int)(x / (unsigned __int128)(uintptr_t)s);
}
