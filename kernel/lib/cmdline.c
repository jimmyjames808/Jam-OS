/* The kernel command line: a boot word is a whole space-separated word;
 * a value is key=N in decimal (`vlan=` is read strictly, by cmdline_vlan). The string comes from the loader and is
 * never written, so reading it needs no lock. */
#include <jam/cmdline.h>
#include <jam/string.h>

static const char *cmdline = "";

void cmdline_set(const char *s)
{
    cmdline = s ? s : "";
}

const char *cmdline_get(void)
{
    return cmdline;
}

bool cmdline_has(const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = cmdline; *p; p++)
        if ((p == cmdline || p[-1] == ' ') && !memcmp(p, word, wl) &&
            (p[wl] == ' ' || p[wl] == '\0'))
            return true;
    return false;
}

uint64_t cmdline_get_u64(const char *key, uint64_t dflt, uint64_t bare)
{
    size_t kl = strlen(key);
    for (const char *p = cmdline; *p; p++) {
        if ((p != cmdline && p[-1] != ' ') || memcmp(p, key, kl))
            continue;
        if (p[kl] == ' ' || p[kl] == '\0')
            return bare;
        if (p[kl] != '=')
            continue;
        uint64_t v = 0;
        for (const char *q = p + kl + 1; *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (uint64_t)(*q - '0');
        return v;
    }
    return dflt;
}

/* The id in a `vlan` word's value (n bytes at v), or 0 if it isn't one:
 * 1 to 4 decimal digits, 1..4094. */
static uint32_t vlan_value(const char *v, size_t n)
{
    if (n == 0 || n > 4)
        return 0;
    uint32_t id = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return 0;
        id = id * 10 + (uint32_t)(v[i] - '0');
    }
    return id >= 1 && id <= 4094 ? id : 0;
}

uint32_t cmdline_vlan(const char *line, uint32_t dflt)
{
    bool seen = false;
    uint32_t vlan = 0;
    for (const char *p = line; *p;) {
        while (*p == ' ')
            p++;
        size_t n = 0;
        while (p[n] && p[n] != ' ')
            n++;
        /* "vlan" alone, or "vlan=<value>": anything else starting with
         * "vlan" (vlanx=3) is another word. */
        if (n >= 4 && !memcmp(p, "vlan", 4) && (n == 4 || p[4] == '=')) {
            uint32_t id = n > 5 ? vlan_value(p + 5, n - 5) : 0;
            vlan = seen && id != vlan ? 0 : id;
            if (!vlan)
                return 0;   /* fail closed: one bad or disagreeing word is no VLAN */
            seen = true;
        }
        p += n;
    }
    return seen ? vlan : dflt;
}
