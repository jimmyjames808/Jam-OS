/* The wayland section's fuzz loop: FUZZ_ROUNDS rounds, each on a fresh
 * rig (wlrig.h), of either a mutated stream of messages sent in random
 * pieces with random descriptors (every batch the compositor reads must
 * be well formed), or mutated batches (bad magics, counts that lie,
 * handles of every kind, random bytes) read with random room. Nothing
 * may crash, and at the end the job holds the handles it held before
 * and the lowest free descriptor is the one it was. A fixed seed, so a
 * failure repeats. */
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "pt.h"
#include "wlrig.h"

#define FUZZ_ROUNDS 10000

static uint64_t seed = 0x6a616d4f53ull;

/* A number below n (a 64-bit LCG's high bits). */
static uint32_t rnd(uint32_t n)
{
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(seed >> 33) % n;
}

static void fuzz_stream(struct rig *r, const int *pool, unsigned npool)
{
    static uint8_t s[8192];
    uint32_t len = 0;
    while (len < sizeof(s) - JWL_MSG_MAX) {
        uint32_t size = 8 + 4 * rnd(rnd(8) ? 16 : 1023);
        wr_msg(s + len, 1 + rnd(100), size);
        len += size;
    }
    for (unsigned k = rnd(3); k; k--)   /* a few bytes changed: sizes go bad */
        s[rnd(len)] = (uint8_t)rnd(256);
    for (uint32_t at = 0; at < len;) {
        uint32_t piece = 1 + rnd(rnd(4) ? 64 : 6000);
        int fds[4];
        unsigned nf = rnd(4) ? 0 : 1 + rnd(4);
        for (unsigned i = 0; i < nf; i++)
            fds[i] = pool[rnd(npool)];
        ssize_t n = wr_send(r->fd, s + at, piece < len - at ? piece : len - at, fds, nf);
        if (n <= 0)
            break;   /* a bad header, a refused kind, too many waiting: the stream stops */
        at += (uint32_t)n;
    }
    static struct batch bt;
    while (wr_comp_read(r, &bt) == OK) {
        CHECK(wr_batch_ok(&bt));
        wr_close_handles(bt.h, bt.nh);
    }
}

/* A handle of a random kind: a kept VMO, a channel end, or an event. */
static handle_t any_handle(void)
{
    handle_t h = HANDLE_INVALID, spare;
    uint32_t kind = rnd(3);
    if (kind == 0 && jam_vmo_create(0, VMO_KEEP_PAGES, &h) == OK)
        return h;
    if (kind == 1 && jam_channel_create(&h, &spare) == OK) {
        jam_handle_close(spare);
        return h;
    }
    CHECK(jam_event_create(&h) == OK);
    return h;
}

static void fuzz_batches(struct rig *r)
{
    static uint8_t b[1024];
    for (unsigned k = 1 + rnd(4); k; k--) {
        uint32_t n = rnd(sizeof(b)), nh = rnd(5);
        for (uint32_t i = 0; i < n; i++)
            b[i] = (uint8_t)rnd(256);
        handle_t h[4];
        for (uint32_t i = 0; i < nh; i++)
            h[i] = any_handle();
        const uint32_t hdr[2] = { rnd(8) ? JWL_MAGIC : (uint32_t)seed, rnd(8) ? nh : rnd(30) };
        (void)wr_comp_write_hdr(r, hdr, b, n, h, nh);   /* the handles move, whatever */
    }
    for (int i = 0; i < 8; i++) {
        uint8_t in[512];
        struct rx got;
        ssize_t n = wr_recv(r->fd, in, 1 + rnd(sizeof(in)), rnd(5), &got);
        for (unsigned f = 0; f < got.n; f++)
            close(got.fds[f]);
        if (n <= 0)
            break;
    }
}

void wr_fuzz(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    int pool[] = { wr_memfd_with("fuzz"), p[0], p[1], eventfd(0, 0) };   /* the eventfd: refused */
    int lowest = wr_sock_new();
    close(lowest);
    uint64_t before = wr_job_handles();
    int rounds = 0;
    for (int i = 0; i < FUZZ_ROUNDS; i++) {
        struct rig r;
        if (!wr_open(&r))
            break;
        if (rnd(2))
            fuzz_stream(&r, pool, 4);
        else
            fuzz_batches(&r);
        wr_close(&r);
        rounds++;
    }
    CHECK(rounds == FUZZ_ROUNDS);
    uint64_t after = wr_job_handles();
    if (after != before)
        printf("posixtest: wayland: fuzz: %llu handles before, %llu after\n",
               (unsigned long long)before, (unsigned long long)after);
    CHECK(after == before);
    int fd = wr_sock_new();
    CHECK(fd == lowest);   /* no descriptor left open */
    close(fd);
    for (int i = 0; i < 4; i++)
        close(pool[i]);
    printf("posixtest: wayland: fuzz: %d rounds\n", rounds);
}
