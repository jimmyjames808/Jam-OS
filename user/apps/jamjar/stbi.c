/* jamjar: stb_image's implementation (third_party/stb_image, public domain
 * or MIT), compiled once, here, for the album covers (cover.c): PNG and
 * JPEG only, from memory, no stdio, no HDR.
 *
 * The data is untrusted (a file on a stick), so it goes through stb_image
 * only after cover.c has checked the image's size (stbi_info), and every
 * byte stb_image allocates comes from one arena: a VMO of ARENA bytes,
 * mapped once, bump-allocated, emptied before each image (stbi_arena_reset).
 * A picture that needs more fails to decode (stb_image checks what its
 * allocator returns) and the album keeps its berries. The arena's pages
 * are committed as they are first touched, so memory is the biggest image
 * decoded so far, never more than ARENA. One thread (cover.c's) decodes. */
#include <os.h>
#include "jamjar.h"

#define ARENA (40u << 20)
#define ALIGN 16u

static uint8_t *arena;
static size_t used;

/* Each block is preceded by its size, so realloc can copy it. */
static void *arena_alloc(size_t n)
{
    if (!arena) {
        handle_t v;
        uint64_t addr = 0;
        if (jam_vmo_create(ARENA, 0, HANDLE_INVALID, &v) != OK)
            return NULL;
        status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, ARENA,
                                   VMAR_READ | VMAR_WRITE, &addr);
        jam_handle_close(v);   /* the mapping keeps it */
        if (st != OK)
            return NULL;
        arena = (uint8_t *)(uintptr_t)addr;
    }
    size_t need = (n + ALIGN - 1) / ALIGN * ALIGN + ALIGN;
    if (n > ARENA || need > ARENA - used)
        return NULL;
    uint8_t *p = arena + used;
    *(size_t *)p = n;
    used += need;
    return p + ALIGN;
}

static void *arena_realloc(void *old, size_t n)
{
    void *p = arena_alloc(n);
    if (p && old) {
        size_t had = *(size_t *)((uint8_t *)old - ALIGN);
        memcpy(p, old, had < n ? had : n);
    }
    return p;
}

void stbi_arena_reset(void)
{
    used = 0;
}

void *stbi_arena_take(size_t n)
{
    return arena_alloc(n);
}

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS (1 << 14)   /* cover.c refuses far less (COVER_MAX_SIDE) */
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(n)       arena_alloc(n)
#define STBI_REALLOC(p, n)   arena_realloc(p, n)
#define STBI_FREE(p)         ((void)(p))   /* the arena is emptied whole */
/* Jam OS threads have no thread-local storage (no TLS block, FS base 0):
 * stb_image's few per-thread settings (flipping, premultiplying) are left
 * at their defaults and are plain variables here. Only cover.c's thread
 * decodes (and the self-test, which has no such thread). */
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION

/* The vendored code as released: its own warnings under -Wall -Wextra are
 * not ours to fix (it is not edited), so they are not errors here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <stb_image.h>
#pragma GCC diagnostic pop

bool stbi_size(const uint8_t *data, size_t n, int *w, int *h)
{
    int comp = 0;
    return n <= INT32_MAX && stbi_info_from_memory(data, (int)n, w, h, &comp);
}

uint8_t *stbi_rgba(const uint8_t *data, size_t n, int *w, int *h)
{
    int comp = 0;
    if (n > INT32_MAX)
        return NULL;
    return stbi_load_from_memory(data, (int)n, w, h, &comp, 4);
}
