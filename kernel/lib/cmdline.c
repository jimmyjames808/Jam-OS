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
