/* splash: pl_mpeg's implementation (third_party/pl_mpeg, MIT), compiled
 * once, here.
 *
 * No stdio (PLM_NO_STDIO): the file is in memory (bootfs), read through
 * plm_create_with_memory. Memory: small blocks from libos's heap; big ones
 * (a video decoder's three frames: 16.6 MB at 2560x1440, more than the
 * whole heap) each from a VMO of their own, mapped, remembered in `bigs`
 * so that free unmaps them. Both threads ask for big ones: the sound
 * thread's plm_create_with_memory makes a video decoder too, which reads
 * the sequence header and takes its frames before plm_set_video_enabled
 * can say no (audio review, item 6), so the table is under a spinlock (the
 * VMO calls are made outside it). pl_mpeg doesn't check
 * what malloc returns, so an allocation that fails would crash it: the
 * splash then just ends (the console draws). realloc is only used to grow a buffer that is appended
 * to, which a memory buffer never is, so libos (which has no realloc) gets
 * a stand-in that fails. */
#include <os.h>

#define BIG      (256u << 10)   /* bytes: from here on, a VMO of its own */
#define MAX_BIGS 16             /* at once (two decoders hold four) */

#define TAKEN    ((void *)1)    /* a slot claimed, its VMO not mapped yet */

static struct { void *p; uint64_t len; } bigs[MAX_BIGS];
static int bigs_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&bigs_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&bigs_lock, __ATOMIC_RELAXED))
            __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_store_n(&bigs_lock, 0, __ATOMIC_RELEASE);
}

static void *plm_alloc(size_t n)
{
    if (n < BIG)
        return malloc(n);
    uint64_t len = (n + 4095) & ~4095ull, addr = 0;
    unsigned i = 0;
    lock();
    while (i < MAX_BIGS && bigs[i].p)
        i++;
    if (i < MAX_BIGS)
        bigs[i].p = TAKEN;
    unlock();
    if (i == MAX_BIGS)
        return NULL;
    handle_t v;
    status_t st = jam_vmo_create(len, 0, HANDLE_INVALID, &v);
    if (st == OK) {
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, len, VMAR_READ | VMAR_WRITE, &addr);
        jam_handle_close(v);   /* the mapping keeps it */
    }
    lock();
    bigs[i].len = len;
    bigs[i].p = st == OK ? (void *)(uintptr_t)addr : NULL;
    unlock();
    return st == OK ? (void *)(uintptr_t)addr : NULL;
}

static void plm_release(void *p)
{
    uint64_t len = 0;
    lock();
    for (unsigned i = 0; p && i < MAX_BIGS; i++) {
        if (bigs[i].p == p) {
            len = bigs[i].len;
            bigs[i].p = NULL;
            break;
        }
    }
    unlock();
    if (len)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p, len);
    else
        free(p);
}

static void *no_realloc(void *p, size_t n)
{
    (void)p;
    (void)n;
    return NULL;
}

#define PLM_NO_STDIO
#define PLM_MALLOC(sz)     plm_alloc(sz)
#define PLM_FREE(p)        plm_release(p)
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
