/* Numbers for the commands: parsing counts, seconds and decibels, printing
 * sizes and decibels. */
#include <mixmath.h>
#include "sh.h"

bool sh_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

bool sh_parse_seconds(const char *s, uint64_t *ns)
{
    uint64_t whole = 0, frac = 0, scale = NS_PER_S;
    const char *p = s;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; p++, any = true) {
        if (whole > 1000000)
            return false;
        whole = whole * 10 + (uint64_t)(*p - '0');
    }
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, any = true)
            if (scale >= 10) {
                scale /= 10;
                frac += (uint64_t)(*p - '0') * scale;
            }
    if (*p || !any)
        return false;
    *ns = whole * NS_PER_S + frac;
    return true;
}

const char *sh_human(uint64_t b, char *buf, size_t cap)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    unsigned u = 0;
    uint64_t scale = 1;
    while (u < 4 && b >= scale * 1024) {
        scale *= 1024;
        u++;
    }
    if (!u) {
        snprintf(buf, cap, "%lu B", (unsigned long)b);
    } else {
        uint64_t tenths = (b * 10 + scale / 2) / scale;
        snprintf(buf, cap, "%lu.%lu %s", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10),
                 units[u]);
    }
    return buf;
}

const char *sh_db(int32_t cb, char *buf, size_t size)
{
    return mix_db_str(cb, buf, size);
}

bool sh_parse_db(const char *s, int32_t *cb)
{
    return mix_parse_db(s, cb);
}
