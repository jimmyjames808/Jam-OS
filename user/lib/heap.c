/* A small heap: one VMO of HEAP_SIZE mapped read-write into our address
 * space on the first malloc (pages are committed when first touched, and
 * charged to our job then). Blocks come from a bump pointer; freed blocks
 * go on a first-fit free list and are split when much bigger than asked.
 * No coalescing: good enough for M5's programs, replaced when something
 * needs better. A spinlock makes it safe for several threads. */
#include <os.h>

#define ALIGN       16u
#define SPLIT_MIN   64u   /* don't split off remainders smaller than this */
#define MAGIC_USED  0x7573656475736564ull
#define MAGIC_FREE  0x6672656566726565ull

struct block {
    uint64_t size;    /* payload bytes, a multiple of ALIGN */
    uint64_t magic;
    /* payload; while free its first 8 bytes link the free list */
};

static uint8_t      *heap_base, *heap_next, *heap_end;
static struct block *free_list;
static int           heap_lock;
static int           heap_failed;

static void lock(void)
{
    while (__atomic_exchange_n(&heap_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&heap_lock, __ATOMIC_RELAXED))
            __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_store_n(&heap_lock, 0, __ATOMIC_RELEASE);
}

static struct block **next_of(struct block *b)
{
    return (struct block **)(b + 1);
}

/* Map the heap VMO. Needs the SR_SELF_VMAR handle from the startup message. */
static int heap_init(void)
{
    if (heap_failed)
        return -1;
    handle_t vmar = startup_handle(SR_SELF_VMAR), vmo;
    uint64_t addr = 0;
    status_t st = vmar == HANDLE_INVALID ? ERR_BAD_HANDLE
                : jam_vmo_create(HEAP_SIZE, 0, HANDLE_INVALID, &vmo);
    if (st == OK) {
        st = jam_vmar_map(vmar, vmo, 0, HEAP_SIZE, VMAR_READ | VMAR_WRITE, &addr);
        jam_handle_close(vmo);   /* the mapping keeps the VMO alive */
    }
    if (st != OK) {
        heap_failed = 1;
        /* Short enough that printf formats it on the stack, without malloc. */
        printf("libos: heap unavailable (%s)\n", status_str(st));
        return -1;
    }
    heap_base = heap_next = (uint8_t *)(uintptr_t)addr;
    heap_end = heap_base + HEAP_SIZE;
    return 0;
}

void *malloc(size_t n)
{
    if (n == 0)
        n = 1;
    if (n > HEAP_SIZE)
        return NULL;
    uint64_t size = (n + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);

    lock();
    if (!heap_base && heap_init() != 0) {
        unlock();
        return NULL;
    }
    struct block **pp = &free_list, *b = NULL;
    for (; *pp; pp = next_of(*pp)) {
        if ((*pp)->size >= size) {
            b = *pp;
            *pp = *next_of(b);
            break;
        }
    }
    if (b && b->size >= size + sizeof(struct block) + SPLIT_MIN) {
        struct block *rest = (struct block *)((uint8_t *)(b + 1) + size);
        rest->size = b->size - size - sizeof(struct block);
        rest->magic = MAGIC_FREE;
        *next_of(rest) = free_list;
        free_list = rest;
        b->size = size;
    }
    if (!b) {
        if ((uint64_t)(heap_end - heap_next) < sizeof(struct block) + size) {
            unlock();
            return NULL;
        }
        b = (struct block *)heap_next;
        b->size = size;
        heap_next += sizeof(struct block) + size;
    }
    b->magic = MAGIC_USED;
    unlock();
    return b + 1;
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    if ((uint8_t *)b < heap_base || (uint8_t *)p > heap_next || b->magic != MAGIC_USED) {
        printf("libos: free(%p): not a live heap block\n", p);
        __builtin_trap();
    }
    lock();
    b->magic = MAGIC_FREE;
    *next_of(b) = free_list;
    free_list = b;
    unlock();
}
