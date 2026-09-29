/* Must be ACCEPTED: only <jam/driver.h>, a generated protocol header and
 * the freestanding headers; a struct copy big enough that GCC may emit
 * memcpy (an allowed compiler helper); formatting with drv_snprintf and
 * drv_vsnprintf. */
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

static int format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = drv_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int driver_main(const struct driver_start *s)
{
    a = b;
    uint64_t v = 0;
    char line[32];
    null_ping(drv_handle(s, DR_SERVE), a.v[3], &v);
    drv_snprintf(line, sizeof(line), "%s", status_str(OK));
    format(line, sizeof(line), "%lu", (unsigned long)v);
    drv_log("%s", line);
    return (int)v;
}
