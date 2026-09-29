/* Must be ACCEPTED: only <jam/driver.h>, a generated protocol header and
 * the freestanding headers; a struct copy big enough that GCC may emit
 * memcpy (an allowed compiler helper). */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/driver.h>
#include <idl/null.h>

struct big {
    uint64_t v[64];
};

static struct big a, b;

int driver_main(const struct driver_start *s)
{
    a = b;
    uint64_t v = 0;
    null_ping(drv_handle(s, DR_SERVE), a.v[3], &v);
    drv_log("%s", status_str(OK));
    return (int)v;
}
