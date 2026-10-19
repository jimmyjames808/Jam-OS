/* The POSIX hand-over as hostile input (<posixfds.h>): every malformed
 * form a reader must refuse, each built by hand from a good two-message
 * hand-over with one thing broken, then 10,000 random ones; and what the
 * writer refuses. Each time: the read fails (or, for a random one that
 * happens to be good, succeeds with descriptors of the right shape), the
 * sink holds nothing, nothing is left queued, and every handle the
 * hand-over carried is closed (the process's handle count is back where
 * it was). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <posixfds.h>
#include "pfdtest.h"
#include "utest.h"

#define FUZZ_ROUNDS 10000

static struct pfd_raw msgs[3];       /* the hand-over being sent */
static struct pfd_collect col;       /* the sink */

/* The good hand-over: message 0 with "/data", a pipe (0), a file (1) and
 * a directory (2); message 1, the last, with a socket (5), /dev/null (6)
 * and a directory channel (7: "/data", "/data/d"). */
static status_t good(void)
{
    status_t st;
    uint8_t dirch[64];
    uint32_t dl = pfd_dirch(dirch, 9, "/data", "/data/d");
    msgs[2].nh = 0;
    pfd_raw_begin(&msgs[0], 0, false, "/data");
    pfd_raw_begin(&msgs[1], 1, true, NULL);
    if ((st = pfd_raw_fd(&msgs[0], 0, POSIXFDS_PIPE, 0, 0, NULL, 0)) != OK ||
        (st = pfd_raw_fd(&msgs[0], 1, POSIXFDS_FILE, 2, 10, "f", 2)) != OK ||
        (st = pfd_raw_fd(&msgs[0], 2, POSIXFDS_DIR, 0, 0, "/boot", 6)) != OK ||
        (st = pfd_raw_fd(&msgs[1], 5, POSIXFDS_SOCKET, 2048, 0, NULL, 0)) != OK ||
        (st = pfd_raw_fd(&msgs[1], 6, POSIXFDS_DEV, 2, 0, "null", 5)) != OK)
        return st;
    return pfd_raw_fd(&msgs[1], 7, POSIXFDS_DIR_CHANNEL, 0, 3, dirch, dl);
}

/* The directory channel's data in good()'s message 1: byte off of it. */
#define DIRCH(off) rec_data(1, 2, sizeof(struct posixfds_dirch) + (off))

static struct posixfds_fd *rec(unsigned m, unsigned i)
{
    return (struct posixfds_fd *)(msgs[m].b + msgs[m].rec_at[i]);
}

/* Byte `off` of record i's data in message m. */
static uint8_t *rec_data(unsigned m, unsigned i, unsigned off)
{
    return msgs[m].b + msgs[m].rec_at[i] + sizeof(struct posixfds_fd) + off;
}

/* Break the good hand-over in way `which` (*nmsg: messages to send);
 * false past the last way. */
