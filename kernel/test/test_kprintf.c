/* The kernel's printf: the formats drivers and the kernel use. */
#include <stdarg.h>
#include <stdbool.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/string.h>

static bool fmt_is(const char *want, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool fmt_is(const char *want, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, want) != 0) {
        kprintf("kprintf: \"%s\" gave \"%s\", want \"%s\"\n", fmt, buf, want);
        return false;
    }
    return true;
}

KTEST(kprintf_formats)
{
    KT_ASSERT(fmt_is("42 -7 ff FF", "%d %d %x %X", 42, -7, 255u, 255u));
    KT_ASSERT(fmt_is("0x1f 0 0XAB", "%#x %#x %#X", 0x1fu, 0u, 0xabu));
    KT_ASSERT(fmt_is("0x0001f", "%#07x", 0x1fu));          /* zeros after the prefix */
    KT_ASSERT(fmt_is("   0x1f", "%#7x", 0x1fu));           /* spaces before it */
    KT_ASSERT(fmt_is("0x1f   |", "%-#7x|", 0x1fu));
    KT_ASSERT(fmt_is("0x123456789a", "%#lx", 0x123456789aul));
    KT_ASSERT(fmt_is("007 |ab  |", "%03u |%-4s|", 7u, "ab"));
}
