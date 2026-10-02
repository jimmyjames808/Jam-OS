/* utest: the netdev rings' code (<jam/netdev.h>), which a network driver
 * and netstack share: the counts (empty, full, wrapping), a hostile peer
 * (counts out of range, bad slot lengths and flags: the side that reads
 * them never goes outside the ring and never trusts its own field back),
 * the sleep/wake flags, the VLAN word, and a whole exchange between a fake
 * driver thread and the test as netstack, over real VMOs and events with
 * the rights netdev.open hands out. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/driver.h>
#include <jam/netdev.h>
#include <os.h>
#include "utest.h"

/* ---- helpers -------------------------------------------------------------------- */

/* A ring VMO mapped read-write in this process; *map its address. */
static status_t ring_map(handle_t vmo, void **map)
{
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, NETDEV_RING_BYTES,
                               VMAR_READ | VMAR_WRITE, &va);
    if (st == OK)
        *map = (void *)(uintptr_t)va;
    return st;
}

static void ring_unmap(void *map)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map,
                         NETDEV_RING_BYTES);   /* nothing to do if it fails */
}

/* A new ring VMO, mapped. */
static status_t ring_new(handle_t *vmo, void **map)
{
    status_t st = jam_vmo_create(NETDEV_RING_BYTES, 0, HANDLE_INVALID, vmo);
    if (st != OK)
        return st;
    st = ring_map(*vmo, map);
    if (st != OK)
        jam_handle_close(*vmo);
    return st;
}

/* Frame i of a test stream: 14..1514 bytes, every length over the run. */
static uint32_t frame_len(uint64_t i)
{
    return NETDEV_FRAME_MIN + (uint32_t)(i * 37 % (NETDEV_FRAME_MAX - NETDEV_FRAME_MIN + 1));
}

static void frame_fill(uint64_t i, uint8_t *f)
{
    for (uint32_t k = 0; k < frame_len(i); k++)
        f[k] = (uint8_t)(i * 7 + k);
}

static bool frame_ok(uint64_t i, const uint8_t *f, uint32_t len)
{
    if (len != frame_len(i))
        return false;
    for (uint32_t k = 0; k < len; k++)
        if (f[k] != (uint8_t)(i * 7 + k))
            return false;
    return true;
}

/* ---- the counts, as pure functions ---------------------------------------------------- */

bool t_netdev_ring_counts(void)
{
    bool bad = false;
    /* empty, part full, full, and the same far into the stream (wrapped) */
    CHECK_EQ(netdev_ring_ready(0, 0, &bad), 0);
    CHECK_EQ(netdev_ring_ready(5, 2, &bad), 3);
    CHECK_EQ(netdev_ring_ready(NETDEV_SLOTS, 0, &bad), NETDEV_SLOTS);
    CHECK_EQ(netdev_ring_ready(1000003, 1000000, &bad), 3);
    CHECK_EQ(netdev_ring_room(0, 0, &bad), NETDEV_SLOTS);
    CHECK_EQ(netdev_ring_room(NETDEV_SLOTS, 0, &bad), 0);
    CHECK_EQ(netdev_ring_room(1000003, 1000000, &bad), NETDEV_SLOTS - 3);
    CHECK(!bad);
    /* A producer's count that went backwards: nothing to take. */
    CHECK_EQ(netdev_ring_ready(9, 10, &bad), 0);
    CHECK(bad);
    /* More than a ring ahead, up to the largest count: clamped to a ring. */
    bad = false;
    CHECK_EQ(netdev_ring_ready(10 + NETDEV_SLOTS + 1, 10, &bad), NETDEV_SLOTS);
    CHECK(bad);
    bad = false;
    CHECK_EQ(netdev_ring_ready(UINT64_MAX, 0, &bad), NETDEV_SLOTS);
    CHECK(bad);
    /* A consumer's count ahead of what was produced, or more than a ring
     * behind (both impossible from an honest consumer): no room at all. */
    bad = false;
    CHECK_EQ(netdev_ring_room(10, 11, &bad), 0);
    CHECK(bad);
    bad = false;
    CHECK_EQ(netdev_ring_room(10, UINT64_MAX, &bad), 0);
    CHECK(bad);
    bad = false;
    CHECK_EQ(netdev_ring_room(1000, 1000 - NETDEV_SLOTS - 1, &bad), 0);
    CHECK(bad);
    bad = false;
    CHECK_EQ(netdev_ring_room(1000, 1000 - NETDEV_SLOTS, &bad), 0);   /* honestly full */
    CHECK(!bad);
    return true;
}

