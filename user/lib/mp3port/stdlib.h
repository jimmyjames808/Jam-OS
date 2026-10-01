/* The <stdlib.h> dr_mp3 includes, for dr_mp3 only (the Makefile puts this
 * directory on its include path). Jam OS has no C library: malloc and free
 * are libos's, declared in <os.h>; realloc is not there (see
 * dr_mp3_impl.c). */
#pragma once

#include <os.h>