static bool breaks(unsigned which, unsigned *nmsg)
{
    struct posixfds_msg *h0 = pfd_hdr(&msgs[0]), *h1 = pfd_hdr(&msgs[1]);
    uint8_t *cwd = msgs[0].b + sizeof(*h0);
    switch (which) {
    case 0:  h0->txid = 1; break;
    case 1:  h0->magic ^= 1; break;
    case 2:  h1->version = POSIXFDS_VERSION + 1; break;
    case 3:  h1->flags |= 2; break;
    case 4:  h0->seq = 1; break;
    case 5:  h1->seq = 2; break;
    case 6:  h1->seq = 0; break;
    case 7:  h0->nhandles++; break;
    case 8:  h0->cwd_len = 0; break;
    case 9:  cwd[0] = 'd'; break;                         /* "data": not absolute */
    case 10: cwd[5] = 'x'; break;                         /* no NUL */
    case 11: cwd[2] = 0; break;                           /* a NUL inside */
    case 12: cwd[7] = 1; break;                           /* its padding */
    case 13: h0->state.umask = 01000; break;
    case 14: h0->state.sigmask |= POSIXFDS_SIG(9); break;
    case 15: h0->state.sigignore |= POSIXFDS_SIG(19); break;
    case 16: h0->state.ppid = POSIXFDS_PID_MAX; break;
    case 17: h1->state.umask = 1; break;                  /* state past the first */
    case 18: h1->cwd_len = 2; break;
    case 19: rec(0, 0)->fd = -1; break;
    case 20: rec(0, 0)->fd = POSIXFDS_FD_MAX; break;
    case 21: rec(0, 1)->fd = 0; break;                    /* twice in a message */
    case 22: rec(1, 0)->fd = 0; break;                    /* twice in the hand-over */
    case 23: rec(0, 0)->kind = 0; break;
    case 24: rec(0, 0)->kind = POSIXFDS_KIND_COUNT; break;
    case 25: rec(0, 0)->kind = POSIXFDS_EVENTFD; break;   /* a channel, not an event */
    case 27: rec(0, 0)->nhandles = 2; break;
    case 28: rec(0, 0)->status = 0x80000000u; break;
    case 29: rec(0, 0)->status = POSIXFDS_O_ACCMODE; break;
    case 30: rec(0, 0)->offset = 1; break;                /* a pipe has none */
    case 31: *rec_data(0, 2, 0) = 'b'; break;             /* a relative directory */
    case 32: *rec_data(1, 1, 0) = '/'; break;             /* a /dev name with '/' */
    case 33: *rec_data(0, 2, 5) = 'x'; break;             /* a path without its NUL */
    case 34: *rec_data(0, 2, 7) = 1; break;               /* data padding */
    case 35: rec(0, 2)->data_len = POSIXFDS_DATA_MAX + 1; break;
    case 36: msgs[1].n += 8; break;                       /* trailing bytes */
    case 37: msgs[1].n -= 8; break;                       /* cut short */
    case 38: *nmsg = 1; break;                            /* the last never comes */
    case 39: msgs[0].n = 16; break;                       /* shorter than a header */
    case 40: h0->reserved = 1; break;
    case 41: memset(msgs[1].b, 0x5a, msgs[1].n); break;   /* garbage */
    case 42: h1->flags = 0; break;                        /* not the last after all */
    case 43:                                              /* an extra handle */
        if (jam_event_create(&msgs[1].h[msgs[1].nh]) == OK)
            h1->nhandles = ++msgs[1].nh;
        break;
    case 45: h0->flags = POSIXFDS_LAST; break;            /* a message after the last */
    case 44:                                              /* a later message with nothing */
        pfd_raw_drop(&msgs[1]);
        pfd_raw_begin(&msgs[1], 1, true, NULL);
        break;
    case 46: h0->exe_len = 2; break;                      /* a program's path that isn't there */
    case 47: h1->exe_len = 2; break;                      /* a path past the first */
    case 48: h0->state.sigpending |= POSIXFDS_SIG(9); break;
    case 49: h0->version = 1; break;                      /* the format before FL3's */
    case 50: h0->version = 2; break;                      /* ... and before DL2's */
    case 51: h1->flags |= POSIXFDS_CWD; break;            /* a directory channel past the first */
    case 52: h0->flags |= POSIXFDS_CWD; break;            /* "/data" is no channel's data */
    case 53: *DIRCH(0) = 'd'; break;                      /* a mount "data" */
    case 54: *DIRCH(5) = '/'; break;                      /* a mount "/data//data/d" */
    case 55: *DIRCH(6) = 'x'; break;                      /* a relative text */
    case 56: *DIRCH(13) = 'x'; break;                     /* the text without its NUL */
    case 57: rec(1, 2)->data_len = POSIXFDS_DIRCH_MIN - 1; break;
    case 58: rec(1, 2)->nhandles = 0; break;              /* its channel left out */
    case 59:                                              /* an event, not a channel */
        jam_handle_close(msgs[1].h[4]);
        if (jam_event_create(&msgs[1].h[4]) != OK)
            msgs[1].h[4] = HANDLE_INVALID;
        break;
    default:
        return false;
    }
    return true;
}

/* Send msgs[0 .. n) on a and read them from b: the read's status. */
static status_t send_and_read(handle_t a, handle_t b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        (void)pfd_raw_send(a, &msgs[i]);   /* a failed write closes its handles: still checked */
    for (unsigned i = n; i < 3; i++)
        pfd_raw_drop(&msgs[i]);
    struct posixfds_sink sink = pfd_sink(&col);
    struct posixfds_state s;
    static char cwd[POSIXFDS_PATH_MAX];
    return posixfds_read(b, &sink, &s, cwd);
}

