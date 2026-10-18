/* The layer's heap (docs/CR-PLAN.md, LH): every allocation the call
 * layer and libjam make in a POSIX program (jam_malloc, jam_calloc,
 * jam_free: <os.h>; jam_realloc, jam_strdup: layer.h), never the
 * program's malloc.
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
 * The heap is one arena: a VMO of ARENA_SIZE mapped at the first
 * allocation, whose pages are committed when first touched (libos's
 * heap's way, user/lib/heap.c), in runs of RUN_SIZE. A block of up to
 * SMALL_MAX bytes is one of a size class's (16-byte steps up to 64, then
 * four to each doubling), each run holding blocks of one class; a freed
 * one goes on its class's list for the next block of its class, and a
 * class's runs stay its own (what it held at its peak: the layer's small
 * blocks are its descriptions, its sockets and short-lived buffers). A
 * bigger block is a span of whole runs, given back by its free to a list
 * of free spans (by address, merged with free neighbours; one that ends
 * at the runs never used goes back to them), which the next big block or
 * class run takes from first. So the heap is one mapping, made once: its
 * memory is in no count of the program's mappings or handles that moves
 * (posixtest's scm and peer cases count the job's handles across calls
 * that allocate). Only when the arena is full (or couldn't be made) is a
 * block a VMO of its own, mapped for it and unmapped by its free, its
 * length in a header before it. No VMO handle is kept: a forked child's
 * copy of the arena is a VMO it holds no handle to, so nothing here
 * decommits, and the pages a big block touched stay committed for the
 * next one. Neither the arena nor a VMO of a block's own is in libc's
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
#define SMALL_MAX  16384u                    /* bigger blocks: a span of runs */
#define CLASSES    36u                       /* 16, 32, 48, 64, 80, 96, ..., SMALL_MAX */
#define RUN_SHIFT  16u
#define RUN_SIZE   (1ull << RUN_SHIFT)       /* 64 KiB: four blocks of the biggest class */
#define ARENA_SIZE (1ull << 30)              /* address space, as libos's heap's */
#define ARENA_RUNS (ARENA_SIZE / RUN_SIZE)
#define RUN_SPAN   0xfeu                     /* run_class: a big block's first run */
#define RUN_FREE   0xffu                     /* run_class: a free span's first run, a big block's others */
#define NO_SPAN    0xffffffffu               /* the end of the list of free spans */
#define OWN_HEAD   16u                       /* a block of its own's header, 16-byte aligned after it */
#define OWN_MAGIC  0x6a6c626967626c6bull     /* a live block of its own's header */
#define OWN_MAX    (1ull << 40)              /* past any VMO the kernel would make */
#define HOLDER_OTHER 1u                      /* a holder with no thread pointer to read */

_Static_assert(RUN_SIZE % SMALL_MAX == 0, "whole blocks of the biggest class in a run");
_Static_assert(ARENA_RUNS <= UINT16_MAX, "a span's runs in run_len");

struct own {
    uint64_t len;     /* the mapping's length: the header and the block, whole pages */
    uint64_t magic;   /* OWN_MAGIC while the block is live */
};
_Static_assert(sizeof(struct own) == OWN_HEAD, "a block of its own's header");

static lock_t    heap_lock;
static uintptr_t holder;                    /* heap_holder's; 0 while the lock is free */
static uint8_t  *arena;                     /* NULL until first use */
static bool      arena_failed;              /* couldn't be made: every block is its own */
static uint32_t  runs_used;                 /* runs [0, runs_used) have been taken */
static uint32_t  runs_touched;              /* and [0, runs_touched) ever: past it, zero */
static uint8_t   run_class[ARENA_RUNS];     /* each taken run's class, RUN_SPAN or RUN_FREE */
static uint16_t  run_len[ARENA_RUNS];       /* a big block's or a free span's runs, at its first */
static uint32_t  span_next[ARENA_RUNS];     /* a free span's first run: the next free span's */
static uint32_t  free_spans = NO_SPAN;      /* the lowest free span */
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

/* ---- blocks of their own: a VMO each, once the arena has no room ------------------- */

