/* The layer's heap (docs/CR-PLAN.md, LH): every allocation the call
 * layer and libjam make in a POSIX program (jam_malloc, jam_calloc,
 * jam_free: <os.h>; jam_realloc: layer.h), never the program's malloc.
 *
 * Why. A program may replace malloc with its own (Chromium's
 * PartitionAlloc) that takes its lock and then calls into the layer
 * (mmap, for its pages): a layer that allocated from it there would wait
 * for that lock for ever. And a signal handler may interrupt the program
 * inside its malloc and make a call that allocates (Chromium's crash
 * handler opens /proc/self/status): Linux's calls never touch the
 * program's heap, so ours mustn't either (docs/SG-REVIEW.md, finding 1).
 * musl's own allocator is no answer: in a program that keeps it, it is
 * the program's malloc. tools/checklayerheap.py (make check) proves no
 * object of the layer or of libjam in libc.a reaches it.
 *
 * The heap. A block of up to SMALL_MAX bytes comes from the arena: one
 * VMO of ARENA_SIZE mapped on first use, whose pages are committed when
 * first touched (libos's heap's way, user/lib/heap.c), cut into runs of
 * RUN_SIZE, each run holding blocks of one size class (16-byte steps up
 * to 64, then four to each doubling). A freed block goes on its class's
 * list, the next block of its class; runs are never given back (what a
 * class held at its peak stays the class's: the layer's blocks are its
 * descriptions, its sockets and short-lived buffers). A bigger block, or
 * any once the arena is full, is a VMO of its own, mapped for it and
 * unmapped by its free, with its length in a header before it: fresh
 * pages, so jam_calloc clears nothing there. No VMO handle is kept: a
 * forked child's copy of the arena is a VMO it holds no handle to, so
 * nothing here decommits. Neither the arena nor a big block is in libc's
 * table of mappings (maptable.h), so no munmap of the program's reaches
 * them.
 *
 * The rules (fork.c and signal.c keep them with this file):
 * - The lock is a leaf: nothing is taken or waited for with it held but
 *   the kernel's VMO and address-space calls.
 * - It is held only inside a quiet stretch (sig_quiet_begin): a raise
 *   that comes there is put off to its end, so no handler runs on a
 *   thread that holds it, and no handler's call can wait for it. A fault
 *   there is the kernel's kill, as one with sig_lock held: sig_fault_begin
 *   asks heap_holder.
 * - fork takes it after every other lock of the layer's, for every way
 *   in (fork, _Fork and a raw SYS_fork, vfork, both clones: heap_fork),
 *   gives it back first, and the child makes it fresh before anything
 *   else, so the child's copy of the heap is never one half-changed by
 *   another thread. */
#include <stddef.h>
#include "layer.h"
#include "sig.h"

#define PAGE       4096ull
#define SMALL_MAX  16384u                    /* bigger blocks: a VMO each */
#define CLASSES    36u                       /* 16, 32, 48, 64, 80, 96, ..., SMALL_MAX */
#define RUN_SHIFT  16u
#define RUN_SIZE   (1ull << RUN_SHIFT)       /* 64 KiB: four blocks of the biggest class */
#define ARENA_SIZE (1ull << 30)              /* address space, as libos's heap's */
#define ARENA_RUNS (ARENA_SIZE / RUN_SIZE)
#define BIG_HEAD   16u                       /* a big block's header, 16-byte aligned after it */
#define BIG_MAGIC  0x6a6c626967626c6bull     /* a live big block's header */
#define BIG_MAX    (1ull << 40)              /* past any VMO the kernel would make */
#define HOLDER_OTHER 1u                      /* a holder with no thread pointer to read */

_Static_assert(RUN_SIZE % SMALL_MAX == 0, "whole blocks of the biggest class in a run");

struct big {
    uint64_t len;     /* the mapping's length: the header and the block, whole pages */
    uint64_t magic;   /* BIG_MAGIC while the block is live */
};
_Static_assert(sizeof(struct big) == BIG_HEAD, "a big block's header");

