/* ext: the block cache. Every metadata block the library reads, and the
 * partial blocks of file data, are read into a buffer here; the buffers
 * are carved from the one region the mounter gives ext_mount, nothing is
 * allocated.
 *
 * The region: the buffer records, then the hash chains (a power of two of
 * them, at least as many as buffers), then each buffer's block, each
 * followed by a guard of GUARD bytes. Built with AddressSanitizer (the
 * Mac's tests), the guards are poisoned, so a parser that reads or writes
 * past a block's end is caught even though the next block is the same
 * allocation.
 *
 * A miss takes the least recently used buffer that is neither pinned nor
 * dirty; if every buffer is one or the other, the call fails with
 * ERR_NO_RESOURCES and nothing changes. A pinned buffer's data never moves
 * and is never reused, so a pointer into it stays good until ext_brelse.
 * Dirty buffers are written home only by ext_sync, in block order, and
 * only to a volume that may be written: the cache is where a transaction
 * collects (U3's journal commits what ext_sync would write). */
#include "ext_int.h"

#define GUARD 64u   /* bytes after each block: poisoned under ASan */

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EXT_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define EXT_ASAN 1
#endif
#ifdef EXT_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define POISON(p, n)   __asan_poison_memory_region((p), (n))
#define UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define POISON(p, n)   ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

/* The chains for n buffers: the power of two at least n. */
static uint32_t chains_for(uint32_t n)
{
    uint32_t c = 1;
    while (c < n)
        c <<= 1;
    return c;
}

/* 8 bytes to align the records, 64 to align the blocks. */
#define SLACK 72u

size_t ext_mem_size(uint32_t nbufs, uint32_t block_size)
{
    return SLACK + (size_t)nbufs * (sizeof(struct ext_buf) + block_size + GUARD) +
           (size_t)chains_for(nbufs) * sizeof(uint32_t);
}

/* Carve the cache for fs->block_size from mem: as many buffers as fit. */
status_t ext_cache_init(struct ext_fs *fs, void *mem, size_t mem_size)
{
    struct ext_cache *c = &fs->cache;
    uint32_t bs = fs->block_size;
    UNPOISON(mem, mem_size);
    size_t per = sizeof(struct ext_buf) + bs + GUARD;   /* the chains take a few more bytes */
    size_t n = mem_size > SLACK ? (mem_size - SLACK) / per : 0;
    if (n > UINT32_MAX / 2)
        n = UINT32_MAX / 2;
    while (n >= EXT_CACHE_MIN && ext_mem_size((uint32_t)n, bs) > mem_size)
        n--;
    if (n < EXT_CACHE_MIN)
        return ERR_NO_MEMORY;
    __builtin_memset(c, 0, sizeof(*c));
    uintptr_t p = ((uintptr_t)mem + 7) & ~(uintptr_t)7;
    c->nbufs = (uint32_t)n;
    c->bufs = (struct ext_buf *)p;
    p += n * sizeof(struct ext_buf);
    uint32_t chains = chains_for((uint32_t)n);
    c->heads = (uint32_t *)p;
    p += chains * sizeof(uint32_t);
    c->hash_shift = 64 - (uint32_t)__builtin_ctz(chains);
    p = (p + 63) & ~(uintptr_t)63;
    __builtin_memset(c->heads, 0, chains * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) {
        struct ext_buf *b = &c->bufs[i];
        __builtin_memset(b, 0, sizeof(*b));
        b->blk = EXT_NO_BLOCK;
        b->data = (uint8_t *)p;
        POISON(b->data + bs, GUARD);
        p += bs + GUARD;
    }
    return OK;
}

static uint32_t *chain(struct ext_cache *c, uint64_t blk)
{
    if (c->hash_shift >= 64)   /* one chain */
        return &c->heads[0];
    return &c->heads[(blk * 0x9E3779B97F4A7C15ull) >> c->hash_shift];
}

static struct ext_buf *lookup(struct ext_cache *c, uint64_t blk)
{
    for (uint32_t i = *chain(c, blk); i; i = c->bufs[i - 1].next)
        if (c->bufs[i - 1].blk == blk)
            return &c->bufs[i - 1];
    return NULL;
}

static void unhash(struct ext_cache *c, struct ext_buf *b)
{
    uint32_t me = (uint32_t)(b - c->bufs) + 1;
    for (uint32_t *at = chain(c, b->blk); *at; at = &c->bufs[*at - 1].next)
        if (*at == me) {
            *at = b->next;
            break;
        }
    b->next = 0;
    b->blk = EXT_NO_BLOCK;
    b->kind = EXT_KIND_NONE;
}

static void hash(struct ext_cache *c, struct ext_buf *b, uint64_t blk)
{
    uint32_t *head = chain(c, blk);
    b->blk = blk;
    b->next = *head;
    *head = (uint32_t)(b - c->bufs) + 1;
}

