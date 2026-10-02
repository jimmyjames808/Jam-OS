/* netstack: the <string.h> lwIP includes (port/ is on its include path).
 * Jam OS has no C library: memcpy, memmove, memset, memcmp, strlen and
 * strncmp are libos's, declared in <os.h>. */
#pragma once

#include <os.h>
