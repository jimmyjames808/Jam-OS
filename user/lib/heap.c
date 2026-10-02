/* A small heap: one VMO of HEAP_SIZE mapped read-write into our address
 * space on the first malloc (pages are committed when first touched, and
 * charged to our job then, for good). Blocks come from a bump pointer;
 * freed blocks go on a free list kept in address order, and a freed block
 * merges with a free neighbour on either side, or gives its space back to
 * the bump pointer when it is the last block. malloc takes the first
 * (lowest) free block that fits and splits it when it is much bigger than
 * asked.
 *
 * Why the merging and the order: without them a long-running program that
 * asks for the same big buffer again and again (the shell's `ls`, 68 KiB
 * each time) finds the last one split by a small allocation made
 * meanwhile, never whole again, and the bump pointer moves on: its pages
 * grew on every command until the heap ran out. Lowest-first keeps small
 * blocks together at the bottom and big holes whole.
 *
 * free walks the list (O(free blocks)): fine for the programs here, which
 * keep few; big buffers can still come from VMOs of their own. A lock
 * (lock_take) makes it safe for several threads. */
#include <os.h>

#define ALIGN       16u
#define SPLIT_MIN   64u   /* don't split off remainders smaller than this */
#define MAGIC_USED  0x7573656475736564ull
#define MAGIC_FREE  0x6672656566726565ull

struct block {
    uint64_t size;    /* payload bytes, a multiple of ALIGN */
    uint64_t magic;   /* MAGIC_USED or MAGIC_FREE: catches a bad free */
    /* payload; while free its first 8 bytes link the free list */
};

static uint8_t      *heap_base, *heap_next, *heap_end;
static struct block *free_list;
static bool          heap_lock;
static int           heap_failed;

static void lock(void)
{
    lock_take(&heap_lock);
}

static void unlock(void)
{
    lock_give(&heap_lock);
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

static uint8_t *end_of(struct block *b)
{
    return (uint8_t *)(b + 1) + b->size;
}

/* b (marked free) into the list at its address, merged with the free
 * block right after it and the one right before it; a block that then
 * ends at the bump pointer goes back to it instead. */
static void release_locked(struct block *b)
{
    struct block **pp = &free_list, *prev = NULL;
    while (*pp && *pp < b) {
        prev = *pp;
        pp = next_of(*pp);
    }
    struct block *next = *pp;
    if (next && end_of(b) == (uint8_t *)next) {
        b->size += sizeof(struct block) + next->size;
        next = *next_of(next);
    }
    if (prev && end_of(prev) == (uint8_t *)b) {
        prev->size += sizeof(struct block) + b->size;
        *next_of(prev) = next;
        b = prev;
    } else {
        *next_of(b) = next;
        *pp = b;
    }
    if (end_of(b) != heap_next)
        return;
    /* The last block: unlink it (it is prev, or where b was put). */
    struct block **q = &free_list;
    while (*q != b)
        q = next_of(*q);
    *q = *next_of(b);
    heap_next = (uint8_t *)b;
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
        /* The rest takes b's place in the list: the order holds. */
        struct block *rest = (struct block *)((uint8_t *)(b + 1) + size);
        rest->size = b->size - size - sizeof(struct block);
        rest->magic = MAGIC_FREE;
        *next_of(rest) = *pp;
        *pp = rest;
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
    release_locked(b);
    unlock();
}
