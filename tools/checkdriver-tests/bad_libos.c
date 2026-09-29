/* Must be REJECTED: libos is linked into every driver process, but only
 * through <jam/driver.h>: calling its vsnprintf directly (instead of
 * drv_vsnprintf) is not allowed. */
#include <stdarg.h>
#include <jam/driver.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

static int format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int driver_main(const struct driver_start *s)
{
    char line[16];
    return format(line, sizeof(line), "%u", s->nhandles);
}
