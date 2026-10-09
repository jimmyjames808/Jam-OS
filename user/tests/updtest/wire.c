/* updtest wire: the offer call's wire (abi/idl/update.idl) against the live
 * init, by hand: every malformed form of an offer is refused before init
 * looks at it, and a good one still goes through afterwards on the same
 * channel (M12 P6b). Run by tools/update-test.sh after `updtest bad`.
 *
 * The messages are built byte for byte, so the test says what is on the
 * wire: txid, ordinal ((82 << 16) | 1), flags, the three lengths, the
 * manifest as a u16 count and its bytes, and the VMOs as handles. The
 * generated server refuses a message of the wrong size, a manifest past
 * 1024 bytes, a count that is not the bytes there, more than three handles
 * or a handle that is no VMO; a message of another protocol (or of the
 * older one, whose second word was the magic "JUPO") is
 * ERR_NOT_SUPPORTED. None of this reaches init's update code, and none of
 * it loads anything: the one good offer is check-only.
 *
 * Needs the files updtest needs (/data/update/, init's control channel). */
#include <idl/initctl.h>
#include <os.h>
#include <update.h>
#include "updtest.h"

#define WIRE_WAIT   (10 * NS_PER_S)
#define UPDATE_ID   82u
#define M_OFFER     1u
#define OLD_MAGIC   0x4f50554au   /* "JUPO": the second word of the older offer */
#define FIXED       36u           /* flags and the three lengths, with the header */

/* The request: header, flags, lengths, then a manifest of `count` and mlen bytes. */
static uint32_t body(uint8_t *q, uint32_t ordinal, uint32_t flags, const struct build *b,
                     uint16_t count, uint32_t mlen)
{
    memset(q, 0, 8);
    memcpy(q + 4, &ordinal, 4);
    memcpy(q + 8, &flags, 4);
    memcpy(q + 12, b->bytes, 24);
    memcpy(q + FIXED, &count, 2);
    for (uint32_t i = 0; i < mlen; i++)
        q[FIXED + 2 + i] = i < b->manifest_len ? (uint8_t)b->manifest[i] : 'x';
    return FIXED + 2 + mlen;
}

/* Call on ch: q (n bytes) with the handles; the reply's status (*st) and, if the
 * reply has them, the verdict and why. */
static status_t call(handle_t ch, uint8_t *q, uint32_t n, const handle_t *hs, uint32_t nh,
                     status_t *st, int32_t *verdict, uint32_t *why)
{
    uint8_t rep[512];
    uint32_t got = 0;
    struct channel_call_args a = {
        .h = ch, .wr_len = n, .wr_bytes = (uint64_t)(uintptr_t)q,
        .wr_handles = (uint64_t)(uintptr_t)hs, .wr_nhandles = nh,
        .rd_cap = sizeof(rep), .rd_bytes = (uint64_t)(uintptr_t)rep,
        .rd_actual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + WIRE_WAIT,
    };
    status_t r = jam_channel_call(&a);
    if (r != OK)
        return r;
    if (got < 8)
        return ERR_INTERNAL;
    int32_t s;
    memcpy(&s, rep + 4, 4);
    *st = s;
    if (s == OK && got >= 16) {
        memcpy(verdict, rep + 8, 4);
        memcpy(why, rep + 12, 4);
    }
    return OK;
}

/* n duplicates of the build's kernel VMO. */
static bool dups(const struct build *b, handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (jam_handle_duplicate(b->vmo[i % UPDATE_FILES], RIGHT_SAME, &hs[i]) != OK)
            return false;
    return true;
}

static void say(const char *name, bool ok, const char *got)
{
    failures += !ok;
    printf("updtest: %s (wire): %s %s\n", name, got, ok ? "as expected" : "FAILED");
}

/* One malformed offer on a channel of its own: the call must be refused with `want`. */
static void refused(const char *name, const struct build *b, status_t want, uint32_t ordinal,
                    uint16_t count, uint32_t mlen, int trim, unsigned nvmo, bool channel_end)
{
    handle_t ch, hs[5];
    uint8_t q[FIXED + 2 + 1100];
    status_t st = initctl_update_offer_until(initctl, now() + WIRE_WAIT, &ch);
    if (st != OK) {
        say(name, false, status_str(st));
        return;
    }
    uint32_t n = body(q, ordinal, UPDATE_OFFER_CHECK_ONLY, b, count, mlen);
    if (!dups(b, hs, nvmo)) {
        say(name, false, "no handles");
        return;
    }
    handle_t a, other = HANDLE_INVALID;
    if (channel_end && jam_channel_create(&a, &other) == OK) {   /* the last handle is no VMO */
        jam_handle_close(hs[nvmo - 1]);
        hs[nvmo - 1] = a;
    }
    status_t reply = ERR_INTERNAL;
    int32_t verdict = 0;
    uint32_t why = 0;
    st = call(ch, q, (uint32_t)((int)n + trim), hs, nvmo, &reply, &verdict, &why);
    if (other)
        jam_handle_close(other);
    jam_handle_close(ch);
    say(name, st == OK && reply == want, st == OK ? status_str(reply) : status_str(st));
}

