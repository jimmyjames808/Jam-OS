/* SHA-256 (FIPS 180-4) in the kernel (kernel/lib/sha256.c): the hash
 * vmo_make_exec_verified checks every page of an approved program against
 * (<jam/vmo_verify.h>). The same functions as libos's <sha256.h> and the
 * drivers' <jam/sha256.h>, which the kernel can't link; the ktests check
 * it against FIPS 180-4's examples and libos's digest of the same page.
 * Plain C on 32-bit words (the kernel has no SSE): a 4 KiB page takes
 * some 10 us. No secret is hashed, so nothing here needs constant time. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_BYTES 32   /* the digest */

struct sha256 {
    uint32_t h[8];        /* the state */
    uint64_t length;      /* bytes taken so far */
    uint8_t  block[64];   /* a block being filled */
    unsigned fill;        /* bytes of it filled */
};

void sha256_init(struct sha256 *s);
void sha256_add(struct sha256 *s, const void *data, size_t n);
void sha256_done(struct sha256 *s, uint8_t out[SHA256_BYTES]);
/* All three for one buffer. */
void sha256(const void *data, size_t n, uint8_t out[SHA256_BYTES]);
