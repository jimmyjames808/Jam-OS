/* The update manifest's signature check (<update.h>): Ed25519 as RFC 8032
 * has it (SHA-512), with Monocypher's crypto_ed25519_check
 * (third_party/monocypher), the same code tools/jamos-sign signs with on
 * the Mac. A file of its own so that only a program that checks a
 * signature (init, utest) links Monocypher in, not every user of the
 * manifest's parser (bin/update). */
#include <monocypher-ed25519.h>
#include <update.h>

status_t update_manifest_verify(const struct update_manifest *m, const void *text,
                                const uint8_t key[UPDATE_KEY_BYTES])
{
    if (!m->has_signature || !text)
        return ERR_ACCESS_DENIED;
    /* 0 only for a valid signature of exactly these bytes by this key, and
     * a canonical one (S below the group order: no second form of it). */
    int bad = crypto_ed25519_check(m->signature, key, text, m->signed_len);
    return bad ? ERR_ACCESS_DENIED : OK;
}
