/* jamcover: jamjar's cover helper. An album cover is a picture inside an
 * MP3's tag on someone's stick, decoded by stb_image, which was not written
 * for hostile input; so it is decoded here, in a process that holds
 * nothing but the picture and its pixels (<jamcover.h>): no namespace, no
 * console, no services, an empty list. A picture that crashes or hangs
 * stb_image costs this process, which jamjar kills and starts again, and
 * that album its cover; never jamjar's handles.
 *
 * It maps the input VMO read-only and the output VMO to write, and serves
 * the jamcover channel (abi/idl/jamcover.idl) until jamjar closes it:
 * each decode (decode.c) reads the picture from the input's first `len`
 * bytes and writes the two sizes of the cover into the output.
 *
 *   jamcover --selftest   the decoder's checks (selftest.c), then exit
 *   --crash, --hang       for jamjar's self-test only: every decode
 *                         crashes the process, or never answers */
#include <idl/jamcover.h>
#include "jamcover_int.h"

struct helper {
    const uint8_t *in;    /* the picture's bytes, JAMCOVER_IN_BYTES, read-only */
    uint32_t      *out;   /* the pixels, JAMCOVER_OUT_BYTES */
    bool           crash, hang;   /* the self-test's broken decoders */
};

static status_t do_decode(void *ctx, uint64_t len, uint32_t large, uint32_t *out_w,
                          uint32_t *out_h)
{
    struct helper *h = ctx;
    if (h->crash)
        *(volatile uint32_t *)0 = 1;   /* a decoder bug, as a page fault */
    while (h->hang)
        jam_nanosleep(DEADLINE_NEVER);
    if (!len || len > JAMCOVER_IN_BYTES)
        return ERR_INVALID_ARGS;
    int w = 0, ht = 0;
    status_t st = cover_decode(h->in, (size_t)len, h->out + JAMCOVER_SMALL_AT / 4,
                               large ? h->out + JAMCOVER_LARGE_AT / 4 : NULL, &w, &ht);
    if (st != OK)
        return st;
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)ht;
    return OK;
}

/* VMO v mapped (bytes of it) with `flags`; NULL if it can't be. */
static void *map(handle_t v, uint64_t bytes, uint32_t flags)
{
    uint64_t addr = 0;
    if (!v || jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, bytes, flags, &addr) != OK)
        return NULL;
    return (void *)(uintptr_t)addr;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--selftest"))
        return jamcover_selftest();
    struct helper h = {
        .in = map(startup_handle(SR_USER + JAMCOVER_ROLE_IN), JAMCOVER_IN_BYTES, VMAR_READ),
        .out = map(startup_handle(SR_USER + JAMCOVER_ROLE_OUT), JAMCOVER_OUT_BYTES,
                   VMAR_READ | VMAR_WRITE),
        .crash = has_arg(argc, argv, "--crash"),
        .hang = has_arg(argc, argv, "--hang"),
    };
    handle_t ch = startup_handle(SR_USER + JAMCOVER_ROLE_CH);
    if (!ch || !h.in || !h.out) {
        printf("jamcover: started without its channel and buffers: jamjar starts it\n");
        return 2;
    }
    static const struct jamcover_ops ops = { .decode = do_decode };
    status_t st = jamcover_serve(ch, &ops, &h);
    return st == OK ? 0 : 1;
}
