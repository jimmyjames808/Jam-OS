/* netstack: the <stdlib.h> lwIP includes. It uses one function libos
 * hasn't: atoi, for netif_find's interface number ("en0"). */
#pragma once

#include <os.h>

/* A decimal number at the start of s, or 0: no sign, and it stops at the
 * first non-digit or after 9 digits (the most an int always holds). */
static inline int atoi(const char *s)
{
    int v = 0;
    for (int i = 0; i < 9 && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}