static lock_t    heap_lock;
static uintptr_t holder;                    /* heap_holder's; 0 while the lock is free */
static uint8_t  *arena;                     /* NULL until first use */
static bool      arena_failed;              /* couldn't be made: every block is big */
static uint32_t  runs_used;                 /* [0, runs_used) have a class */
static uint8_t   run_class[ARENA_RUNS];     /* each used run's class */
static void     *free_head[CLASSES];        /* each class's freed blocks, linked by their first word */
static uint8_t  *run_next[CLASSES];         /* each class's newest run: its next block */
static uint8_t  *run_end[CLASSES];          /* and its end */

/* A size's class (1 <= n <= SMALL_MAX), and a class's block size. */
static unsigned class_of(size_t n)
{
    if (n <= 64)
        return (unsigned)((n + 15) / 16) - 1;
    unsigned b = 63u - (unsigned)__builtin_clzll(n - 1);   /* 2^b <= n - 1 < 2^(b + 1) */
    return 4u + (b - 6u) * 4u + (unsigned)(((n - 1) >> (b - 2)) & 3);
}

static size_t class_size(unsigned c)
{
    if (c < 4)
        return 16u * (c + 1);
    unsigned b = 6u + (c - 4) / 4, q = (c - 4) % 4;
    return ((size_t)1 << b) + ((size_t)(q + 1) << (b - 2));
}

_Static_assert(CLASSES == 4u + (14u - 6u) * 4u, "the last class is SMALL_MAX (2^14)");

static uintptr_t thread_pointer(void)
{
    uintptr_t tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));   /* it points at itself: the FS base */
    return tp;
}

/* The lock, in a quiet stretch; holder marked (a listed thread's thread
 * pointer: an unlisted one runs no handler, so the fault entry never asks
 * for it). */
static struct sig_thread *hold(void)
{
    struct sig_thread *q = sig_quiet_begin();
    lock_take(&heap_lock);
    __atomic_store_n(&holder, q ? thread_pointer() : HOLDER_OTHER, __ATOMIC_RELAXED);
    return q;
}

static void unhold(struct sig_thread *q)
{
    __atomic_store_n(&holder, 0, __ATOMIC_RELAXED);
    lock_give(&heap_lock);
    sig_quiet_end(q);
}

uintptr_t heap_holder(void)
{
    return __atomic_load_n(&holder, __ATOMIC_RELAXED);
}

void heap_fork(int step)
{
    if (step == LX_FORK_TAKE) {
        lock_take(&heap_lock);   /* in a call or a quiet stretch already: fork.c */
        __atomic_store_n(&holder, thread_pointer(), __ATOMIC_RELAXED);
    } else if (step == LX_FORK_GIVE) {
        __atomic_store_n(&holder, 0, __ATOMIC_RELAXED);
        lock_give(&heap_lock);
    } else {
        holder = 0;
        heap_lock = (lock_t){ 0 };
    }
}

static _Noreturn void bad_free(void)
{
    static const char line[] = "posix: the layer's heap: a free of no live block\n";
    (void)jam_debug_write(line, sizeof(line) - 1);
    __builtin_trap();
}

/* ---- big blocks: a VMO each --------------------------------------------------------- */