/* ---- one ring, both ends in this thread ------------------------------------------------- */

/* Frames through one ring past its end several times: every frame comes
 * out as it went in, the ring is full at NETDEV_SLOTS and empty after. */
static bool exchange(struct netdev_end *p, struct netdev_end *c)
{
    static uint8_t f[NETDEV_FRAME_MAX], g[NETDEV_FRAME_MAX];
    CHECK_EQ(netdev_ready(c), 0);
    for (unsigned i = 0; i < NETDEV_SLOTS; i++) {
        CHECK_EQ(netdev_room(p), NETDEV_SLOTS - i);
        frame_fill(i, f);
        netdev_put(p, f, frame_len(i));
    }
    CHECK_EQ(netdev_room(p), 0);              /* by its own count: full */
    CHECK_EQ(netdev_ready(c), 0);             /* not published yet */
    CHECK(!netdev_publish(p));                /* nobody waits */
    CHECK_EQ(netdev_ready(c), NETDEV_SLOTS);
    uint64_t next = 0;
    for (uint64_t sent = NETDEV_SLOTS; next < 5 * NETDEV_SLOTS + 7;) {
        uint32_t n = netdev_ready(c), len = 0;
        for (uint32_t k = 0; k < n; k++, next++) {
            CHECK_ST(netdev_take(c, g, sizeof(g), &len), OK);
            CHECK(frame_ok(next, g, len));
        }
        (void)netdev_publish(c);
        for (uint32_t k = netdev_room(p); k > 0 && sent < 5 * NETDEV_SLOTS + 7; k--, sent++) {
            frame_fill(sent, f);
            netdev_put(p, f, frame_len(sent));
        }
        (void)netdev_publish(p);
    }
    CHECK_EQ(netdev_ready(c), 0);
    CHECK_EQ(netdev_room(p), NETDEV_SLOTS);
    CHECK(!p->errors && !c->errors);
    return true;
}

/* A hostile producer: counts out of range and bad slots. The consumer
 * stays inside the ring, skips and reports bad slots without copying,
 * and never reads its own count back from the shared header. */
static bool hostile_producer(struct netdev_end *c)
{
    struct netdev_ring *h = c->hdr;
    uint8_t dst[64 + 8];
    uint32_t len = 0;
    c->count = 300;
    h->consumed = 7;                                  /* its own field, scribbled on */
    h->produced = 299;                                /* backwards */
    CHECK_EQ(netdev_ready(c), 0);
    CHECK_EQ(c->errors, 1);
    h->produced = 300 + NETDEV_SLOTS + 50;            /* too far ahead */
    CHECK_EQ(netdev_ready(c), NETDEV_SLOTS);
    CHECK_EQ(c->errors, 2);
    /* Bad lengths and flags: refused, nothing copied, the slot passed. */
    static const uint32_t lens[] = { 0, NETDEV_FRAME_MIN - 1, NETDEV_FRAME_MAX + 1,
                                     NETDEV_SLOT_SIZE, 0xffffffffu, 65 };
    for (unsigned i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        struct netdev_slot *s = netdev_slot_of(c, c->count);
        s->len = lens[i];
        s->flags = 0;
        memset(dst, 0xa5, sizeof(dst));
        uint64_t before = c->count;
        CHECK_ST(netdev_take(c, dst, 64, &len), ERR_OUT_OF_RANGE);   /* 65 > cap 64 too */
        CHECK_EQ(len, lens[i]);
        CHECK_EQ(c->count, before + 1);
        for (unsigned k = 0; k < sizeof(dst); k++)
            CHECK_EQ(dst[k], 0xa5);
    }
    struct netdev_slot *s = netdev_slot_of(c, c->count);
    s->len = 20;
    s->flags = 1;
    CHECK_ST(netdev_take(c, dst, 64, &len), ERR_INVALID_ARGS);
    s = netdev_slot_of(c, c->count);
    s->len = NETDEV_FRAME_MIN;
    s->flags = 0;
    memset(s->frame, 0x42, NETDEV_FRAME_MIN);
    CHECK_ST(netdev_take(c, dst, 64, &len), OK);
    CHECK(len == NETDEV_FRAME_MIN && dst[0] == 0x42 && dst[13] == 0x42 && dst[14] == 0xa5);
    /* The consumer publishes its own count, not what was in the header. */
    (void)netdev_publish(c);
    CHECK_EQ(h->consumed, 300 + 8);
    return true;
}

