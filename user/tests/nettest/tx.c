/* nettest vlan: the session rules and the tx ring at its worst (nettest.h).
 * The driver must refuse and count every bad slot exactly and send every
 * good frame tagged VLAN 21; the host checks what actually left. */
#define CHECK_PROG "nettest"
#define CHECK_CUR  cur
#include <check.h>
#include <idl/netdev.h>
#include "nettest.h"

#define RACE_FRAMES 2000u
#define SETTLE      (3 * NS_PER_S)

/* ---- the session rules ----------------------------------------------------------- */

bool t_session(void)
{
    struct netdev_stats a0, a1;
    CHECK(get_stats(nic, &a0));
    struct sess s, t;
    CHECK_ST(sess_open(&s), OK);
    handle_t h[5];
    CHECK_ST(netdev_open_until(nic, soon(), &h[0], &h[1], &h[2], &h[3], &h[4]), ERR_BAD_STATE);
    CHECK_ST(netdev_open_until(s.ch, soon(), &h[0], &h[1], &h[2], &h[3], &h[4]),
             ERR_NOT_SUPPORTED);
    uint8_t m[6], chip[16];
    uint16_t vlan = 0, mtu = 0;
    uint32_t link, speed, changes;
    CHECK_ST(netdev_info_until(s.ch, soon(), m, &vlan, &mtu, &link, &speed, &changes, chip), OK);
    CHECK_EQ(vlan, 21);
    CHECK_EQ(mtu, NETDEV_MTU);
    CHECK(!memcmp(chip, "82574L", 7));
    /* The rights netdev.h names: no duplicate, no resize, to_driver only
     * signals. */
    handle_t x;
    CHECK_ST(jam_handle_duplicate(s.tx, RIGHT_SAME, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_set_size(s.rx, 4096), ERR_ACCESS_DENIED);
    signals_t seen;
    CHECK_ST(jam_object_wait_one(s.to_driver, NETDEV_SIG_TX, 0, &seen), ERR_ACCESS_DENIED);
    /* The opener goes (its channel closed, the rings still held): the
     * next open gets a new session at once. */
    jam_handle_close(s.ch);
    s.ch = 0;
    CHECK_ST(sess_open(&t), OK);
    CHECK(t.txe.hdr->produced == 0 && t.rxe.hdr->produced == 0);
    sess_close(&s);
    sess_close(&t);
    CHECK(get_stats(nic, &a1));
    CHECK_EQ(a1.sessions - a0.sessions, 2);
    return true;
}

/* ---- hostile slots ------------------------------------------------------------------ */

/* A slot written by hand: any length and flags, a 64-byte test frame. */
static void put_raw(struct sess *s, uint32_t len, uint32_t flags, uint16_t type)
{
    struct netdev_slot *slot = netdev_slot_of(&s->txe, s->txe.count);
    frame_make(slot->frame, 64, "nettest-bad", (uint32_t)s->txe.count);
    slot->frame[12] = (uint8_t)(type >> 8);
    slot->frame[13] = (uint8_t)type;
    slot->frame[14] = 0;   /* a TCI as a tagged frame would have: VLAN 21 */
    slot->frame[15] = 21;
    slot->len = len;
    slot->flags = flags;
    s->txe.count++;
}

/* Wait (bounded) until the driver has consumed everything up to want. */
static bool drained(struct sess *s, uint64_t want)
{
    uint64_t end = now() + SETTLE;
    while (__atomic_load_n(&s->txe.hdr->consumed, __ATOMIC_ACQUIRE) != want) {
        if (now() > end)
            FAIL("the driver consumed %lu of %lu", (unsigned long)s->txe.hdr->consumed,
                 (unsigned long)want);
        (void)jam_nanosleep(now() + NS_PER_MS);
    }
    return true;
}

/* Good frames of every interesting length, bad lengths, bad flags and
 * frames already tagged: each refusal counted under its reason. */
static bool every_bad_slot(struct sess *s)
{
    static const uint32_t good[] = { 14, 15, 59, 60, 61, 100, 1000, 1514 };
    static const uint32_t bad[] = { 0, 1, 13, 1515, 1516, 2032, 2048, 0xffffffffu };
    static const uint16_t tags[] = { 0x8100, 0x88a8, 0x9100 };
    struct netdev_stats a, b;
    CHECK(get_stats(nic, &a));
    uint8_t f[NETDEV_FRAME_MAX];
    for (unsigned i = 0; i < 8; i++) {
        frame_make(f, good[i], "nettest-tx", i);
        netdev_put(&s->txe, f, good[i]);
    }
    for (unsigned i = 0; i < 8; i++)
        put_raw(s, bad[i], 0, NT_ETHERTYPE);
    put_raw(s, 60, 1, NT_ETHERTYPE);
    put_raw(s, 60, 0x80000000u, NT_ETHERTYPE);
    for (unsigned i = 0; i < 3; i++)
        put_raw(s, 64, 0, tags[i]);
    tx_kick(s);
    CHECK(drained(s, s->txe.count));
    CHECK(get_stats(nic, &b));
    CHECK_EQ(b.tx_frames - a.tx_frames, 8);
    CHECK_EQ(b.tx_bad_len - a.tx_bad_len, 8);
    CHECK_EQ(b.tx_bad_flags - a.tx_bad_flags, 2);
    CHECK_EQ(b.tx_bad_tag - a.tx_bad_tag, 3);
    return true;
}

/* netstack's produced count a ring and one ahead, then behind: clamped
 * and counted; exactly the frames in the slots are sent, nothing else. */
static bool bad_counts(struct sess *s)
{
    struct netdev_stats a, b, c;
    CHECK(get_stats(nic, &a));
    uint64_t base = s->txe.count;
    for (uint32_t i = 0; i < NETDEV_SLOTS; i++) {
        struct netdev_slot *slot = netdev_slot_of(&s->txe, base + i);
        frame_make(slot->frame, 64, "nettest-jump", i);
        slot->len = 64;
        slot->flags = 0;
    }
    __atomic_store_n(&s->txe.hdr->produced, base + NETDEV_SLOTS + 1, __ATOMIC_RELEASE);
    (void)jam_event_signal(s->to_driver, 0, NETDEV_SIG_TX);
    CHECK(drained(s, base + NETDEV_SLOTS + 1));
    s->txe.count = base + NETDEV_SLOTS + 1;
    CHECK(get_stats(nic, &b));
    CHECK_EQ(b.tx_frames - a.tx_frames, NETDEV_SLOTS + 1);
    CHECK(b.ring_errors > a.ring_errors);
    /* Backwards: nothing taken, an error counted, then put right. */
    __atomic_store_n(&s->txe.hdr->produced, s->txe.count - 3, __ATOMIC_RELEASE);
    (void)jam_event_signal(s->to_driver, 0, NETDEV_SIG_TX);
    (void)jam_nanosleep(now() + 200 * NS_PER_MS);
    CHECK(get_stats(nic, &c));
    CHECK_EQ(c.tx_frames, b.tx_frames);
    CHECK(c.ring_errors > b.ring_errors);
    CHECK_EQ(s->txe.hdr->consumed, s->txe.count);
    tx_kick(s);   /* produced back where it belongs */
    return true;
}

/* ---- the racer -------------------------------------------------------------------- */

static volatile bool race_stop;
static _Alignas(16) uint8_t race_stack[16384];

/* Rewrites bytes 12-13 of every slot, tag and not, while the driver copies. */
static void racer(void *arg)
{
    struct sess *s = arg;
    for (uint32_t round = 0; !race_stop; round++) {
        for (uint32_t i = 0; i < NETDEV_SLOTS; i++) {
            volatile uint8_t *f = netdev_slot_of(&s->txe, i)->frame;
            f[12] = round & 1 ? 0x81 : 0x88;
            f[13] = round & 1 ? 0x00 : 0xb5;
        }
    }
}

/* Room in the tx ring, waiting for the driver's NETDEV_SIG_TX_ROOM. */
static bool room(struct sess *s)
{
    uint64_t end = now() + SETTLE;
    while (netdev_room(&s->txe) == 0) {
        (void)jam_event_signal(s->to_stack, NETDEV_SIG_TX_ROOM, 0);   /* before looking */
        signals_t seen = 0;
        if (netdev_sleep(&s->txe))
            (void)jam_object_wait_one(s->to_stack, NETDEV_SIG_TX_ROOM, now() + 100 * NS_PER_MS,
                                      &seen);
        netdev_awake(&s->txe);
        if (now() > end)
            FAIL("no room in the tx ring for 3 s");
    }
    return true;
}

static bool race(struct sess *s)
{
    struct netdev_stats a, b;
    CHECK(get_stats(nic, &a));
    handle_t th;
    race_stop = false;
    CHECK_ST(thread_spawn("nettest-racer", racer, s, race_stack, sizeof(race_stack), &th), OK);
    uint8_t f[64];
    for (uint32_t i = 0; i < RACE_FRAMES; i++) {
        if (!room(s))
            break;
        frame_make(f, sizeof(f), "nettest-race", i);
        netdev_put(&s->txe, f, sizeof(f));
        if (i % 32 == 31)
            tx_kick(s);
    }
    tx_kick(s);
    bool ok = drained(s, s->txe.count);
    race_stop = true;
    signals_t seen;
    CHECK_ST(jam_object_wait_one(th, SIG_TERMINATED, soon(), &seen), OK);
    jam_handle_close(th);
    CHECK(ok);
    CHECK(get_stats(nic, &b));
    uint64_t sent = b.tx_frames - a.tx_frames, refused = b.tx_bad_tag - a.tx_bad_tag;
    printf("nettest: race: %lu frames sent, %lu refused as tagged (their EtherType was 0x8100 "
           "when copied)\n", (unsigned long)sent, (unsigned long)refused);
    CHECK_EQ(sent + refused, RACE_FRAMES);
    CHECK_EQ(b.tx_bad_len - a.tx_bad_len, 0);
    CHECK_EQ(b.tx_bad_flags - a.tx_bad_flags, 0);
    return true;
}

/* Every queued frame sent by the chip, and the chip sent nothing else. */
static bool settled(void)
{
    struct netdev_stats b;
    uint64_t end = now() + SETTLE;
    do {
        CHECK(get_stats(nic, &b));
        if (b.tx_done == b.tx_frames && b.chip_tx_ok == b.tx_frames)
            break;
        (void)jam_nanosleep(now() + 20 * NS_PER_MS);
    } while (now() < end);
    printf("nettest: tx: the driver queued %lu frames since it started, the chip sent %lu "
           "(reaped %lu); refused %lu bad length, %lu tagged, %lu flags; ring errors %lu\n",
           (unsigned long)b.tx_frames, (unsigned long)b.chip_tx_ok, (unsigned long)b.tx_done,
           (unsigned long)b.tx_bad_len, (unsigned long)b.tx_bad_tag,
           (unsigned long)b.tx_bad_flags, (unsigned long)b.ring_errors);
    CHECK_EQ(b.tx_done, b.tx_frames);
    CHECK_EQ(b.chip_tx_ok, b.tx_frames);
    CHECK(b.chip_counted & NETDEV_CHIP_TX_OK);
    return true;
}

bool t_hostile(void)
{
    struct sess s;
    CHECK(wait_link());
    CHECK_ST(sess_open(&s), OK);
    bool ok = every_bad_slot(&s) && bad_counts(&s) && race(&s);
    sess_close(&s);
    return ok && settled();
}
