/* ChaCha20 (<jam/chacha20.h>), written from RFC 8439 section 2: the state
 * is 16 little-endian words (4 constants, 8 of key, the block counter, 3
 * of nonce); 20 rounds, alternately on its columns and its diagonals; the
 * block is the result plus the starting state. Every intermediate copy of
 * the key or keystream on the stack is cleared before returning. */
#include <jam/chacha20.h>
#include <jam/string.h>

static inline uint32_t rotl32(uint32_t v, unsigned n)
{
    return v << n | v >> (32 - n);
}

static inline uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* RFC 8439 2.1: the quarter round on four words of the state. */
#define QR(x, a, b, c, d)                                     \
    do {                                                      \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 16);  \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 12);  \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = rotl32(x[d], 8);   \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = rotl32(x[b], 7);   \
    } while (0)

void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[CHACHA20_NONCE_SIZE], uint8_t out[CHACHA20_BLOCK_SIZE])
{
    /* "expand 32-byte k" (RFC 8439 2.3). */
    uint32_t in[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    for (unsigned i = 0; i < 8; i++)
        in[4 + i] = load_le32(key + 4 * i);
    in[12] = counter;
    for (unsigned i = 0; i < 3; i++)
        in[13 + i] = load_le32(nonce + 4 * i);

    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (unsigned r = 0; r < 10; r++) {   /* 10 double rounds = 20 rounds */
        QR(x, 0, 4, 8, 12);
        QR(x, 1, 5, 9, 13);
        QR(x, 2, 6, 10, 14);
        QR(x, 3, 7, 11, 15);
        QR(x, 0, 5, 10, 15);
        QR(x, 1, 6, 11, 12);
        QR(x, 2, 7, 8, 13);
        QR(x, 3, 4, 9, 14);
    }
    for (unsigned i = 0; i < 16; i++)
        store_le32(out + 4 * i, x[i] + in[i]);
    explicit_bzero(x, sizeof(x));
    explicit_bzero(in, sizeof(in));
}

void chacha20_stream(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                     const uint8_t nonce[CHACHA20_NONCE_SIZE], void *out, size_t len)
{
    uint8_t *p = out;
    while (len >= CHACHA20_BLOCK_SIZE) {
        chacha20_block(key, counter++, nonce, p);
        p += CHACHA20_BLOCK_SIZE;
        len -= CHACHA20_BLOCK_SIZE;
    }
    if (len) {
        uint8_t last[CHACHA20_BLOCK_SIZE];
        chacha20_block(key, counter, nonce, last);
        memcpy(p, last, len);
        explicit_bzero(last, sizeof(last));
    }
}