/* A hostile consumer: the producer's room from a consumer count out of
 * range is none (it never overwrites a slot it may not), and it never
 * reads its own count back. */
static bool hostile_consumer(struct netdev_end *p)
{
    struct netdev_ring *h = p->hdr;
    p->count = 1000;
    h->produced = 3;                                  /* its own field, scribbled on */
    h->consumed = 1001;
    CHECK_EQ(netdev_room(p), 0);
    h->consumed = UINT64_MAX;
    CHECK_EQ(netdev_room(p), 0);
    h->consumed = 1000 - NETDEV_SLOTS - 1;
    CHECK_EQ(netdev_room(p), 0);
    CHECK_EQ(p->errors, 3);
    h->consumed = 990;
    CHECK_EQ(netdev_room(p), NETDEV_SLOTS - 10);
    CHECK_EQ(p->errors, 3);
    (void)netdev_publish(p);
    CHECK_EQ(h->produced, 1000);
    return true;
}

/* The flags: a side about to sleep raises its own and looks again; the
 * peer's publish then says "signal". */
static bool wake_flags(struct netdev_end *p, struct netdev_end *c)
{
    p->count = c->count = 0;
    p->hdr->produced = p->hdr->consumed = 0;
    CHECK(netdev_sleep(c));                    /* empty: sleep, flag up */
    CHECK_EQ(c->hdr->consumer_waits, 1);
    uint8_t f[NETDEV_FRAME_MIN] = { 0 };
    netdev_put(p, f, sizeof(f));
    CHECK(netdev_publish(p));                  /* the consumer waits: signal it */
    netdev_awake(c);
    CHECK_EQ(c->hdr->consumer_waits, 0);
    CHECK(!netdev_sleep(c));                   /* a frame is there: no sleep, flag down */
    CHECK_EQ(c->hdr->consumer_waits, 0);
    for (unsigned i = 1; i < NETDEV_SLOTS; i++)
        netdev_put(p, f, sizeof(f));
    (void)netdev_publish(p);
    CHECK(netdev_sleep(p));                    /* full: the producer sleeps */
    CHECK_EQ(p->hdr->producer_waits, 1);
    uint32_t len = 0;
    CHECK_ST(netdev_take(c, f, sizeof(f), &len), OK);
    CHECK(netdev_publish(c));                  /* room, and the producer waits */
    CHECK(!netdev_sleep(p));
    CHECK_EQ(p->hdr->producer_waits, 0);
    return true;
}

