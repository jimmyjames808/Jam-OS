/* nvme: the test words (drv/nvme), for the QEMU tests until the storage
 * protocol is served: `nvmetest=` and a comma-separated list of
 *   selftest  read the namespace's first and last MiB (at most) through
 *             the read path (PRPs, the I/O queue, the interrupt) and log
 *             each one's SHA-256, which a test compares with sha256sum of
 *             the image's ends
 *   loop      read and hash them again and again, each round checked
 *             against the first (a kill then lands mid-read)
 *   reset     reset the controller with the first round's reads out:
 *             they are sent again after it, and must hash the same
 *   abort     the first read is past its deadline at once: an Abort is
 *             sent for it (whatever it answers, the round must come out
 *             right: a read the abort ended is read once more)
 *   nostop    reset, and the reset takes the controller as not stopped
 *             (CSTS.RDY still 1): bus mastering goes off before the
 *             reads out are answered, and the driver ends (exit 3; devmgr
 *             starts it again, and it does the same)
 *   slow      block.c: a channel's answers come BLK_SLOW_STEP apart
 *   on        nothing: no test word (devmgr binds drv/nvme on every boot
 *             now; the tests that want only the driver give this one)
 * Each of loop, reset and abort implies selftest. Reads only, like
 * everything in this driver.
 *
 * One 2 MiB buffer, pinned writable (the device writes what it reads
 * there): the first MiB for the namespace's start, the second for its
 * end. */
#include <jam/sha256.h>
#include "nvme.h"

#define END_BYTES (1u << 20)
#define BUF_PAGES (2 * END_BYTES / NVME_PAGE)

static struct {
    struct nvme *c;
    handle_t vmo;
    uint8_t *buf;
    uint64_t addr[BUF_PAGES];
    uint64_t pin;
    bool     pinned;
    struct nvme_io io[2];
    uint32_t round;            /* rounds started */
    uint32_t left;             /* this round's reads not answered */
    status_t st;               /* this round's first failure */
    bool     again;            /* round 1 was tried twice */
    uint64_t t0;
    uint8_t  first[2][SHA256_BYTES];   /* round 1's digests */
} t = { .vmo = HANDLE_INVALID };

uint32_t selftest_words(const struct driver_start *s)
{
    static const struct { const char *w; uint32_t bits; } words[] = {
        { "selftest", NVME_TEST_SELFTEST },
        { "loop", NVME_TEST_SELFTEST | NVME_TEST_LOOP },
        { "reset", NVME_TEST_SELFTEST | NVME_TEST_RESET },
        { "abort", NVME_TEST_SELFTEST | NVME_TEST_ABORT },
        { "nostop", NVME_TEST_SELFTEST | NVME_TEST_RESET | NVME_TEST_NOSTOP },
        { "slow", NVME_TEST_SLOW },   /* block.c's: no test reads of its own */
        { "on", 0 },                  /* no test word: just the driver */
    };
    static const char key[] = "nvmetest=";
    uint32_t bits = 0;
    for (uint32_t i = 0; i < s->nargs && i < DRV_MAX_ARGS; i++) {
        const char *a = s->args[i];
        unsigned k = 0;
        while (a && key[k] && a[k] == key[k])
            k++;
        if (!a || key[k])
            continue;
        for (const char *p = a + k; *p;) {
            unsigned n = 0;
            while (p[n] && p[n] != ',')
                n++;
            for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
                unsigned m = 0;
                while (m < n && words[w].w[m] == p[m])
                    m++;
                if (m == n && !words[w].w[m])
                    bits |= words[w].bits;
            }
            p += n + (p[n] == ',');
        }
    }
    return bits;
}

static void round_start(void);

static bool same(const uint8_t *a, const uint8_t *b)
{
    unsigned d = 0;
    for (unsigned i = 0; i < SHA256_BYTES; i++)
        d |= a[i] ^ b[i];
    return !d;
}

/* Round 1: the digests logged (the test compares them); later rounds:
 * compared with round 1's. */