/* The good offer, check-only, on one channel after malformed ones on it. */
static void good_after(const struct build *b)
{
    handle_t ch, hs[3];
    uint8_t q[FIXED + 2 + UPDATE_MANIFEST_MAX];
    uint32_t ordinal = (UPDATE_ID << 16) | M_OFFER, m = b->manifest_len;
    status_t st = initctl_update_offer_until(initctl, now() + WIRE_WAIT, &ch);
    if (st != OK) {
        say("a good offer after malformed ones", false, status_str(st));
        return;
    }
    /* four malformed ones (each with a VMO: it is closed with the refusal) */
    const int trims[4] = { -1, 1, -(int)(m + 2), -2 };
    status_t reply = ERR_INTERNAL;
    int32_t verdict = -1;
    uint32_t why = 99;
    bool ok = true;
    for (unsigned i = 0; ok && i < 4; i++) {
        uint32_t n = body(q, ordinal, UPDATE_OFFER_CHECK_ONLY, b, (uint16_t)m, m);
        ok = dups(b, hs, 1) &&
             call(ch, q, (uint32_t)((int)n + trims[i]), hs, 1, &reply, &verdict, &why) == OK &&
             reply == ERR_INVALID_ARGS;
    }
    say("four malformed offers on one channel", ok, status_str(reply));
    ok = ok && dups(b, hs, 2);
    if (ok) {
        uint32_t n = body(q, ordinal, UPDATE_OFFER_CHECK_ONLY, b, (uint16_t)m, m);
        st = call(ch, q, n, hs, 2, &reply, &verdict, &why);
        ok = st == OK && reply == OK && verdict == OK && why == UPDATE_ACCEPTED;
    }
    say("then the good offer, on the same channel", ok, st == OK ? update_why_str(why) : status_str(st));
    /* one offer a channel: init closed it with the answer */
    if (ok && dups(b, hs, 2)) {
        uint32_t n = body(q, ordinal, UPDATE_OFFER_CHECK_ONLY, b, (uint16_t)m, m);
        st = call(ch, q, n, hs, 2, &reply, &verdict, &why);
        say("a second offer on that channel", st == ERR_PEER_CLOSED, status_str(st));
    }
    jam_handle_close(ch);
}

/* No VMOs at all: not malformed, so an answer: BAD_OFFER. */
static void no_files(const struct build *b)
{
    handle_t ch;
    uint8_t q[FIXED + 2 + UPDATE_MANIFEST_MAX];
    status_t st = initctl_update_offer_until(initctl, now() + WIRE_WAIT, &ch);
    if (st != OK) {
        say("no files", false, status_str(st));
        return;
    }
    uint32_t n = body(q, (UPDATE_ID << 16) | M_OFFER, UPDATE_OFFER_CHECK_ONLY, b,
                      (uint16_t)b->manifest_len, b->manifest_len);
    status_t reply = ERR_INTERNAL;
    int32_t verdict = 0;
    uint32_t why = 0;
    st = call(ch, q, n, NULL, 0, &reply, &verdict, &why);
    jam_handle_close(ch);
    say("no files (an answer, not a refusal)", st == OK && reply == OK && verdict != OK &&
        why == UPDATE_BAD_OFFER, st == OK ? update_why_str(why) : status_str(st));
}

void wire_cases(const struct build *b)
{
    uint32_t ok_ord = (UPDATE_ID << 16) | M_OFFER, m = b->manifest_len;
    refused("another protocol's ordinal", b, ERR_NOT_SUPPORTED, (18u << 16) | 1u, (uint16_t)m, m,
            0, 2, false);
    refused("the older form (its second word was the magic)", b, ERR_NOT_SUPPORTED, OLD_MAGIC,
            (uint16_t)m, m, 0, 2, false);
    refused("a manifest of 1025 bytes", b, ERR_INVALID_ARGS, ok_ord, 1025, 1025, 0, 2, false);
    refused("a count past the bytes there", b, ERR_INVALID_ARGS, ok_ord, (uint16_t)(m + 50), m, 0,
            2, false);
    refused("a byte after the manifest", b, ERR_INVALID_ARGS, ok_ord, (uint16_t)m, m, 1, 2,
            false);
    refused("the manifest cut off", b, ERR_INVALID_ARGS, ok_ord, (uint16_t)m, m, -(int)(m / 2), 2,
            false);
    refused("no manifest count at all", b, ERR_INVALID_ARGS, ok_ord, 0, 0, -2, 2, false);
    refused("four VMOs", b, ERR_INVALID_ARGS, ok_ord, (uint16_t)m, m, 0, 4, false);
    refused("a channel end for a file", b, ERR_WRONG_TYPE, ok_ord, (uint16_t)m, m, 0, 2, true);
    no_files(b);
    good_after(b);
}
