/* The kernel's random number generator (<jam/random.h> has the model):
 * a ChaCha20 key with fast key erasure, seeded at boot from RDSEED or
 * RDRAND (when CPUID has them and they pass a check), TSC timings and boot
 * data, and reseeded from the hardware every RANDOM_RESEED_NS by the first
 * request after that time.
 *
 * Lock: one per generator, "random key", a leaf (nothing is taken under
 * it), always with interrupts off. It guards only the key. The other
 * fields are set once before the generator is shared, or are atomics. */
#include <jam/acpi.h>
#include <jam/boot.h>
#include <jam/cpu.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/random.h>
#include <jam/report.h>
#include <jam/string.h>
#include <jam/wallclock.h>
#include <jam/x86.h>

/* Mixing and requests draw blocks from the key under different nonces, so
 * no block serves both. */
static const uint8_t NONCE_MIX[CHACHA20_NONCE_SIZE] = "random mix";
static const uint8_t NONCE_OUT[CHACHA20_NONCE_SIZE] = "random out";

/* Tries per hardware word. Each try is one instruction that answers at
 * once, with a value or with "none ready" (CF = 0); there is no device to
 * wait on, so the bound is a count. Intel's DRNG guide (5.2.1) retries
 * RDRAND 10 times; RDSEED runs dry under load and gets more, with a pause
 * between tries. A reseed may run with interrupts off, so the worst case
 * (every word coming on its last try) stays near a millisecond; a source
 * that is dry just skips that reseed. */
#define RDRAND_TRIES 10
#define RDSEED_TRIES 100

/* Timings are taken in rounds of this many (512 bytes of stack each). */
#define TIMING_ROUND 64

static struct random_state machine;

enum hw_result { HW_OK, HW_DRY, HW_STUCK };

static bool rdrand64(uint64_t *v)
{
    bool ok;
    __asm__ volatile("rdrand %0" : "=r"(*v), "=@ccc"(ok));
    return ok;
}

static bool rdseed64(uint64_t *v)
{
    bool ok;
    __asm__ volatile("rdseed %0" : "=r"(*v), "=@ccc"(ok));
    return ok;
}

/* One word from src (RANDOM_RDSEED or RANDOM_RDRAND); false if every try
 * came back empty. */
static bool hw_word(enum random_source src, uint64_t *out)
{
    unsigned tries = src == RANDOM_RDSEED ? RDSEED_TRIES : RDRAND_TRIES;
    for (unsigned i = 0; i < tries; i++) {
        if (src == RANDOM_RDSEED ? rdseed64(out) : rdrand64(out))
            return true;
        cpu_relax();
    }
    return false;
}

static enum hw_result hw_words(enum random_source src, uint64_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (!hw_word(src, &w[i]))
            return HW_DRY;
    return random_hw_words_ok(w, n) ? HW_OK : HW_STUCK;
}

bool random_hw_words_ok(const uint64_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (w[i] == 0 || w[i] == UINT64_MAX)
            return false;
        for (unsigned j = 0; j < i; j++)
            if (w[i] == w[j])
                return false;
    }
    return true;
}

/* Mix len bytes into s->key, 32 at a time (the last piece as long as it
 * is): key = ChaCha20(key, NONCE_MIX) XOR piece. s->lock held, or s not
 * shared yet. */
static void mix_locked(struct random_state *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t block[CHACHA20_BLOCK_SIZE];
    while (len) {
        size_t n = len < CHACHA20_KEY_SIZE ? len : CHACHA20_KEY_SIZE;
        chacha20_block(s->key, 0, NONCE_MIX, block);
        for (size_t i = 0; i < n; i++)
            block[i] ^= p[i];
        memcpy(s->key, block, CHACHA20_KEY_SIZE);
        p += n;
        len -= n;
    }
    explicit_bzero(block, sizeof(block));
}

/* n TSC readings into w, each taken after a CPUID (serialising, and a
 * trip to the hypervisor in a VM) and a read at an address the previous
 * reading picks. The low byte of each delta is marked in seen (a 256-bit
 * map): how many differ hints at how much the timings vary. */
