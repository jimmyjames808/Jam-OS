/* The random number generator (<jam/random.h>) and its cipher
 * (<jam/chacha20.h>): RFC 8439's ChaCha20 test vectors; the generator's
 * output as a known function of its key; fast key erasure; reseeding;
 * the stuck-source check; the path without RDSEED and RDRAND; and that the
 * machine's output differs call to call and looks uniform. random_get from
 * user space is utest's (user/tests/utest/random.c). */
#include <jam/cpu.h>
#include <jam/ktest.h>
#include <jam/random.h>
#include <jam/string.h>
#include <jam/time.h>

/* The key 00 01 .. 1f of RFC 8439's 2.3.2 and 2.4.2. */
static void counting_key(uint8_t key[CHACHA20_KEY_SIZE])
{
    for (unsigned i = 0; i < CHACHA20_KEY_SIZE; i++)
        key[i] = (uint8_t)i;
}

/* RFC 8439 2.3.2 (the block function) and A.1 vectors 1 and 2 (zero key
 * and nonce, block counter 0 and 1). */
KTEST(random_chacha20_block_vectors)
{
    static const uint8_t want_232[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    static const uint8_t want_a1_1[64] = {
        0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90, 0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
        0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a, 0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
        0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d, 0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
        0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c, 0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86,
    };
    static const uint8_t want_a1_2[64] = {
        0x9f, 0x07, 0xe7, 0xbe, 0x55, 0x51, 0x38, 0x7a, 0x98, 0xba, 0x97, 0x7c, 0x73, 0x2d, 0x08, 0x0d,
        0xcb, 0x0f, 0x29, 0xa0, 0x48, 0xe3, 0x65, 0x69, 0x12, 0xc6, 0x53, 0x3e, 0x32, 0xee, 0x7a, 0xed,
        0x29, 0xb7, 0x21, 0x76, 0x9c, 0xe6, 0x4e, 0x43, 0xd5, 0x71, 0x33, 0xb0, 0x74, 0xd8, 0x39, 0xd5,
        0x31, 0xed, 0x1f, 0x28, 0x51, 0x0a, 0xfb, 0x45, 0xac, 0xe1, 0x0a, 0x1f, 0x4b, 0x79, 0x4d, 0x6f,
    };
    static const uint8_t nonce_232[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    uint8_t key[32], zero[32] = { 0 }, zero_nonce[12] = { 0 }, out[64];
    counting_key(key);
    chacha20_block(key, 1, nonce_232, out);
    KT_ASSERT(!memcmp(out, want_232, sizeof(out)));
    chacha20_block(zero, 0, zero_nonce, out);
    KT_ASSERT(!memcmp(out, want_a1_1, sizeof(out)));
    chacha20_block(zero, 1, zero_nonce, out);
    KT_ASSERT(!memcmp(out, want_a1_2, sizeof(out)));
}

/* RFC 8439 2.4.2: 114 bytes of keystream from block 1 (a whole block and
 * a cut one) XOR the RFC's plaintext give its ciphertext. */
KTEST(random_chacha20_stream_vector)
{
    static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                "only one tip for the future, sunscreen would be it.";
    static const uint8_t want[114] = {
        0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
        0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
        0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
        0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
        0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
        0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
        0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
        0x87, 0x4d,
    };
    static const uint8_t nonce[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    KT_EQ(sizeof(plain) - 1, sizeof(want));
    uint8_t key[32], ks[sizeof(want) + 1];
    counting_key(key);
    ks[sizeof(want)] = 0xa5;   /* the stream must stop at its length */
    chacha20_stream(key, 1, nonce, ks, sizeof(want));
    KT_EQ(ks[sizeof(want)], 0xa5);
    for (unsigned i = 0; i < sizeof(want); i++)
        KT_EQ(ks[i] ^ (uint8_t)plain[i], want[i]);
}

/* A test generator: seeded without the hardware (fast, and the same on
 * every machine), never due a reseed unless the test says so. */
static void test_state(struct random_state *s)
{
    random_state_init(s, NULL, RANDOM_NO_RDSEED | RANDOM_NO_RDRAND);
    s->next_reseed = UINT64_MAX;
}

/* A request is ChaCha20 of the key with the nonce "random out": the
 * block's first half is the next key, its second half keys the stream.
 * The old key is gone afterwards. */
KTEST(random_output_is_the_construction)
{
    static const uint8_t nonce_out[12] = "random out";
    struct random_state s;
    test_state(&s);
    uint8_t key[32], block[64], want[100], got[100];
    memcpy(key, s.key, sizeof(key));
    chacha20_block(key, 0, nonce_out, block);
    chacha20_stream(block + 32, 0, nonce_out, want, sizeof(want));
    random_state_bytes(&s, got, sizeof(got));
    KT_ASSERT(!memcmp(got, want, sizeof(got)));
    KT_ASSERT(!memcmp(s.key, block, 32));      /* fast key erasure */
    KT_ASSERT(memcmp(s.key, key, sizeof(key)));
    KT_EQ(s.reseeds, 0);
}

/* Mixing changes the key; the same data into the same key gives the same
 * key (it is a function, not a counter). */
KTEST(random_mix_is_a_function)
{
    struct random_state a, b;
    test_state(&a);
    test_state(&b);
    memcpy(b.key, a.key, sizeof(a.key));
    uint8_t before[32];
    memcpy(before, a.key, sizeof(before));
    static const char data[] = "forty bytes of data, more than one piece";
    random_state_mix(&a, data, sizeof(data));
    random_state_mix(&b, data, sizeof(data));
    KT_ASSERT(memcmp(a.key, before, sizeof(before)));
    KT_ASSERT(!memcmp(a.key, b.key, sizeof(a.key)));
    random_state_mix(&b, "x", 1);
    KT_ASSERT(memcmp(a.key, b.key, sizeof(a.key)));
}

/* The first request after next_reseed reseeds once and sets the next. */
KTEST(random_reseeds_when_due)
{
    struct random_state s;
    random_state_init(&s, NULL, 0);   /* with the hardware, if this CPU has it */
    s.next_reseed = 0;
    uint8_t key[32], buf[16];
    memcpy(key, s.key, sizeof(key));
    uint64_t before = uptime_ns();
    random_state_bytes(&s, buf, sizeof(buf));
    KT_EQ(s.reseeds, 1);
    KT_ASSERT(s.next_reseed >= before + RANDOM_RESEED_NS);
    random_state_bytes(&s, buf, sizeof(buf));
    KT_EQ(s.reseeds, 1);
    KT_ASSERT(memcmp(s.key, key, sizeof(key)));
    KT_EQ(s.hw, s.source);   /* a working source stays in use */
}

KTEST(random_stuck_source_check)
{
    uint64_t w[4] = { 0x0123456789abcdefull, 0xfedcba9876543210ull, 0x1111222233334444ull,
                      0x5555666677778888ull };
    KT_ASSERT(random_hw_words_ok(w, 4));
    uint64_t same[4] = { 7, 7, 7, 7 };
    KT_ASSERT(!random_hw_words_ok(same, 4));
    w[3] = w[0];                                   /* a repeat, not next to it */
    KT_ASSERT(!random_hw_words_ok(w, 4));
    w[3] = UINT64_MAX;                             /* the all-ones failure */
    KT_ASSERT(!random_hw_words_ok(w, 4));
    w[3] = 0;
    KT_ASSERT(!random_hw_words_ok(w, 4));
}

/* What the machine's generator was seeded from follows CPUID, unless the
 * hardware failed its check (then the boot log said so). */
KTEST(random_machine_source)
{
    enum random_source want = cpu_features.rdseed   ? RANDOM_RDSEED
                              : cpu_features.rdrand ? RANDOM_RDRAND
                                                    : RANDOM_TIMING;
    struct random_state s;
    random_state_init(&s, NULL, 0);
    if (!s.hw_problem) {
        KT_EQ(s.source, want);
        KT_EQ(random_source(), want);
    }
    if (cpu_features.rdrand) {   /* RDRAND alone, as on a CPU without RDSEED */
        random_state_init(&s, NULL, RANDOM_NO_RDSEED);
        KT_ASSERT(s.source == RANDOM_RDRAND || s.hw_problem);
    }
}

/* Without RDSEED and RDRAND (as on QEMU's qemu64 CPU): timings alone, and
 * still two generators never agree. */
KTEST(random_without_hardware)
{
    struct random_state a, b;
    test_state(&a);
    test_state(&b);
    KT_EQ(a.source, RANDOM_TIMING);
    KT_EQ(a.hw, RANDOM_TIMING);
    KT_ASSERT(a.hw_problem == NULL);
    KT_ASSERT(a.timings_distinct >= 1 && a.timings_distinct <= 256);
    KT_ASSERT(memcmp(a.key, b.key, sizeof(a.key)));   /* the TSC moved on */
    uint64_t x, y;
    random_state_bytes(&a, &x, sizeof(x));
    random_state_bytes(&b, &y, sizeof(y));
    KT_ASSERT(x != y);
    a.next_reseed = 0;   /* a reseed without hardware: the TSC alone */
    random_state_bytes(&a, &x, sizeof(x));
    KT_EQ(a.reseeds, 1);
}

KTEST(random_calls_differ)
{
    uint8_t a[32], b[32];
    random_bytes(a, sizeof(a));
    random_bytes(b, sizeof(b));
    KT_ASSERT(memcmp(a, b, sizeof(a)));
    KT_ASSERT(random_u64() != random_u64());
    KT_ASSERT(random_source() != RANDOM_NONE);
}

static unsigned ones8(uint8_t v)
{
    unsigned n = 0;
    for (; v; v &= (uint8_t)(v - 1))
        n++;
    return n;
}

/* 64 KiB from the machine's generator: a chi-square over the byte values
 * (255 degrees of freedom: mean 255, sd 22.6) and the count of one bits
 * (mean 262144, sd 362). Each bound is 6 to 7 sd out: a working generator
 * fails one about once in 10^9 runs; a broken one (a counter, a stuck
 * byte, a bias) fails at once. */
KTEST(random_bytes_look_uniform)
{
    uint16_t count[256] = { 0 };
    uint8_t buf[256];
    uint64_t ones = 0;
    for (unsigned i = 0; i < 256; i++) {
        random_bytes(buf, sizeof(buf));
        for (unsigned j = 0; j < sizeof(buf); j++) {
            count[buf[j]]++;
            ones += ones8(buf[j]);
        }
    }
    uint64_t sum = 0;
    for (unsigned k = 0; k < 256; k++) {
        int64_t d = (int64_t)count[k] - 256;
        sum += (uint64_t)(d * d);
    }
    uint64_t chi2 = sum / 256;
    if (chi2 <= 120 || chi2 >= 420 || ones <= 262144 - 2600 || ones >= 262144 + 2600)
        ktest_fail("chi-square %lu (want 120-420), %lu one bits (want 262144 +- 2600)", chi2,
                   ones);
}
