/* ChaCha20 (RFC 8439 section 2): the block function and its keystream,
 * for the kernel's random number generator (<jam/random.h>). The kernel
 * encrypts nothing, so there is no XOR helper and no Poly1305. Plain C on
 * 32-bit words (the kernel has no SSE); a block takes well under a
 * microsecond. The RFC's test vectors are ktests (test_random.c). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

/* One block of keystream (RFC 8439 2.3): the 256-bit key, the 32-bit
 * block counter and the 96-bit nonce, all as the RFC's bytes. */
void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[CHACHA20_NONCE_SIZE], uint8_t out[CHACHA20_BLOCK_SIZE]);

/* len bytes of keystream: blocks counter, counter + 1, ..., the last one
 * cut short. The counter must not wrap (len <= (2^32 - counter) * 64);
 * the generator asks for a few hundred bytes at most. */
void chacha20_stream(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                     const uint8_t nonce[CHACHA20_NONCE_SIZE], void *out, size_t len);