/* Every way of breaking it is refused, whole, with everything closed. */
bool t_posixfds_malformed(void)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    uint64_t before = pfd_handles();
    CHECK_ST(good(), OK);
    CHECK_ST(send_and_read(a, b, 2), OK);   /* the good one is good */
    CHECK_EQ(col.n, 6u);
    pfd_collect_close(&col);
    unsigned tried = 0;
    for (unsigned which = 0; which < 64; which++) {
        unsigned nmsg = 2;
        CHECK_ST(good(), OK);
        if (!breaks(which, &nmsg)) {
            pfd_raw_drop(&msgs[0]);
            pfd_raw_drop(&msgs[1]);
            continue;
        }
        tried++;
        status_t st = send_and_read(a, b, nmsg);
        if (st != ERR_INVALID_ARGS)
            FAIL("break %u: read gave %s", which, status_str(st));
        if (col.n || pfd_queued(b) || pfd_handles() != before)
            FAIL("break %u: %u kept, %u queued, %ld handles more", which, col.n,
                 pfd_queued(b), (long)(pfd_handles() - before));
        if (which == 22 && col.resets != 1)
            FAIL("break 22: the first message's descriptors were not reset");
    }
    CHECK_EQ(tried, 59u);

    /* A sink that refuses: its error, everything closed. */
    CHECK_ST(good(), OK);
    for (unsigned i = 0; i < 2; i++)
        CHECK_ST(pfd_raw_send(a, &msgs[i]), OK);
    struct posixfds_sink sink = pfd_sink(&col);
    col.refuse = ERR_NO_MEMORY;
    struct posixfds_state s;
    static char cwd[POSIXFDS_PATH_MAX];
    CHECK_ST(posixfds_read(b, &sink, &s, cwd), ERR_NO_MEMORY);
    CHECK_EQ(pfd_queued(b), 0u);
    CHECK_EQ(pfd_handles(), before);
    jam_handle_close(a);
    jam_handle_close(b);
    return true;
}

/* POSIXFDS_DUP: a descriptor that is another of the hand-over, sent
 * before it. One good form, and what the reader must refuse. */
bool t_posixfds_dup(void)
{
    struct dup_case {
        const char *what;
        int32_t  fd;
        uint32_t target, status, data_len;
        uint64_t offset;
        bool     good;
    } cases[] = {
        { "an earlier descriptor", 3, 0, 0, 4, 0, true },
        { "a descriptor not sent", 3, 9, 0, 4, 0, false },
        { "itself", 3, 3, 0, 4, 0, false },
        { "one sent after it", 3, 5, 0, 4, 0, false },
        { "past the table", 3, POSIXFDS_FD_MAX, 0, 4, 0, false },
        { "data of 3 bytes", 3, 0, 0, 3, 0, false },
        { "data of 8 bytes", 3, 0, 0, 8, 0, false },
        { "no data", 3, 0, 0, 0, 0, false },
        { "a status of its own", 3, 0, 2, 4, 0, false },
        { "an offset of its own", 3, 0, 0, 4, 1, false },
    };
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    uint64_t before = pfd_handles();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct dup_case *c = &cases[i];
        uint8_t data[8] = { 0 };
        memcpy(data, &c->target, sizeof(c->target));
        pfd_raw_begin(&msgs[0], 0, true, "/data");
        if (pfd_raw_fd(&msgs[0], 0, POSIXFDS_PIPE, 0, 0, NULL, 0) != OK ||
            pfd_raw_fd(&msgs[0], c->fd, POSIXFDS_DUP, c->status, c->offset, data,
                       c->data_len) != OK ||
            pfd_raw_fd(&msgs[0], 5, POSIXFDS_DEV, 2, 0, "null", 5) != OK)
            FAIL("dup case %u (%s): not built", i, c->what);
        status_t st = send_and_read(a, b, 1);
        if (c->good ? st != OK || col.n != 3u || col.got[1].rec.kind != POSIXFDS_DUP
                    : st != ERR_INVALID_ARGS || col.n)
            FAIL("dup case %u (%s): read gave %s, %u kept", i, c->what, status_str(st), col.n);
        pfd_collect_close(&col);
        if (pfd_queued(b) || pfd_handles() != before)
            FAIL("dup case %u (%s): %u queued, %ld handles more", i, c->what, pfd_queued(b),
                 (long)(pfd_handles() - before));
    }
    jam_handle_close(a);
    jam_handle_close(b);
    return true;
}

/* ---- the fuzz loop ------------------------------------------------------------------- */

static uint64_t rnd_state;

static uint32_t rnd(uint32_t below)
{
    uint64_t x = rnd_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rnd_state = x;
    return (uint32_t)((x * 0x2545f4914f6cdd1dull) >> 32) % below;
}