static void timings(uint64_t *w, unsigned n, uint8_t seen[32])
{
    volatile const uint64_t *probe = w;
    uint64_t prev = rdtsc();
    for (unsigned i = 0; i < n; i++) {
        uint32_t a, b, c, d;
        cpuid(0, 0, &a, &b, &c, &d);
        (void)probe[prev % n];
        uint64_t t = rdtsc();
        uint8_t low = (uint8_t)(t - prev);
        seen[low / 8] |= (uint8_t)(1u << (low % 8));
        w[i] = t;
        prev = t;
    }
}

/* RANDOM_TIMINGS timings mixed into s; returns how many distinct delta
 * low bytes they had. */
static unsigned mix_timings(struct random_state *s)
{
    uint64_t w[TIMING_ROUND] = { 0 };
    uint8_t seen[32] = { 0 };
    for (unsigned r = 0; r < RANDOM_TIMINGS / TIMING_ROUND; r++) {
        timings(w, TIMING_ROUND, seen);
        mix_locked(s, w, sizeof(w));
    }
    unsigned distinct = 0;
    for (unsigned i = 0; i < 256; i++)
        distinct += seen[i / 8] >> (i % 8) & 1;
    return distinct;
}

/* What differs between machines and between boots: none of it secret from
 * someone with the machine, but it keeps two machines (or two boots with
 * the same timings) apart. */
static void mix_boot_data(struct random_state *s, const struct boot_info *bi)
{
    size_t nmem = bi->memmap_count < BOOT_MAX_MEMMAP ? bi->memmap_count : BOOT_MAX_MEMMAP;
    size_t ncpu = bi->cpu_count < BOOT_MAX_CPUS ? bi->cpu_count : BOOT_MAX_CPUS;
    mix_locked(s, bi->memmap, nmem * sizeof(bi->memmap[0]));
    mix_locked(s, bi->cpus, ncpu * sizeof(bi->cpus[0]));
    mix_locked(s, bi->cmdline, strlen(bi->cmdline));
    mix_locked(s, &acpi, sizeof(acpi));
    mix_locked(s, &cpu_features, sizeof(cpu_features));
    struct wall_clock wc;
    if (wallclock_get(&wc) == OK)
        mix_locked(s, &wc, sizeof(wc));
}

static const char *hw_problem(enum random_source src, enum hw_result r)
{
    if (src == RANDOM_RDSEED)
        return r == HW_DRY ? "RDSEED never gave a value" : "RDSEED repeated a value";
    return r == HW_DRY ? "RDRAND never gave a value" : "RDRAND repeated a value";
}

/* The seed's hardware words: RDSEED, else RDRAND, each only if CPUID has
 * it, flags allow it and its words pass the check. */
static void seed_hw(struct random_state *s, unsigned flags)
{
    uint64_t w[RANDOM_SEED_WORDS];
    static const struct {
        enum random_source src;
        unsigned           off;   /* the flag that leaves it out */
    } order[] = { { RANDOM_RDSEED, RANDOM_NO_RDSEED }, { RANDOM_RDRAND, RANDOM_NO_RDRAND } };
    for (unsigned i = 0; i < 2 && s->source == RANDOM_TIMING; i++) {
        bool has = order[i].src == RANDOM_RDSEED ? cpu_features.rdseed : cpu_features.rdrand;
        if (!has || (flags & order[i].off))
            continue;
        enum hw_result r = hw_words(order[i].src, w, RANDOM_SEED_WORDS);
        if (r == HW_OK) {
            mix_locked(s, w, sizeof(w));
            s->source = order[i].src;
        } else if (!s->hw_problem) {
            s->hw_problem = hw_problem(order[i].src, r);
        }
    }
    explicit_bzero(w, sizeof(w));
}

void random_state_init(struct random_state *s, const struct boot_info *bi, unsigned flags)
{
    memset(s, 0, sizeof(*s));
    spin_init(&s->lock, "random key");
    s->timings_distinct = mix_timings(s);
    if (bi)
        mix_boot_data(s, bi);
    s->source = RANDOM_TIMING;
    seed_hw(s, flags);
    uint64_t tsc = rdtsc();
    mix_locked(s, &tsc, sizeof(tsc));
    s->hw = s->source;
    s->next_reseed = uptime_ns() + RANDOM_RESEED_NS;
}

