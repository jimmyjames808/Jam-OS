/* The kernel's random numbers (kernel/dev/random.c), for itself and for
 * user space (the random_get system call, libos's os_random).
 *
 * One generator for the whole machine, built like OpenBSD's arc4random and
 * Linux's crng: its whole state is a 256-bit ChaCha20 key (<jam/chacha20.h>).
 * A request turns the key into one ChaCha20 block under the lock: the
 * block's first 32 bytes are the next key, its last 32 the key of the
 * request's own keystream, made after the lock is dropped. The old key is
 * gone when the lock is ("fast key erasure"), so memory read later can't
 * give back what was handed out before.
 *
 * Mixing data into the key, 32 bytes at a time: the key's next block XOR
 * the data becomes the key (arc4random's rekey). Mixing and requests use
 * different nonces. Mixing is not entropy counting: data no attacker knows
 * makes the key unknown; data an attacker knows does no harm.
 *
 * The seed (random_init, once the TSC is calibrated): RDSEED, else RDRAND,
 * each only if CPUID has it, drawn with a retry limit and checked for a
 * stuck source (random_hw_words_ok); plus TSC timings around CPUID and
 * memory reads, and what differs between machines and boots (the loader's
 * memory map, CPUs and command line, ACPI's tables as parsed, the RTC's
 * date, the CPU's identity). With neither instruction (QEMU's qemu64 CPU)
 * the timings and boot data are all there is, and the log says loudly
 * that the numbers are weak. A request that finds RANDOM_RESEED_NS gone
 * since the last reseed first mixes in fresh words from the hardware and
 * the TSC.
 *
 * Context: any CPU, any context, interrupts off included. Nothing waits
 * for entropy: the lock (class "random key", taken with interrupts off) is
 * held for one or a few ChaCha20 blocks, and a reseed's hardware draws
 * happen before it is taken. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/chacha20.h>
#include <jam/spinlock.h>
#include <jam/time.h>

struct boot_info;

/* The best source a generator's seed came from. */
enum random_source {
    RANDOM_NONE,     /* not seeded: using it is a kernel bug */
    RANDOM_TIMING,   /* TSC timings and boot data only: weak */
    RANDOM_RDRAND,   /* RDRAND (a DRBG the CPU reseeds itself), plus the above */
    RANDOM_RDSEED,   /* RDSEED (the CPU's conditioned entropy), plus the above */
};

#define RANDOM_RESEED_NS (60 * NS_PER_S)   /* as Linux's crng */
#define RANDOM_SEED_WORDS 8                /* 64-bit hardware words in a seed or reseed */
#define RANDOM_TIMINGS    256              /* TSC samples in the seed */

/* A generator. The machine has one (random_bytes); tests make their own. */
struct random_state {
    spinlock_t         lock;                      /* guards key */
    uint8_t            key[CHACHA20_KEY_SIZE];    /* replaced on every use; lock */
    enum random_source source;                    /* the seed's best source; set once */
    enum random_source hw;                        /* reseeds draw from this (RANDOM_TIMING:
                                                   * none); atomic, a failed check lowers it */
    uint64_t           next_reseed;               /* uptime of the next reseed; atomic */
    uint64_t           reseeds;                   /* reseeds done; atomic */
    unsigned           timings_distinct;          /* distinct low bytes of the seed's TSC
                                                   * deltas (of RANDOM_TIMINGS): a hint */
    const char        *hw_problem;                /* why a hardware source was refused at
                                                   * seeding, or NULL; set once */
};

/* random_state_init flags: leave an instruction out as if CPUID lacked it. */
#define RANDOM_NO_RDSEED (1u << 0)
#define RANDOM_NO_RDRAND (1u << 1)

/* Seed s from scratch: the hardware (unless flags leave it out), timings
 * and, with bi, the boot data. Says nothing in the log. Interrupts on, s
 * not yet shared with another CPU. */
void random_state_init(struct random_state *s, const struct boot_info *bi, unsigned flags);
/* len bytes from s (reseeding it first when due). Any context. */
void random_state_bytes(struct random_state *s, void *buf, size_t len);
/* Mix len bytes of data into s's key. Any context. */
void random_state_mix(struct random_state *s, const void *data, size_t len);

/* At boot, once the TSC is calibrated and ACPI and the wall clock are
 * read: seed the machine's generator and say in the log from what. */
void random_init(const struct boot_info *bi);
/* len bytes from the machine's generator. Any context, after random_init
 * (before it: a panic). */
void random_bytes(void *buf, size_t len);
uint64_t random_u64(void);
/* The machine generator's seed source. */
enum random_source random_source(void);
const char *random_source_name(enum random_source src);

/* n words drawn from RDSEED or RDRAND look like a working source: no two
 * of them equal, none all zeros or all ones (a stuck or failed source
 * repeats a value; some AMD parts returned all ones with success). A
 * working source fails this with odds around n^2 / 2^64. */
bool random_hw_words_ok(const uint64_t *w, unsigned n);