bool t_netdev_ring_one_thread(void)
{
    handle_t vmo = HANDLE_INVALID;
    void *map = NULL;
    CHECK_ST(ring_new(&vmo, &map), OK);
    struct netdev_end p, c;
    netdev_end_make(&p, map, NETDEV_RING_TX, true);
    /* What a consumer accepts as a ring: this layout, this kind. */
    CHECK(!netdev_end_attach(&c, map, NETDEV_RING_RX, false));
    CHECK(netdev_end_attach(&c, map, NETDEV_RING_TX, false));
    bool ok = exchange(&p, &c) && wake_flags(&p, &c) && hostile_producer(&c) &&
              hostile_consumer(&p);
    struct netdev_ring *h = (struct netdev_ring *)map;
    h->slots = NETDEV_SLOTS * 2;
    CHECK(!netdev_end_attach(&c, map, NETDEV_RING_TX, false));
    h->slots = NETDEV_SLOTS;
    h->magic ^= 1;
    CHECK(!netdev_end_attach(&c, map, NETDEV_RING_TX, false));
    ring_unmap(map);
    CHECK_ST(jam_handle_close(vmo), OK);
    return ok;
}

bool t_netdev_vlan_word(void)
{
    CHECK_EQ(netdev_vlan_word("vlan=21"), 21);
    CHECK_EQ(netdev_vlan_word("vlan=1"), 1);
    CHECK_EQ(netdev_vlan_word("vlan=4094"), 4094);
    static const char *const bad[] = { NULL, "", "vlan", "vlan=", "vlan=0", "vlan=4095",
                                       "vlan=off", "vlan=21x", "vlan=-1", "vlan=00021",
                                       "vlan=65557", "vlanx=21", " vlan=21", "VLAN=21" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK_EQ(netdev_vlan_word(bad[i]), 0);
    const char *one[] = { "netprobe", "vlan=21" };
    CHECK_EQ(netdev_vlan_args(one, 2), 21);
    CHECK_EQ(netdev_vlan_args(one, 1), 0);     /* none */
    const char *two[] = { "vlan=21", "vlan=21" };
    CHECK_EQ(netdev_vlan_args(two, 2), 21);
    const char *differ[] = { "vlan=21", "vlan=22" };
    CHECK_EQ(netdev_vlan_args(differ, 2), 0);
    const char *off[] = { "vlan=21", "vlan=off" };
    CHECK_EQ(netdev_vlan_args(off, 2), 0);
    return true;
}

/* ---- a fake driver and the test as netstack --------------------------------------------- */

#define XCHG_FRAMES 4000u   /* over 15 times round the rings */
#define XCHG_WAIT   (5 * NS_PER_S)

/* The fake driver's side: what a driver keeps. */
struct fake_driver {
    handle_t tx_vmo, rx_vmo;     /* its own handles, all rights */
    handle_t to_driver, to_stack;
    struct netdev_end tx, rx;    /* consumer of tx, producer of rx */
    bool stop;                   /* set by the test: end the loop */
    uint32_t held, refused;      /* passes cut short by a full rx ring / bad tx slots */
    uint32_t signals;            /* to_stack signals sent */
    status_t st;                 /* how the loop ended */
};

/* One pass over the tx ring: echo each frame into the rx ring. A real
 * driver drops a frame the rx ring has no room for; this one, so every
 * frame can be checked, leaves it in the tx ring for a later pass
 * (returns false: come back soon, there is no wake for rx room). */
static bool driver_pass(struct fake_driver *d, uint8_t *buf)
{
    uint32_t n = netdev_ready(&d->tx), room = netdev_room(&d->rx), len = 0;
    bool all = n <= room;
    if (!all)
        n = room;
    for (uint32_t i = 0; i < n; i++) {
        if (netdev_take(&d->tx, buf, NETDEV_FRAME_MAX, &len) == OK)
            netdev_put(&d->rx, buf, len);
        else
            d->refused++;
    }
    if (n && netdev_publish(&d->rx)) {
        d->signals++;
        (void)drv_event_signal(d->to_stack, 0, NETDEV_SIG_RX);
    }
    if (n && netdev_publish(&d->tx)) {
        d->signals++;
        (void)drv_event_signal(d->to_stack, 0, NETDEV_SIG_TX_ROOM);
    }
    d->held += !all;
    return all;
}

static void driver_thread(void *arg)
{
    struct fake_driver *d = arg;
    static uint8_t buf[NETDEV_FRAME_MAX];
    /* Start late, so the test fills the tx ring and waits for room. */
    (void)jam_nanosleep(now() + 20 * NS_PER_MS);
    for (unsigned pass = 0; !__atomic_load_n(&d->stop, __ATOMIC_ACQUIRE); pass++) {
        (void)drv_event_signal(d->to_driver, NETDEV_SIG_TX, 0);
        netdev_awake(&d->tx);
        bool all = driver_pass(d, buf);
        if (!all || pass % 16 == 15)
            (void)jam_nanosleep(now() + NS_PER_MS);   /* a slow moment: the tx ring fills */
        if (!all || !netdev_sleep(&d->tx))
            continue;
        signals_t seen = 0;
        status_t st = jam_object_wait_one(d->to_driver, NETDEV_SIG_TX, now() + XCHG_WAIT, &seen);
        if (st != OK && !__atomic_load_n(&d->stop, __ATOMIC_ACQUIRE)) {
            d->st = st;
            return;
        }
    }
    d->st = OK;
}

/* The test's side: netstack's handles, with netdev.open's rights. */
struct fake_stack {
    handle_t to_driver, to_stack;
    void *tx_map, *rx_map;
    struct netdev_end tx, rx;    /* producer of tx, consumer of rx */
    uint64_t sent, got;          /* frames written / read back */
    uint32_t signals, room_waits;
};

/* What netdev.open would hand netstack, from the fake driver's handles. */
static bool stack_handles(struct fake_driver *d, struct fake_stack *s)
{
    handle_t tx = HANDLE_INVALID, rx = HANDLE_INVALID;
    CHECK_ST(jam_handle_duplicate(d->tx_vmo, NETDEV_RING_RIGHTS, &tx), OK);
    CHECK_ST(jam_handle_duplicate(d->rx_vmo, NETDEV_RING_RIGHTS, &rx), OK);
    CHECK_ST(jam_handle_duplicate(d->to_driver, NETDEV_TO_DRIVER_RIGHTS, &s->to_driver), OK);
    CHECK_ST(jam_handle_duplicate(d->to_stack, NETDEV_TO_STACK_RIGHTS, &s->to_stack), OK);
    /* No resize (the driver's mapping is safe), no duplicate. */
    CHECK_ST(jam_vmo_set_size(tx, 4096), ERR_ACCESS_DENIED);
    handle_t dup = HANDLE_INVALID;
    CHECK_ST(jam_handle_duplicate(rx, RIGHT_SAME, &dup), ERR_ACCESS_DENIED);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(s->to_driver, NETDEV_SIG_TX, 0, &seen), ERR_ACCESS_DENIED);
    /* Mapped at addresses of its own, as netstack would. */
    CHECK_ST(ring_map(tx, &s->tx_map), OK);
    CHECK_ST(ring_map(rx, &s->rx_map), OK);
    jam_handle_close(tx);
    jam_handle_close(rx);
    CHECK(netdev_end_attach(&s->tx, s->tx_map, NETDEV_RING_TX, true));
    CHECK(netdev_end_attach(&s->rx, s->rx_map, NETDEV_RING_RX, false));
    return true;
}

