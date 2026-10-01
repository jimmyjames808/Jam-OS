/* The <string.h> dr_mp3 includes, for dr_mp3 only (the Makefile puts this
 * directory on its include path). Jam OS has no C library: memcpy,
 * memmove and memset are libos's, declared in <os.h>. */
#pragma once

#include <os.h>
