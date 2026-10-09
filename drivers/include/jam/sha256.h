/* <jam/sha256.h>: SHA-256 (FIPS 180-4) for drivers (drivers/lib/sha256.c,
 * linked into a driver's own object by the Makefile's DRV_LIB_<name>):
 * the digests drv/nvme computes of what it reads, so they can be compared
 * with the same bytes hashed elsewhere (sha256sum on a host, Linux). The
 * same functions as libos's <sha256.h>, which drivers can't include. Not
 * built for speed. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_BYTES 32   /* the digest */

struct sha256 {
    uint32_t h[8];         /* the state */
    uint64_t length;       /* bytes taken so far */
    uint8_t  block[64];    /* a block being filled */
    unsigned fill;         /* bytes of it filled */
};

void sha256_init(struct sha256 *s);
void sha256_add(struct sha256 *s, const void *data, size_t n);
void sha256_done(struct sha256 *s, uint8_t out[SHA256_BYTES]);
/* All three for one buffer. */
void sha256(const void *data, size_t n, uint8_t out[SHA256_BYTES]);
/* The digest as 64 lower-case hex digits and a NUL. */
void sha256_hex(const uint8_t digest[SHA256_BYTES], char out[2 * SHA256_BYTES + 1]);