/* A random hand-over of 1 to 3 messages, mostly good. */
static unsigned random_handover(void)
{
    static const char *const data[] = { "/data", "null", "", "x", "/etc/hosts", "a/b" };
    static const uint32_t status[] = { 0, 1, 2, 0x400, 0x802, 3, 0x40000000 };
    unsigned n = 1 + rnd(3);
    for (unsigned m = 0; m < n; m++) {
        pfd_raw_begin(&msgs[m], (uint16_t)m, m == n - 1, rnd(8) ? "/data" : "rel");
        for (unsigned i = m ? 1 + rnd(4) : rnd(5); i; i--) {
            const char *d = data[rnd(6)];
            uint32_t len = *d ? (uint32_t)strlen(d) + 1 : 0;
            uint16_t kind = (uint16_t)(1 + rnd(POSIXFDS_KIND_COUNT - 1));
            (void)pfd_raw_fd(&msgs[m], (int32_t)rnd(1100), kind, status[rnd(7)],
                             rnd(4) ? 0 : rnd(100), d, len);
        }
    }
    for (unsigned m = n; m < 3; m++)
        msgs[m].nh = 0;
    return n;
}

/* Spoil it a little, most of the time. */
static void spoil(unsigned *n)
{
    struct pfd_raw *m = &msgs[rnd(*n)];
    switch (rnd(6)) {
    case 0:
        for (unsigned k = 1 + rnd(3); k; k--)
            m->b[rnd(m->n)] ^= (uint8_t)(1 + rnd(255));
        break;
    case 1:
        m->n = rnd(m->n + 1);
        break;
    case 2:
        if (*n > 1)
            pfd_raw_drop(&msgs[--*n]);
        break;
    case 3:
        if (m->nh)
            jam_handle_close(m->h[--m->nh]);   /* one handle short */
        break;
    case 4:
        m->b[rnd(m->n < 64 ? m->n : 64)] = (uint8_t)rnd(256);   /* the header */
        break;
    default:
        break;   /* as made */
    }
}

/* A hand-over the reader took: every descriptor of its kind's shape. */
static bool took_good(void)
{
    for (unsigned i = 0; i < col.n; i++) {
        uint32_t types[POSIXFDS_KIND_HANDLES];
        int k = posixfds_kind_handles(col.got[i].rec.kind, types);
        CHECK(k >= 0 && k == col.got[i].rec.nhandles);
        for (int j = 0; j < k; j++) {
            struct handle_info info;
            CHECK_ST(jam_handle_info(col.got[i].hs[j], &info), OK);
            CHECK_EQ(info.type, types[j]);
        }
    }
    return true;
}

bool t_posixfds_fuzz(void)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    uint64_t before = pfd_handles();
    rnd_state = 0x9e3779b97f4a7c15ull;
    unsigned taken = 0;
    for (unsigned round = 0; round < FUZZ_ROUNDS; round++) {
        unsigned n = random_handover();
        spoil(&n);
        status_t st = send_and_read(a, b, n);
        if (st == OK) {
            taken++;
            if (!took_good())
                FAIL("round %u: a hand-over taken with a bad descriptor", round);
        } else if (st != ERR_INVALID_ARGS || col.n) {
            FAIL("round %u: read gave %s, %u kept", round, status_str(st), col.n);
        }
        pfd_collect_close(&col);
        if (pfd_queued(b) || pfd_handles() != before)
            FAIL("round %u: %u queued, %ld handles more", round, pfd_queued(b),
                 (long)(pfd_handles() - before));
    }
    printf("utest: posixfds_fuzz: %u of %u taken\n", taken, FUZZ_ROUNDS);
    CHECK(taken > 0 && taken < FUZZ_ROUNDS);
    jam_handle_close(a);
    jam_handle_close(b);
    return true;
}

/* ---- the writer ---------------------------------------------------------------------- */

/* Write out with one descriptor of kind (its handles fresh) changed by
 * fix: the status, every handle closed, nothing left queued on b. */