static void *big_alloc(size_t n)
{
    if (n > BIG_MAX)
        return NULL;
    uint64_t len = (n + BIG_HEAD + PAGE - 1) & ~(PAGE - 1);
    handle_t vmo;
    uint64_t addr = 0;
    if (jam_vmo_create(len, 0, &vmo) != OK)
        return NULL;
    status_t st = jam_vmar_map(lx_self_vmar(), vmo, 0, len, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return NULL;
    struct big *b = (struct big *)(uintptr_t)addr;
    *b = (struct big){ .len = len, .magic = BIG_MAGIC };
    return b + 1;
}

static struct big *big_of(void *p)
{
    struct big *b = (struct big *)p - 1;
    if (((uintptr_t)p & (PAGE - 1)) != BIG_HEAD || b->magic != BIG_MAGIC)
        bad_free();
    return b;
}

static void big_free(void *p)
{
    struct big *b = big_of(p);
    b->magic = 0;
    (void)jam_vmar_unmap(lx_self_vmar(), (uint64_t)(uintptr_t)b, b->len);
}

/* ---- the arena ---------------------------------------------------------------------- */

static bool in_arena(const void *p)
{
    uint8_t *a = __atomic_load_n(&arena, __ATOMIC_ACQUIRE);
    return a && (uintptr_t)p - (uintptr_t)a < ARENA_SIZE;
}

static bool make_arena_locked(void)
{
    if (arena_failed)
        return false;
    handle_t vmo;
    uint64_t addr = 0;
    status_t st = jam_vmo_create(ARENA_SIZE, 0, &vmo);
    if (st == OK) {
        st = jam_vmar_map(lx_self_vmar(), vmo, 0, ARENA_SIZE, VMAR_READ | VMAR_WRITE, &addr);
        jam_handle_close(vmo);   /* the mapping keeps it */
    }
    if (st != OK) {
        arena_failed = true;
        return false;
    }
    __atomic_store_n(&arena, (uint8_t *)(uintptr_t)addr, __ATOMIC_RELEASE);
    return true;
}

/* A block of class c, or NULL when the arena has none to give. */
static void *take_locked(unsigned c)
{
    void *p = free_head[c];
    if (p) {
        free_head[c] = *(void **)p;
        return p;
    }
    size_t size = class_size(c);
    if ((size_t)(run_end[c] - run_next[c]) < size) {
        if ((!arena && !make_arena_locked()) || runs_used == ARENA_RUNS)
            return NULL;
        run_class[runs_used] = (uint8_t)c;
        run_next[c] = arena + (uint64_t)runs_used++ * RUN_SIZE;
        run_end[c] = run_next[c] + RUN_SIZE;
    }
    p = run_next[c];
    run_next[c] += size;
    return p;
}

/* The class of the arena's block p, checked: a used run's, at one of its
 * blocks' starts. */
static unsigned class_at_locked(const void *p)
{
    uint64_t off = (uint64_t)((const uint8_t *)p - arena), run = off >> RUN_SHIFT;
    if (run >= runs_used)
        bad_free();
    unsigned c = run_class[run];
    if ((off & (RUN_SIZE - 1)) % class_size(c))
        bad_free();
    return c;
}

/* ---- the allocator ------------------------------------------------------------------ */

void *jam_malloc(size_t n)
{
    if (n > SMALL_MAX)
        return big_alloc(n);
    struct sig_thread *q = hold();
    void *p = take_locked(class_of(n ? n : 1));
    unhold(q);
    return p ? p : big_alloc(n);   /* the arena full, or none */
}

void *jam_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = jam_malloc(n * size);
    if (p && in_arena(p))   /* a big block's pages are fresh */
        memset(p, 0, n * size);
    return p;
}

void jam_free(void *p)
{
    if (!p)
        return;
    if (!in_arena(p)) {
        big_free(p);
        return;
    }
    struct sig_thread *q = hold();
    unsigned c = class_at_locked(p);
    *(void **)p = free_head[c];
    free_head[c] = p;
    unhold(q);
}

char *jam_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = jam_malloc(n);
    return d ? memcpy(d, s, n) : NULL;
}

void *jam_realloc(void *p, size_t n)
{
    if (!p)
        return jam_malloc(n);
    size_t have;
    bool same;   /* p is what a new block of n would be: kept */
    if (in_arena(p)) {
        struct sig_thread *q = hold();
        unsigned c = class_at_locked(p);
        unhold(q);
        have = class_size(c);
        same = n && n <= SMALL_MAX && class_of(n) == c;
    } else {
        have = big_of(p)->len - BIG_HEAD;
        same = n > SMALL_MAX && n <= have && n > have / 2;   /* fits, and not mostly unused */
    }
    if (same)
        return p;
    void *r = jam_malloc(n);
    if (!r)
        return NULL;   /* p stays as it was */
    memcpy(r, p, n < have ? n : have);
    jam_free(p);
    return r;
}