static void *own_alloc(size_t n)
{
    if (n > OWN_MAX)
        return NULL;
    uint64_t len = (n + OWN_HEAD + PAGE - 1) & ~(PAGE - 1);
    handle_t vmo;
    uint64_t addr = 0;
    if (jam_vmo_create(len, 0, &vmo) != OK)
        return NULL;
    status_t st = jam_vmar_map(lx_self_vmar(), vmo, 0, len, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return NULL;
    struct own *o = (struct own *)(uintptr_t)addr;
    *o = (struct own){ .len = len, .magic = OWN_MAGIC };
    return o + 1;
}

static struct own *own_of(void *p)
{
    struct own *o = (struct own *)p - 1;
    if (((uintptr_t)p & (PAGE - 1)) != OWN_HEAD || o->magic != OWN_MAGIC)
        bad_free();
    return o;
}

static void own_free(void *p)
{
    struct own *o = own_of(p);
    o->magic = 0;
    (void)jam_vmar_unmap(lx_self_vmar(), (uint64_t)(uintptr_t)o, o->len);
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

/* k runs together, the first of them, or NO_SPAN: the last k of the
 * lowest free span that holds them (the span keeps its place in the
 * list), else past the runs taken. *zero: never touched since the arena
 * was made. */
static uint32_t take_runs_locked(uint32_t k, bool *zero)
{
    if (!arena && !make_arena_locked())
        return NO_SPAN;
    for (uint32_t *at = &free_spans; *at != NO_SPAN; at = &span_next[*at]) {
        uint32_t s = *at, len = run_len[s];
        if (len < k)
            continue;
        if (len == k)
            *at = span_next[s];
        else
            run_len[s] = (uint16_t)(len - k);
        *zero = false;
        return s + len - k;
    }
    if (k > ARENA_RUNS - runs_used)
        return NO_SPAN;
    uint32_t r = runs_used;
    runs_used += k;
    *zero = r >= runs_touched;
    if (runs_used > runs_touched)
        runs_touched = runs_used;
    return r;
}

/* The k runs from r back: into the list by address, merged with a free
 * span just after and one just before; a span then ending at the last run
 * taken goes back to the runs never taken. */
static void give_runs_locked(uint32_t r, uint32_t k)
{
    uint32_t *at = &free_spans, *before = NULL;   /* the links to the spans after r and before */
    while (*at != NO_SPAN && *at < r) {
        before = at;
        at = &span_next[*at];
    }
    uint32_t next = *at, *link = at;
    if (next != NO_SPAN && r + k == next) {
        k += run_len[next];
        next = span_next[next];
    }
    run_class[r] = RUN_FREE;
    if (before && *before + run_len[*before] == r) {
        r = *before;
        k += run_len[r];
        link = before;
    }
    if (r + k == runs_used) {   /* nothing above it: next is NO_SPAN */
        *link = NO_SPAN;
        runs_used = r;
        return;
    }
    run_len[r] = (uint16_t)k;
    span_next[r] = next;
    *link = r;
}

/* A block of class c, or NULL when the arena has none to give. */
static void *take_small_locked(unsigned c)
{
    void *p = free_head[c];
    if (p) {
        free_head[c] = *(void **)p;
        return p;
    }
    size_t size = class_size(c);
    if ((size_t)(run_end[c] - run_next[c]) < size) {
        bool zero;
        uint32_t r = take_runs_locked(1, &zero);
        if (r == NO_SPAN)
            return NULL;
        run_class[r] = (uint8_t)c;
        run_next[c] = arena + (uint64_t)r * RUN_SIZE;
        run_end[c] = run_next[c] + RUN_SIZE;
    }
    p = run_next[c];
    run_next[c] += size;
    return p;
}

/* A big block: a span of runs (*zero: all of it never touched), or NULL. */
static void *take_big_locked(size_t n, bool *zero)
{
    if (n > ARENA_SIZE)
        return NULL;
    uint32_t k = (uint32_t)((n + RUN_SIZE - 1) >> RUN_SHIFT);
    uint32_t r = take_runs_locked(k, zero);
    if (r == NO_SPAN)
        return NULL;
    run_class[r] = RUN_SPAN;
    run_len[r] = (uint16_t)k;
    for (uint32_t i = 1; i < k; i++)
        run_class[r + i] = RUN_FREE;   /* no block starts there */
    return arena + (uint64_t)r * RUN_SIZE;
}

/* The run of the arena's block p, checked (a small block's start in a run
 * of its class, or a big block's first run); its class in *c. */
static uint32_t run_at_locked(const void *p, unsigned *c)
{
    uint64_t off = (uint64_t)((const uint8_t *)p - arena);
    uint32_t r = (uint32_t)(off >> RUN_SHIFT);
    if (r >= runs_used)
        bad_free();
    *c = run_class[r];
    uint64_t in = off & (RUN_SIZE - 1);
    if (*c == RUN_SPAN ? in != 0 : *c >= CLASSES || in % class_size(*c))
        bad_free();
    return r;
}

/* ---- the allocator ------------------------------------------------------------------ */

/* A block of n bytes (*zero: it reads as zero), from the arena if it can. */
static void *alloc(size_t n, bool *zero)
{
    struct sig_thread *q = hold();
    void *p;
    *zero = false;
    if (n <= SMALL_MAX)
        p = take_small_locked(class_of(n ? n : 1));
    else
        p = take_big_locked(n, zero);
    unhold(q);
    if (!p) {
        p = own_alloc(n);   /* the arena full, or none: fresh pages */
        *zero = true;
    }
    return p;
}

void *jam_malloc(size_t n)
{
    bool zero;
    return alloc(n, &zero);
}

void *jam_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    bool zero;
    void *p = alloc(n * size, &zero);
    if (p && !zero)
        memset(p, 0, n * size);
    return p;
}

void jam_free(void *p)
{
    if (!p)
        return;
    if (!in_arena(p)) {
        own_free(p);
        return;
    }
    struct sig_thread *q = hold();
    unsigned c;
    uint32_t r = run_at_locked(p, &c);
    if (c == RUN_SPAN) {
        give_runs_locked(r, run_len[r]);
    } else {
        *(void **)p = free_head[c];
        free_head[c] = p;
    }
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
        unsigned c;
        uint32_t r = run_at_locked(p, &c);
        uint32_t k = c == RUN_SPAN ? run_len[r] : 0;
        unhold(q);
        have = k ? (size_t)k << RUN_SHIFT : class_size(c);
        same = k ? n > SMALL_MAX && ((n + RUN_SIZE - 1) >> RUN_SHIFT) == k   /* as many runs */
                 : n && n <= SMALL_MAX && class_of(n) == c;
    } else {
        have = own_of(p)->len - OWN_HEAD;
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