/* Frames in flight at most (in the two rings): more than the tx ring
 * holds, so a slow fake driver lets it fill. */
#define XCHG_WINDOW (NETDEV_SLOTS + NETDEV_SLOTS / 4)

/* Write what fits, read what came back. True if anything moved. */
static bool stack_step(struct fake_stack *s, bool *ok)
{
    static uint8_t f[NETDEV_FRAME_MAX];
    uint32_t room = netdev_room(&s->tx), put = 0, len = 0;
    for (; put < room && s->sent < XCHG_FRAMES && s->sent - s->got < XCHG_WINDOW; put++) {
        frame_fill(s->sent, f);
        netdev_put(&s->tx, f, frame_len(s->sent++));
    }
    if (put && netdev_publish(&s->tx)) {
        s->signals++;
        (void)jam_event_signal(s->to_driver, 0, NETDEV_SIG_TX);
    }
    uint32_t n = netdev_ready(&s->rx);
    for (uint32_t k = 0; k < n; k++, s->got++)
        *ok &= netdev_take(&s->rx, f, sizeof(f), &len) == OK && frame_ok(s->got, f, len);
    if (n)
        (void)netdev_publish(&s->rx);   /* the driver never waits for rx room */
    return put || n;
}

/* Nothing moved: sleep until the fake driver signals (or say why not). */
static status_t stack_wait(struct fake_stack *s)
{
    (void)jam_event_signal(s->to_stack, NETDEV_SIG_RX | NETDEV_SIG_TX_ROOM, 0);
    bool want_room = s->sent < XCHG_FRAMES && s->sent - s->got < XCHG_WINDOW;
    bool sleep_rx = netdev_sleep(&s->rx);
    bool sleep_tx = want_room ? netdev_sleep(&s->tx) : true;
    status_t st = OK;
    if (sleep_rx && sleep_tx) {
        s->room_waits += want_room;
        signals_t seen = 0;
        st = jam_object_wait_one(s->to_stack, NETDEV_SIG_RX | NETDEV_SIG_TX_ROOM,
                                 now() + XCHG_WAIT, &seen);
    }
    netdev_awake(&s->rx);
    netdev_awake(&s->tx);
    return st;
}

