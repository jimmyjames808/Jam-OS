/* dr_mp3's implementation (third_party/dr_mp3/dr_mp3.h, vendored
 * unmodified), compiled once into libos for <mp3.h> (user/lib/mp3.c). The
 * Makefile builds this file with this directory and third_party/dr_mp3 on
 * the include path; nothing else includes dr_mp3.h.
 *
 * The configuration:
 * - DR_MP3_NO_STDIO: no FILE*; mp3.c feeds it through read/seek/tell
 *   callbacks over file_read, 64 KiB at a time.
 * - SIMD on: dr_mp3 uses SSE2 on x86_64 (user programs may use SSE; the
 *   kernel saves each thread's FPU/SSE state, kernel/arch/x86_64/fpu.c).
 * - 16-bit output (DR_MP3_FLOAT_OUTPUT not set): the synthesis filter's
 *   floats are rounded and saturated to 16 bits inside the decoder.
 * - Layer I and II are kept (DR_MP3_ONLY_MP3 not set): they cost a few KiB.
 * - Jam OS has no C library: memcpy, memmove, memset, malloc and free are
 *   libos's (<os.h>, through this directory's string.h and stdlib.h).
 *   libos has no realloc: mp3.c passes allocation callbacks without one,
 *   and dr_mp3 then grows its buffer with malloc, a copy and free; the
 *   default callbacks' realloc (never used) fails.
 * - Its asserts are off (as in a release build). */
#include <os.h>

#define DR_MP3_NO_STDIO
#define DRMP3_ASSERT(expression) ((void)0)
#define DRMP3_REALLOC(p, sz)     ((void)(p), (void)(sz), (void *)0)

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