static status_t write_one(handle_t a, uint16_t kind, int32_t fd, const char *data,
                          const char *cwd, uint64_t sigmask)
{
    static handle_t hs[2][POSIXFDS_KIND_HANDLES];
    status_t st = pfd_make(POSIXFDS_PIPE, hs[0]);
    if (st == OK && posixfds_kind_handles(kind, NULL) > 0)
        st = pfd_make(kind, hs[1]);
    if (st != OK)
        return ERR_INTERNAL;
    struct posixfds_desc d[2] = {
        { .fd = 3, .kind = POSIXFDS_PIPE, .handles = hs[0] },
        { .fd = fd, .kind = kind, .data = data, .data_len = data ? (uint32_t)strlen(data) + 1 : 0,
          .handles = posixfds_kind_handles(kind, NULL) > 0 ? hs[1] : NULL },
    };
    struct posixfds_out out = { .state = { .sigmask = sigmask }, .cwd = cwd, .fds = d,
                                .nfds = 2 };
    return posixfds_write(a, &out);
}

bool t_posixfds_writer_refuses(void)
{
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    uint64_t before = pfd_handles();
    CHECK_ST(write_one(a, POSIXFDS_DIR, 4, "/boot", "/", 0), OK);   /* the good one */
    struct posixfds_sink sink = pfd_sink(&col);
    struct posixfds_state s;
    static char cwd[POSIXFDS_PATH_MAX];
    CHECK_ST(posixfds_read(b, &sink, &s, cwd), OK);
    CHECK_EQ(col.n, 2u);
    pfd_collect_close(&col);
    CHECK_ST(write_one(a, POSIXFDS_DIR, 3, "/boot", "/", 0), ERR_INVALID_ARGS);   /* fd twice */
    CHECK_ST(write_one(a, POSIXFDS_DIR, 4, "boot", "/", 0), ERR_INVALID_ARGS);
    CHECK_ST(write_one(a, POSIXFDS_DEV, 4, "a/b", "/", 0), ERR_INVALID_ARGS);
    CHECK_ST(write_one(a, POSIXFDS_PIPE, POSIXFDS_FD_MAX, NULL, "/", 0), ERR_INVALID_ARGS);
    CHECK_ST(write_one(a, POSIXFDS_KIND_COUNT, 4, NULL, "/", 0), ERR_INVALID_ARGS);
    CHECK_ST(write_one(a, POSIXFDS_PIPE, 4, NULL, "data", 0), ERR_INVALID_ARGS);
    CHECK_ST(write_one(a, POSIXFDS_PIPE, 4, NULL, "/", POSIXFDS_SIG(9)), ERR_INVALID_ARGS);
    CHECK_EQ(pfd_queued(b), 0u);
    CHECK_EQ(pfd_handles(), before);

    /* A fault in a later message: the first went, the reader refuses the
     * rest; every handle closed either side. */
    enum { N = 40 };
    static struct posixfds_desc d[N];
    static handle_t hs[N][POSIXFDS_KIND_HANDLES];
    for (unsigned i = 0; i < N; i++) {
        CHECK_ST(pfd_make(POSIXFDS_FILE, hs[i]), OK);
        d[i] = (struct posixfds_desc){ .fd = (int32_t)i, .kind = POSIXFDS_FILE, .handles = hs[i] };
    }
    d[N - 1].fd = 0;   /* in the second message (80 handles) */
    static uint8_t dirch[64];   /* and the directory's channel in the first: closed too */
    handle_t dir, peer;
    CHECK_ST(jam_channel_create(&dir, &peer), OK);
    jam_handle_close(peer);
    struct posixfds_out out = { .cwd_dir = dir, .cwd_data = dirch,
                                .cwd_data_len = pfd_dirch(dirch, 1, "/data", NULL),
                                .fds = d, .nfds = N };
    CHECK_ST(posixfds_write(a, &out), ERR_INVALID_ARGS);
    CHECK_EQ(pfd_queued(b), 1u);
    CHECK_ST(posixfds_read(b, &sink, &s, cwd), ERR_INVALID_ARGS);
    CHECK_EQ(pfd_queued(b), 0u);
    CHECK_EQ(col.n, 0u);
    CHECK_EQ(pfd_handles(), before);

    /* More descriptors than there are numbers. */
    static struct posixfds_desc many[POSIXFDS_FD_MAX + 1];
    for (unsigned i = 0; i <= POSIXFDS_FD_MAX; i++)
        many[i] = (struct posixfds_desc){ .fd = (int32_t)i, .kind = POSIXFDS_DEV, .data = "null",
                                          .data_len = 5 };
    out = (struct posixfds_out){ .cwd = "/", .fds = many, .nfds = POSIXFDS_FD_MAX + 1 };
    CHECK_ST(posixfds_write(a, &out), ERR_OUT_OF_RANGE);
    CHECK_EQ(pfd_queued(b), 0u);
    jam_handle_close(a);
    jam_handle_close(b);
    return true;
}