static bool exchange_run(struct fake_driver *d, struct fake_stack *s)
{
    static uint8_t stack[16384] __attribute__((aligned(16)));
    handle_t th = HANDLE_INVALID;
    CHECK_ST(thread_spawn("netdev-driver", driver_thread, d, stack, sizeof(stack), &th), OK);
    bool ok = true;
    status_t st = OK;
    while (ok && st == OK && s->got < XCHG_FRAMES)
        if (!stack_step(s, &ok))
            st = stack_wait(s);
    __atomic_store_n(&d->stop, true, __ATOMIC_RELEASE);
    (void)jam_event_signal(s->to_driver, 0, NETDEV_SIG_TX);
    bool joined = wait_threads(&th, 1);
    jam_handle_close(th);
    CHECK(joined);
    CHECK_ST(st, OK);
    CHECK(ok);
    CHECK_ST(d->st, OK);
    CHECK_EQ(s->got, XCHG_FRAMES);
    CHECK_EQ(d->refused, 0);
    CHECK(!d->tx.errors && !d->rx.errors && !s->tx.errors && !s->rx.errors);
    CHECK(s->room_waits > 0);   /* the full ring's wait was taken */
    printf("utest: netdev: %u frames each way, %u + %u signals, %u waits for room, %u held\n",
           XCHG_FRAMES, s->signals, d->signals, s->room_waits, d->held);
    return true;
}

bool t_netdev_ring_exchange(void)
{
    struct fake_driver d = { 0 };
    struct fake_stack s = { 0 };
    void *dtx = NULL, *drx = NULL;
    CHECK_ST(ring_new(&d.tx_vmo, &dtx), OK);
    CHECK_ST(ring_new(&d.rx_vmo, &drx), OK);
    CHECK_ST(jam_event_create(&d.to_driver), OK);
    CHECK_ST(jam_event_create(&d.to_stack), OK);
    netdev_end_make(&d.tx, dtx, NETDEV_RING_TX, false);
    netdev_end_make(&d.rx, drx, NETDEV_RING_RX, true);
    bool ok = stack_handles(&d, &s) && exchange_run(&d, &s);
    if (s.tx_map)
        ring_unmap(s.tx_map);
    if (s.rx_map)
        ring_unmap(s.rx_map);
    ring_unmap(dtx);
    ring_unmap(drx);
    handle_t hs[] = { s.to_driver, s.to_stack, d.to_driver, d.to_stack, d.tx_vmo, d.rx_vmo };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (hs[i])
            jam_handle_close(hs[i]);
    return ok;
}
