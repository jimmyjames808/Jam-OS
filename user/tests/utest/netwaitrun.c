/* utest: wait sets (<netwait.h>) under traffic, over 64 fake sockets
 * (fakesock.c), half datagrams and half byte streams:
 *   - netwait_level: the test plays netstack and the program in turn, at
 *     random (data in, data taken, ends, interests changed, entries taken
 *     out and put back), and after each turn one look must report exactly
 *     what the rings say: every ready socket, with every bit, and no other.
 *     A socket left ready is reported again with no new signal (level), and
 *     one whose wake was lost would be missing;
 *   - netwait_stress: a fake netstack thread sends and takes at random,
 *     sleeping now and then, while the test blocks in the set and moves
 *     everything; a lost wake leaves the test asleep with data waiting and
 *     shows as a timeout. Entries are taken out and put back meanwhile, and
 *     netstack dies under one socket half way. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <netwait.h>
#include <os.h>
#include "fakesock.h"
#include "nettest.h"
#include "utest.h"

static struct fnet net;   /* 64 sockets: too big for a stack frame */

/* ---- one thread, every turn checked -------------------------------------------------------- */

#define LEVEL_TURNS 1500u

/* netstack's part of a turn: a few random moves. */
static void level_stack(uint64_t *seed, uint32_t turn)
{
    for (uint32_t a = fuzz_rand(seed) % 6; a; a--) {
        struct fsock *k = &net.sk[fuzz_rand(seed) % net.n];
        uint32_t what = fuzz_rand(seed) % 10;
        if (what < 5)
            (void)stack_rx(k, fsock_dgram(k) ? 1 + fuzz_rand(seed) % 4 : 1 + fuzz_rand(seed) % 3000,
                           UINT64_MAX);
        else if (what < 8)
            (void)stack_tx(k);
        else if (what == 8 && !fsock_dgram(k) && turn > LEVEL_TURNS / 2 && fuzz_rand(seed) % 8 == 0)
            stack_rx_end(k);   /* a few streams end: readable for good */
    }
}

/* The program's part of a turn before the look: change an interest, or
 * take an entry out and put it back. */
static bool level_prog_set(struct netwait *w, uint64_t *seed)
{
    struct fsock *k = &net.sk[fuzz_rand(seed) % net.n];
    uint32_t interest = fuzz_rand(seed) % 4;   /* 0, READ, WRITE or both */
    uint32_t what = fuzz_rand(seed) % 32;
    if (what < 2) {
        CHECK_ST(netwait_modify(w, k->id, interest), OK);
        k->interest = interest;
    } else if (what == 2) {
        CHECK(fsock_take_out(w, k) && fsock_add(w, k, interest));
    }
    return true;
}

/* ... and after it: move what was reported, some of it or all. */
static void level_prog_move(uint64_t *seed, const uint32_t *bits)
{
    for (uint32_t i = 0; i < net.n; i++) {
        struct fsock *k = &net.sk[i];
        uint32_t how = fuzz_rand(seed) % 3;
        if ((bits[i] & NETWAIT_READ) && how < 2)
            (void)prog_read(k, how ? UINT32_MAX : 1 + fuzz_rand(seed) % 2000);
        uint32_t n = fsock_dgram(k) ? 1 + fuzz_rand(seed) % 3 : 1 + fuzz_rand(seed) % 2000;
        if ((bits[i] & NETWAIT_WRITE) && fuzz_rand(seed) % 2)
            (void)prog_write(k, n);
    }
}

static bool level_run(struct netwait *w)
{
    uint64_t seed = 0x6e657477u;
    uint32_t bits[FAKE_SOCKS], reported = 0;
    status_t errs[FAKE_SOCKS], st;
    for (uint32_t i = 0; i < net.n; i++)
        CHECK(fsock_add(w, &net.sk[i], fuzz_rand(&seed) % 4));
    for (uint32_t turn = 0; turn < LEVEL_TURNS; turn++) {
        level_stack(&seed, turn);
        CHECK(level_prog_set(w, &seed));
        CHECK(fsock_gather(w, 0, bits, errs, &st));
        CHECK(st == OK || st == ERR_TIMED_OUT);
        for (uint32_t i = 0; i < net.n; i++) {
            uint32_t want = fsock_expect(&net.sk[i], net.sk[i].interest);
            if (bits[i] != want)
                FAIL("turn %u, socket %u (interest %u): reported %#x, the rings say %#x", turn, i,
                     net.sk[i].interest, bits[i], want);
            reported += bits[i] != 0;
        }
        level_prog_move(&seed, bits);
    }
    for (uint32_t i = 0; i < net.n; i++)
        CHECK(!net.sk[i].bad_rx && !net.sk[i].bad_tx);
    struct netwait_stats s;
    netwait_get_stats(w, &s);
    printf("utest: netwait: %u turns, %u reports, %lu looks, %lu arms, %lu packets (%lu stale)\n",
           LEVEL_TURNS, reported, (unsigned long)s.looks, (unsigned long)s.arms,
           (unsigned long)s.packets, (unsigned long)s.stale);
    return true;
}

