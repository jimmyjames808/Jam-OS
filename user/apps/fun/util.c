/* libfun: maths without libm, memory, console output, arguments (fun.h). */
#include <idl/console.h>
#include "fun.h"

/* ---- maths ----------------------------------------------------------------------------- */

double log2d(double x)
{
    union { double d; uint64_t u; } u = { x };
    int e = (int)((u.u >> 52) & 0x7ff) - 1023;
    u.u = (u.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull;   /* m in [1, 2) */
    double m = u.d;
    if (m > 1.4142135623730951) {   /* m in [0.707, 1.414): a faster series */
        m *= 0.5;
        e++;
    }
    double t = (m - 1) / (m + 1), t2 = t * t;
    /* ln(m) = 2 atanh(t) */
    double ln = 2 * t * (1 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 * (1.0 / 7 + t2 * (1.0 / 9 +
                t2 * (1.0 / 11 + t2 / 13))))));
    return e + ln * 1.4426950408889634;
}

double exp2d(double x)
{
    if (x < -1000)
        return 0;
    if (x > 1000)
        x = 1000;
    double fl = floord(x);
    double f = (x - fl) * 0.6931471805599453, term = 1, sum = 1;
    for (int k = 1; k < 14; k++) {
        term *= f / k;
        sum += term;
    }
    union { double d; uint64_t u; } v = { sum };
    v.u += (uint64_t)(int64_t)fl << 52;
    return v.d;
}

double sind(double x)
{
    const double pi = 3.141592653589793, tau = 2 * pi;
    x -= tau * (double)(int64_t)(x / tau);
    if (x > pi)
        x -= tau;
    if (x < -pi)
        x += tau;
    double x2 = x * x, term = x, sum = x;
    for (int k = 1; k < 11; k++) {
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

/* ---- odds and ends ---------------------------------------------------------------------- */

void *big_alloc(uint64_t bytes)
{
    bytes = (bytes + 4095) & ~4095ull;
    handle_t v;
    if (jam_vmo_create(bytes, 0, HANDLE_INVALID, &v) != OK)
        return NULL;
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, bytes, VMAR_READ | VMAR_WRITE,
                               &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st == OK ? (void *)(uintptr_t)addr : NULL;
}

void say(const char *fmt, ...)
{
    char buf[2048];   /* console_write sends a whole 2048-byte buffer */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    handle_t con = startup_handle(SR_CONSOLE);
    if (!con || console_write(con, (uint16_t)n, (const uint8_t *)buf) != OK)
        printf("%s", buf);
}

char *commas(char *buf, size_t n, uint64_t v)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v), o = 0;
    for (int i = 0; i < len && (size_t)o + 1 < n; i++) {
        if (i && (len - i) % 3 == 0 && (size_t)o + 2 < n)
            buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
    return buf;
}

bool has_arg(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

uint64_t arg_num(int argc, char **argv, const char *name, uint64_t def)
{
    size_t k = strlen(name);
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], name, k) && argv[i][k] == '=') {
            uint64_t v = 0;
            for (const char *p = argv[i] + k + 1; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (uint64_t)(*p - '0');
            return v;
        }
    return def;
}

/* ---- self-tests ------------------------------------------------------------------------ */

static const char *check_app = "?";
static int check_width, check_failures;

void fun_selftest_begin(const char *app, int width)
{
    check_app = app;
    check_width = width;
    check_failures = 0;
}

void fun_check(bool ok, const char *what)
{
    say("%s: selftest: %-*s %s\n", check_app, check_width, what, ok ? "ok" : "FAILED");
    if (!ok)
        check_failures++;
}

int fun_selftest_end(void)
{
    say("%s: selftest %s (%d failure(s))\n", check_app, check_failures ? "FAILED" : "PASSED",
        check_failures);
    return check_failures ? 1 : 0;
}
