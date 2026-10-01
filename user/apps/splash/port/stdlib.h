/* splash: the <stdlib.h> pl_mpeg (third_party/pl_mpeg) includes: libos has
 * malloc and free; abs is here. */
#pragma once

#include <os.h>

static inline int abs(int x)
{
    return x < 0 ? -x : x;
}