bool t_netwait_level(void)
{
    struct netwait *w = NULL;
    bool ok = fnet_open(&net, FAKE_SOCKS) && netwait_create(FAKE_SOCKS, &w) == OK &&
              level_run(w);
    netwait_destroy(w);
    fnet_close(&net);
    CHECK(ok);
    return true;
}

/* ---- a netstack thread against a blocked program -------------------------------------------- */

#define RX_DGRAMS  160u                 /* datagrams netstack sends a datagram socket */
#define RX_BYTES   (40u * 1024)         /* bytes it sends a stream socket, then its end */
#define TX_DGRAMS  80u                  /* what the program sends */
#define TX_BYTES   (20u * 1024)
#define KILLED     61u                  /* netstack dies under this socket half way */
#define STRESS_FOR (60 * NS_PER_S)      /* the whole test, at most */

static bool stress_stop;                /* set by the program (atomic) */
static bool stress_done;                /* set by netstack: all sent and taken (atomic) */

static uint64_t rx_total(const struct fsock *k)
{
    return fsock_dgram(k) ? RX_DGRAMS : RX_BYTES;
}

static uint64_t tx_total(const struct fsock *k)
{
    return fsock_dgram(k) ? TX_DGRAMS : TX_BYTES;
}

/* netstack's view: is everything sent (and ended) and taken? */
static bool stack_finished(void)
{
    for (uint32_t i = 0; i < net.n; i++) {
        struct fsock *k = &net.sk[i];
        if (k->dead)
            continue;
        if (k->rx_made < rx_total(k) || (!fsock_dgram(k) && !k->s.rx.ended) ||
            k->tx_took < tx_total(k))
            return false;
    }
    return true;
}

/* One random socket's turn; true if anything moved. */
static bool stack_turn(uint64_t *seed)
{
    struct fsock *k = &net.sk[fuzz_rand(seed) % net.n];
    if (k->dead)
        return false;
    bool moved = stack_tx(k) != 0;
    if (k->rx_made < rx_total(k))
        moved |= stack_rx(k, fsock_dgram(k) ? 1 + fuzz_rand(seed) % 3 : 1 + fuzz_rand(seed) % 3000,
                          rx_total(k)) != 0;
    else if (!fsock_dgram(k) && !k->s.rx.ended)
        stack_rx_end(k);
    if (k->idx == KILLED && k->rx_made >= rx_total(k) / 2)
        stack_kill(k);
    return moved;
}

static void stress_stack(void *arg)
{
    uint64_t seed = (uintptr_t)arg;
    uint32_t idle = 0;
    for (uint32_t turn = 0; !__atomic_load_n(&stress_stop, __ATOMIC_ACQUIRE); turn++) {
        if (stack_turn(&seed)) {
            idle = 0;
            if (fuzz_rand(&seed) % 16 == 0)   /* now and then: the program catches up and sleeps */
                (void)jam_nanosleep(now() + fuzz_rand(&seed) % 200 * NS_PER_US);
        } else if (++idle >= 2 * net.n) {
            idle = 0;
            (void)jam_nanosleep(now() + 100 * NS_PER_US);
        }
        if (turn % 256 == 0 && stack_finished())
            break;
    }
    __atomic_store_n(&stress_done, true, __ATOMIC_RELEASE);
}

/* What the program still wants from k: 0 when it is done with it. */
static uint32_t prog_wants(struct fsock *k)
{
    bool rx_done = k->rx_got == rx_total(k) && (fsock_dgram(k) || sockring_at_end(&k->p.rx));
    return (rx_done ? 0 : NETWAIT_READ) | (k->tx_made < tx_total(k) ? NETWAIT_WRITE : 0);
}

/* One reported entry: check the report against the rings, move, and keep
 * the interest to what is still wanted. *done counts finished sockets. */
