#include <jam/cmdline.h>
#include <jam/string.h>

static const char *cmdline = "";

void cmdline_set(const char *s)
{
    cmdline = s ? s : "";
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

uint64_t cmdline_get_u64(const char *key, uint64_t dflt)
{
    size_t kl = strlen(key);
    for (const char *p = cmdline; *p; p++) {
        if ((p != cmdline && p[-1] != ' ') || memcmp(p, key, kl))
            continue;
        if (p[kl] == ' ' || p[kl] == '\0')
            return 600;
        if (p[kl] != '=')
            continue;
        uint64_t v = 0;
        for (const char *q = p + kl + 1; *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (uint64_t)(*q - '0');
        return v;
    }
    return dflt;
}