static void digests(struct nvme *c)
{
    uint8_t d[2][SHA256_BYTES];
    char hex[2 * SHA256_BYTES + 1];
    uint32_t bytes = t.io[0].blocks * c->ns.block_size;
    for (unsigned k = 0; k < 2; k++)
        sha256(t.buf + k * END_BYTES, bytes, d[k]);
    if (t.round == 1) {
        for (unsigned k = 0; k < 2; k++) {
            for (unsigned i = 0; i < SHA256_BYTES; i++)
                t.first[k][i] = d[k][i];
            sha256_hex(d[k], hex);
            drv_log("%s: selftest: %s %u bytes (lba %lu) sha256 %s", c->name,
                    k ? "last" : "first", bytes, (unsigned long)t.io[k].lba, hex);
        }
        drv_report("%s: selftest: both ends read and hashed (%u bytes each, %lu ms)", c->name,
                   bytes, (unsigned long)((drv_clock_ns() - t.t0) / NS_PER_MS));
        return;
    }
    if (!same(d[0], t.first[0]) || !same(d[1], t.first[1]))
        drv_report("%s: selftest: FAILED: round %u read different bytes", c->name, t.round);
    else if (t.round % 100 == 0)
        drv_log("%s: selftest: round %u: the same digests", c->name, t.round);
}

static void read_done(struct nvme *c, struct nvme_io *io, status_t st)
{
    (void)io;
    if (st != OK && t.st == OK)
        t.st = st;
    if (--t.left || c->stopping)
        return;
    if (t.st != OK && t.round == 1 && !t.again) {
        drv_log("%s: selftest: round 1 failed (%s): once more", c->name, status_str(t.st));
        t.again = true;
        t.round = 0;
        round_start();
        return;
    }
    if (t.st != OK) {
        drv_report("%s: selftest: FAILED: round %u: %s", c->name, t.round, status_str(t.st));
        return;
    }
    digests(c);
    if (c->test & NVME_TEST_LOOP)
        round_start();
}

static void round_start(void)
{
    struct nvme *c = t.c;
    uint64_t n = END_BYTES / c->ns.block_size;
    n = n < c->ns.blocks ? n : c->ns.blocks;
    t.round++;
    t.st = OK;
    t.left = 2;
    t.t0 = drv_clock_ns();
    for (unsigned k = 0; k < 2; k++) {
        t.io[k] = (struct nvme_io){ .lba = k ? c->ns.blocks - n : 0, .blocks = (uint32_t)n,
                                    .pages = t.addr + k * (BUF_PAGES / 2),
                                    .npages = BUF_PAGES / 2, .done = read_done };
        status_t st = nvme_submit(c, &t.io[k]);
        if (st != OK) {
            drv_report("%s: selftest: FAILED: the read refused (%s)", c->name, status_str(st));
            t.st = st;
            t.left--;
        }
    }
}

bool selftest_start(struct nvme *c)
{
    t.c = c;
    void *p = NULL;
    status_t st = drv_vmo_create(2 * END_BYTES, 0, &t.vmo);
    if (st == OK && (st = drv_vmo_map(t.vmo, 0, 2 * END_BYTES, VMAR_READ | VMAR_WRITE, &p)) == OK)
        t.buf = p;
    if (st == OK && (st = drv_vmo_pin(t.vmo, c->dma, 0, 2 * END_BYTES, t.addr, &t.pin)) == OK)
        t.pinned = true;
    if (st != OK) {
        drv_log("%s: selftest: no buffer (%s)", c->name, status_str(st));
        return false;
    }
    if (c->test & NVME_TEST_ABORT)
        c->expire_next = 1;
    round_start();
    if ((c->test & NVME_TEST_ABORT) && !queue_timeouts(c))   /* before any answer is taken */
        drv_log("%s: selftest: no admin slot for the abort", c->name);
    if (c->test & NVME_TEST_RESET) {
        drv_log("%s: selftest: a reset with %u command(s) out", c->name, c->io.busy);
        if (ctrl_reset(c, "the test word") != OK) {
            c->failed = ERR_IO;   /* the loop ends, as after its own reset failed */
            return false;
        }
    }
    return true;
}

void selftest_stop(struct nvme *c)
{
    if (t.pinned && drv_vmo_unpin(t.vmo, c->dma, t.pin) == OK)
        t.pinned = false;
    if (t.buf)
        (void)drv_vmo_unmap(t.buf, 2 * END_BYTES);
    if (t.vmo != HANDLE_INVALID)
        drv_handle_close(t.vmo);
    t.buf = NULL;
    t.vmo = HANDLE_INVALID;
}
