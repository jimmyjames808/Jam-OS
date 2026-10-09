/* Small ids handed out in turn (<jam/idspace.h> has the rules): a bitmap
 * of the values in use, a second of the values given back but held until
 * the cursor wraps, and the cursor.
 *
 * Why a value given back ahead of the cursor is held: the cursor reaches
 * it later in the same lap, so without the hold it could be handed out
 * again moments after it was given back. Behind the cursor it needs no
 * hold: the cursor reaches it only after wrapping.
 *
 * Cost: a search starts at the cursor and skips whole words in use, so
 * it reads about (values in use ahead of it) / 64 words; the wrap reads
 * the held bitmap once (65,536 words at most) to free what it holds. */
#include <jam/atomic.h>
#include <jam/idspace.h>
#include <jam/panic.h>
#include <jam/string.h>

#define ID_WORDS (IDS_MAX / 64)

/* Zeroed (.bss, not 1 MiB of the kernel's file). Bit 0, the value 0, is
 * never handed out all the same: every search starts at 1 or later. */
static uint64_t pid_used[ID_WORDS], pid_held[ID_WORDS];
static uint64_t tid_used[ID_WORDS], tid_held[ID_WORDS];
struct idspace pid_space = {
    .lock = SPINLOCK_INIT("pid space"), .used = pid_used, .held = pid_held, .max = IDS_MAX,
    .next = 1,
};
struct idspace tid_space = {
    .lock = SPINLOCK_INIT("tid space"), .used = tid_used, .held = tid_held, .max = IDS_MAX,
    .next = 1,
};

void idspace_init(struct idspace *s, const char *name, uint64_t *used, uint64_t *held,
                  uint32_t words)
{
    memset(s, 0, sizeof(*s));
    spin_init(&s->lock, name);
    memset(used, 0, words * sizeof(uint64_t));
    memset(held, 0, words * sizeof(uint64_t));
    s->used = used;
    s->held = held;
    s->max = words * 64;
    s->next = 1;
}

/* Lock held, from >= 1: the first value in [from, s->max) not in use, or
 * 0 (never a value). */
static uint32_t find_free(const struct idspace *s, uint32_t from)
{
    uint32_t w = from / 64;
    uint64_t bits = s->used[w] | ((1ull << (from % 64)) - 1);   /* below `from`: skip */
    for (;;) {
        if (bits != ~0ull)
            return w * 64 + (uint32_t)__builtin_ctzll(~bits);
        if (++w == s->max / 64)
            return 0;
        bits = s->used[w];
    }
}

/* Lock held: the cursor wraps; what was held for the lap is free now. */
static void wrap(struct idspace *s)
{
    for (uint32_t w = 0; s->nheld && w < s->max / 64; w++) {
        uint64_t h = s->held[w];
        if (!h)
            continue;
        s->used[w] &= ~h;
        s->held[w] = 0;
        for (; h; h &= h - 1)   /* one per bit (no libgcc for popcount) */
            s->nheld--;
    }
    s->next = 1;
    s->laps++;
}

status_t idspace_alloc(struct idspace *s, uint32_t *out)
{
    uint64_t f = spin_lock_irqsave(&s->lock);
    uint32_t v = s->next < s->max ? find_free(s, s->next) : 0;
    if (!v) {
        wrap(s);
        v = find_free(s, 1);
    }
    if (v) {
        s->used[v / 64] |= 1ull << (v % 64);
        s->next = v + 1;
        COUNTER_ADD(&s->live, 1);   /* read without the lock */
    }
    spin_unlock_irqrestore(&s->lock, f);
    if (!v)
        return ERR_NO_RESOURCES;
    *out = v;
    return OK;
}

void idspace_free(struct idspace *s, uint32_t v)
{
    uint64_t f = spin_lock_irqsave(&s->lock);
    uint64_t bit = 1ull << (v % 64);
    if (!v || v >= s->max || !(s->used[v / 64] & bit) || (s->held[v / 64] & bit))
        panic("%s: freeing %u, which is not taken", s->lock.name, v);
    if (v >= s->next) {
        s->held[v / 64] |= bit;   /* the cursor would reach it this lap: hold it */
        s->nheld++;
    } else {
        s->used[v / 64] &= ~bit;
    }
    COUNTER_SUB(&s->live, 1);
    spin_unlock_irqrestore(&s->lock, f);
}

uint32_t idspace_live(struct idspace *s)
{
    return __atomic_load_n(&s->live, __ATOMIC_RELAXED);
}
