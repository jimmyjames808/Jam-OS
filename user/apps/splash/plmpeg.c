/* splash: pl_mpeg's implementation (third_party/pl_mpeg, MIT), compiled
 * once, here.
 *
 * No stdio (PLM_NO_STDIO): the file is in memory (bootfs), read through
 * plm_create_with_memory. Memory comes from libos's heap. realloc is only
 * used to grow a buffer that is appended to, which a memory buffer never
 * is, so libos (which has no realloc) gets a stand-in that fails. */
#include <os.h>

static void *no_realloc(void *p, size_t n)
{
    (void)p;
    (void)n;
    return NULL;
}

#define PLM_NO_STDIO
#define PLM_MALLOC(sz)     malloc(sz)
#define PLM_FREE(p)        free(p)
#define PLM_REALLOC(p, sz) no_realloc(p, sz)
#define PL_MPEG_IMPLEMENTATION

/* The vendored code as released: its own warnings under -Wall -Wextra are
 * not ours to fix (it is not edited), so they are not errors here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include <pl_mpeg.h>
#pragma GCC diagnostic pop