/* The first request after next_reseed mixes in fresh hardware words (if
 * the generator has a source) and the TSC; the others go on meanwhile.
 * A dry source is skipped this time; a stuck one is dropped for good. */
static void reseed_if_due(struct random_state *s)
{
    uint64_t now = uptime_ns();
    uint64_t due = __atomic_load_n(&s->next_reseed, __ATOMIC_RELAXED);
    if (now < due || !__atomic_compare_exchange_n(&s->next_reseed, &due,
                                                  now + RANDOM_RESEED_NS, false,
                                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    uint64_t fresh[RANDOM_SEED_WORDS + 2];
    unsigned n = 0;
    enum random_source hw = __atomic_load_n(&s->hw, __ATOMIC_RELAXED);
    if (hw != RANDOM_TIMING) {
        enum hw_result r = hw_words(hw, fresh, RANDOM_SEED_WORDS);
        if (r == HW_OK)
            n = RANDOM_SEED_WORDS;
        if (r == HW_STUCK) {
            __atomic_store_n(&s->hw, RANDOM_TIMING, __ATOMIC_RELAXED);
            report("random: %s at a reseed: reseeds use the TSC alone from now on",
                   hw_problem(hw, r));
        }
    }
    fresh[n++] = rdtsc();
    fresh[n++] = now;
    uint64_t f = spin_lock_irqsave(&s->lock);
    mix_locked(s, fresh, n * sizeof(fresh[0]));
    spin_unlock_irqrestore(&s->lock, f);
    explicit_bzero(fresh, sizeof(fresh));
    __atomic_add_fetch(&s->reseeds, 1, __ATOMIC_RELAXED);
}

void random_state_bytes(struct random_state *s, void *buf, size_t len)
{
    if (s->source == RANDOM_NONE)
        panic("random: a generator used before it was seeded");
    reseed_if_due(s);
    uint8_t block[CHACHA20_BLOCK_SIZE];
    uint64_t f = spin_lock_irqsave(&s->lock);
    chacha20_block(s->key, 0, NONCE_OUT, block);
    memcpy(s->key, block, CHACHA20_KEY_SIZE);   /* the old key is gone */
    spin_unlock_irqrestore(&s->lock, f);
    /* The block's other half keys this request's stream, which nobody
     * else will ever use. */
    chacha20_stream(block + CHACHA20_KEY_SIZE, 0, NONCE_OUT, buf, len);
    explicit_bzero(block, sizeof(block));
}

void random_state_mix(struct random_state *s, const void *data, size_t len)
{
    uint64_t f = spin_lock_irqsave(&s->lock);
    mix_locked(s, data, len);
    spin_unlock_irqrestore(&s->lock, f);
}

const char *random_source_name(enum random_source src)
{
    switch (src) {
    case RANDOM_NONE:   return "nothing";
    case RANDOM_TIMING: return "timing only";
    case RANDOM_RDRAND: return "RDRAND";
    case RANDOM_RDSEED: return "RDSEED";
    }
    return "?";
}

void random_init(const struct boot_info *bi)
{
    random_state_init(&machine, bi, 0);
    if (machine.hw_problem)
        report("random: %s: not used", machine.hw_problem);
    if (machine.source == RANDOM_TIMING)
        report("random: %s: seeded from timing only (%u of %u timings distinct): WEAK",
               cpu_features.rdseed || cpu_features.rdrand ? "RDSEED/RDRAND unusable"
                                                          : "no RDSEED/RDRAND",
               machine.timings_distinct, RANDOM_TIMINGS);
    else
        kprintf("random:      seeded from %s, %u TSC timings (%u distinct) and boot data; "
                "reseeds from %s every %lu s\n", random_source_name(machine.source),
                RANDOM_TIMINGS, machine.timings_distinct, random_source_name(machine.source),
                (uint64_t)(RANDOM_RESEED_NS / NS_PER_S));
}

void random_bytes(void *buf, size_t len)
{
    random_state_bytes(&machine, buf, len);
}

uint64_t random_u64(void)
{
    uint64_t v;
    random_bytes(&v, sizeof(v));
    return v;
}

enum random_source random_source(void)
{
    return machine.source;
}