/* The buffer to reuse: unpinned, clean, least recently used; NULL if none. */
static struct ext_buf *victim(struct ext_cache *c)
{
    struct ext_buf *best = NULL;
    for (uint32_t i = 0; i < c->nbufs; i++) {
        struct ext_buf *b = &c->bufs[i];
        if (b->pins || b->dirty)
            continue;
        if (b->blk == EXT_NO_BLOCK)
            return b;
        if (!best || b->used < best->used)
            best = b;
    }
    return best;
}

/* Block blk pinned, read from the device on a miss unless `fresh` (then zeros). */
static status_t get(struct ext_fs *fs, uint64_t blk, bool fresh, struct ext_buf **out)
{
    struct ext_cache *c = &fs->cache;
    if (blk >= fs->blocks)
        return ERR_OUT_OF_RANGE;
    struct ext_buf *b = lookup(c, blk);
    if (b) {
        if (fresh)
            __builtin_memset(b->data, 0, fs->block_size);
    } else {
        b = victim(c);
        if (!b)
            return ERR_NO_RESOURCES;
        if (b->blk != EXT_NO_BLOCK)
            unhash(c, b);
        if (fresh) {
            __builtin_memset(b->data, 0, fs->block_size);
        } else {
            c->reads++;
            status_t st = fs->dev.read(fs->dev.ctx, blk << fs->block_log, fs->block_size, b->data);
            if (st != OK)
                return ERR_IO;
        }
        hash(c, b, blk);
    }
    if (fresh)
        b->kind = EXT_KIND_NONE;
    b->pins++;
    b->used = ++c->clock;
    *out = b;
    return OK;
}

status_t ext_bread(struct ext_fs *fs, uint64_t blk, struct ext_buf **out)
{
    return get(fs, blk, false, out);
}

status_t ext_bnew(struct ext_fs *fs, uint64_t blk, struct ext_buf **out)
{
    return get(fs, blk, true, out);
}

void ext_brelse(struct ext_fs *fs, struct ext_buf *b)
{
    (void)fs;
    if (b && b->pins)
        b->pins--;
}

void ext_bdirty(struct ext_fs *fs, struct ext_buf *b)
{
    if (!b->dirty)
        fs->cache.ndirty++;
    b->dirty = true;
    b->kind = EXT_KIND_NONE;
}

void ext_binval(struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    struct ext_cache *c = &fs->cache;
    for (uint32_t i = 0; i < c->nbufs; i++) {
        struct ext_buf *b = &c->bufs[i];
        if (b->blk != EXT_NO_BLOCK && b->blk >= blk && b->blk - blk < count && !b->pins &&
            !b->dirty)
            unhash(c, b);
    }
}

void ext_bforget(struct ext_fs *fs, uint64_t blk, uint64_t count)
{
    struct ext_cache *c = &fs->cache;
    for (uint32_t i = 0; i < c->nbufs; i++) {
        struct ext_buf *b = &c->bufs[i];
        if (b->blk == EXT_NO_BLOCK || b->blk < blk || b->blk - blk >= count || b->pins)
            continue;
        if (b->dirty) {
            b->dirty = false;
            c->ndirty--;
        }
        unhash(c, b);
    }
}

/* The dirty buffer with the lowest block above `after` (or any, when
 * `first`); NULL if none. */
static struct ext_buf *next_dirty(struct ext_cache *c, uint64_t after, bool first)
{
    struct ext_buf *best = NULL;
    for (uint32_t i = 0; i < c->nbufs; i++) {
        struct ext_buf *b = &c->bufs[i];
        if (b->dirty && (first || b->blk > after) && (!best || b->blk < best->blk))
            best = b;
    }
    return best;
}

status_t ext_sync(struct ext_fs *fs)
{
    struct ext_cache *c = &fs->cache;
    if (!c->ndirty)
        return OK;
    if (fs->ro || !fs->dev.write)
        return ERR_ACCESS_DENIED;
    bool first = true;
    uint64_t after = 0;
    for (struct ext_buf *b; (b = next_dirty(c, after, first)); first = false) {
        c->writes++;
        if (fs->dev.write(fs->dev.ctx, b->blk << fs->block_log, fs->block_size, b->data) != OK)
            return ERR_IO;
        b->dirty = false;
        c->ndirty--;
        after = b->blk;
    }
    return OK;
}

void ext_discard(struct ext_fs *fs)
{
    struct ext_cache *c = &fs->cache;
    for (uint32_t i = 0; i < c->nbufs; i++) {
        struct ext_buf *b = &c->bufs[i];
        if (!b->dirty)
            continue;
        b->dirty = false;
        unhash(c, b);   /* a holder's pointer stays good until its ext_brelse */
    }
    c->ndirty = 0;
}

uint32_t ext_cache_pins(const struct ext_fs *fs)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < fs->cache.nbufs; i++)
        n += fs->cache.bufs[i].pins;
    return n;
}