static bool stress_entry(struct netwait *w, const struct netwait_ready *r, uint64_t *seed,
                         uint32_t *done)
{
    struct fsock *k = r->user;
    CHECK(k && k->id == r->id);
    if (r->ready & (NETWAIT_HUP | NETWAIT_ERROR)) {
        CHECK(k->idx == KILLED && r->ready == (NETWAIT_HUP | NETWAIT_ERROR));
        CHECK_ST(r->error, ERR_PEER_CLOSED);
        (*done)++;
        return fsock_take_out(w, k);
    }
    if (r->ready & NETWAIT_READ) {
        CHECK(sockring_ready(&k->p.rx) || k->p.rx.ended);   /* never reported empty */
        (void)prog_read(k, UINT32_MAX);
    }
    if (r->ready & NETWAIT_WRITE) {
        uint32_t need = fsock_dgram(k) ? sockring_dgram_bytes(SOCKRING_DGRAM_MAX) : 1;
        CHECK(sockring_room(&k->p.tx) >= need);             /* never reported full */
        uint64_t left = tx_total(k) - k->tx_made;
        uint32_t want = fsock_dgram(k) ? 1 + fuzz_rand(seed) % 3 : 1 + fuzz_rand(seed) % 3000;
        (void)prog_write(k, want < left ? want : (uint32_t)left);
    }
    uint32_t wants = prog_wants(k);
    if (!wants) {
        (*done)++;
        return fsock_take_out(w, k);
    }
    if (wants != k->interest) {
        CHECK_ST(netwait_modify(w, k->id, wants), OK);
        k->interest = wants;
    }
    return true;
}

/* Take a random socket out and put it back, while netstack runs. */
static bool stress_shuffle(struct netwait *w, uint64_t *seed)
{
    struct fsock *k = &net.sk[fuzz_rand(seed) % net.n];
    if (!k->id)
        return true;
    uint32_t interest = k->interest;
    return fsock_take_out(w, k) && fsock_add(w, k, interest);
}

static bool stress_run(struct netwait *w, uint32_t *waits)
{
    static struct netwait_ready got[FAKE_SOCKS];
    uint64_t seed = 0x57e55u, end = now() + STRESS_FOR;
    uint32_t done = 0, n = 0;
    for (uint32_t i = 0; i < net.n; i++)
        CHECK(fsock_add(w, &net.sk[i], NETWAIT_INTEREST));
    while (done < net.n) {
        status_t st = netwait_wait(w, now() + 5 * NS_PER_S, got, FAKE_SOCKS, &n);
        if (st == ERR_TIMED_OUT)
            FAIL("asleep 5 s with %u sockets not done: a lost wake?", net.n - done);
        CHECK_ST(st, OK);
        CHECK(now() < end);
        for (uint32_t j = 0; j < n; j++)
            CHECK(stress_entry(w, &got[j], &seed, &done));
        if (++*waits % 50 == 0)
            CHECK(stress_shuffle(w, &seed));
    }
    return true;
}

static bool stress_check(struct netwait *w, uint32_t waits)
{
    for (uint32_t i = 0; i < net.n; i++) {
        struct fsock *k = &net.sk[i];
        CHECK(!k->bad_rx && !k->bad_tx);
        if (k->idx != KILLED)
            CHECK(k->rx_got == rx_total(k) && k->tx_took == tx_total(k));
    }
    CHECK(net.sk[KILLED].dead);
    struct netwait_stats s;
    netwait_get_stats(w, &s);
    CHECK(s.blocks > 0);   /* the program slept and was woken */
    printf("utest: netwait: stress %u waits, %lu slept, %lu packets (%lu stale), %lu looks, "
           "%lu arms\n", waits, (unsigned long)s.blocks, (unsigned long)s.packets,
           (unsigned long)s.stale, (unsigned long)s.looks, (unsigned long)s.arms);
    return true;
}

bool t_netwait_stress(void)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    struct netwait *w = NULL;
    handle_t th = HANDLE_INVALID;
    uint32_t waits = 0;
    stress_stop = stress_done = false;
    bool ok = fnet_open(&net, FAKE_SOCKS) && netwait_create(FAKE_SOCKS, &w) == OK &&
              thread_spawn("netwait-stack", stress_stack, (void *)(uintptr_t)0x5eed, stack,
                           sizeof(stack), &th) == OK;
    ok = ok && stress_run(w, &waits);
    /* Everything came; netstack still takes the last of what was sent. */
    for (uint64_t until = now() + 5 * NS_PER_S;
         ok && !__atomic_load_n(&stress_done, __ATOMIC_ACQUIRE) && now() < until;)
        (void)jam_nanosleep(now() + NS_PER_MS);
    __atomic_store_n(&stress_stop, true, __ATOMIC_RELEASE);
    bool joined = th == HANDLE_INVALID || wait_threads(&th, 1);
    ok = ok && joined && stress_check(w, waits);
    netwait_destroy(w);
    fnet_close(&net);
    CHECK(ok);
    return true;
}
