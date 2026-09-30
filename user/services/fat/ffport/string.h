/* The <string.h> FatFs's ff.c includes, for FatFs only (the Makefile puts
 * this directory on its include path). Jam OS has no C library: memcpy,
 * memset, memcmp and strchr are libos's, declared in <os.h>. */
#pragma once

#include <os.h>
