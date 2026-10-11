/* The kernel's SHA-256 (<jam/sha256.h>), which vmo_make_exec_verified
 * checks program pages with: FIPS 180-4's examples (one block, two
 * blocks, the empty message, a million 'a's fed in uneven pieces), and a
 * 4 KiB page whose digest libos's user/lib/sha256.c gave on the same
 * bytes (and the host's shasum agreed), so the table `allow` writes with
 * libos's code matches what the kernel computes. */
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/sha256.h>
#include <jam/string.h>

/* Is digest the 64 hex digits of want? */
static bool is(const uint8_t digest[SHA256_BYTES], const char *want)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < SHA256_BYTES; i++)
        if (want[2 * i] != hex[digest[i] >> 4] || want[2 * i + 1] != hex[digest[i] & 15])
            return false;
    return want[2 * SHA256_BYTES] == '\0';
}

/* FIPS 180-4's examples (NIST's "SHA256.pdf" and the standard's appendix):
 * "abc" (one block), the 448-bit message (two blocks: the padding spills),
 * and the empty one. */
KTEST(sha256_fips_vectors)
{
    uint8_t d[SHA256_BYTES];
    sha256("abc", 3, d);
    KT_ASSERT(is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    static const char two[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, sizeof(two) - 1, d);
    KT_ASSERT(is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    sha256("", 0, d);
    KT_ASSERT(is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

/* A million 'a's, added in pieces of 1 to 97 bytes, so every way a piece
 * can meet a block's edge (and the whole-block path) is taken. */
KTEST(sha256_million_a)
{
    static uint8_t a[97];
    memset(a, 'a', sizeof(a));
    struct sha256 s;
    sha256_init(&s);
    uint64_t left = 1000000;
    for (unsigned n = 1; left; n = n % 97 + 1) {
        unsigned take = left < n ? (unsigned)left : n;
        sha256_add(&s, a, take);
        left -= take;
    }
    uint8_t d[SHA256_BYTES];
    sha256_done(&s, d);
    KT_ASSERT(is(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

/* The page byte i = i * 7 + 3: the digest libos's sha256() gives it
 * (user/lib/sha256.c, built for the host), whole and in pieces. */
KTEST(sha256_page_as_libos)
{
    static const char want[] = "7486da8f1e13943fae21a0b043f1e99640d7d8ebafb25266478b5cddae1272b5";
    uint8_t *page = kmalloc(PAGE_SIZE);
    KT_ASSERT(page);
    for (unsigned i = 0; i < PAGE_SIZE; i++)
        page[i] = (uint8_t)(i * 7 + 3);
    uint8_t d[SHA256_BYTES];
    sha256(page, PAGE_SIZE, d);
    KT_ASSERT(is(d, want));
    struct sha256 s;
    sha256_init(&s);
    sha256_add(&s, page, 1);
    sha256_add(&s, page + 1, 63);
    sha256_add(&s, page + 64, 1000);
    sha256_add(&s, page + 1064, PAGE_SIZE - 1064);
    sha256_done(&s, d);
    KT_ASSERT(is(d, want));
    kfree(page);
}
