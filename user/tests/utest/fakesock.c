/* utest: fake sockets for the wait-set tests (fakesock.h has the model). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include "fakesock.h"
#include "utest.h"

#define SOCK_BYTES ((uint64_t)SOCKRING_HDR + 2 * FAKE_RING)
#define CHUNK      2048u   /* a stream's bytes copied at a time */

/* ---- what the bytes are ------------------------------------------------------------- */

/* Datagram q of socket i, rx (netstack to the program) or tx. */
static uint32_t dg_len(const struct fsock *k, uint64_t q, bool rx)
{
    return (uint32_t)((q * (rx ? 29 : 31) + k->idx * 7) % 400);
}

static uint8_t dg_byte(const struct fsock *k, uint64_t q, uint32_t j, bool rx)
{
    return (uint8_t)((q * 5 + k->idx + j) ^ (rx ? 0 : 0x5a));
}

/* Byte b of a stream socket's rx or tx direction. */
static uint8_t st_byte(const struct fsock *k, uint64_t b, bool rx)
{
    return (uint8_t)((b * 13 + k->idx * 3 + (b >> 8)) ^ (rx ? 0 : 0xa5));
}

static void dg_make(const struct fsock *k, uint64_t q, bool rx, struct sockring_dgram *h,
                    uint8_t *d)
{
    *h = (struct sockring_dgram){ .addr = (uint32_t)q, .port = (uint16_t)k->idx,
                                  .len = (uint16_t)dg_len(k, q, rx) };
    for (uint32_t j = 0; j < h->len; j++)
        d[j] = dg_byte(k, q, j, rx);
}

static bool dg_check(const struct fsock *k, uint64_t q, bool rx, const struct sockring_dgram *h,
                     const uint8_t *d)
{
    if (h->addr != (uint32_t)q || h->port != k->idx || h->len != dg_len(k, q, rx))
        return false;
    for (uint32_t j = 0; j < h->len; j++)
        if (d[j] != dg_byte(k, q, j, rx))
            return false;
    return true;
}

/* ---- setting up ---------------------------------------------------------------------- */

bool fsock_dgram(const struct fsock *k)
{
    return k->idx % 2 == 0;
}

static bool fsock_open(struct fnet *f, uint32_t i)
{
    struct fsock *k = &f->sk[i];
    uint8_t *at = f->map + i * SOCK_BYTES;
    uint32_t framing = i % 2 == 0 ? SOCKRING_DGRAM : SOCKRING_STREAM;
    *k = (struct fsock){ .idx = i };
    CHECK_ST(sockring_make(&k->s, at, framing, FAKE_RING, FAKE_RING), OK);
    CHECK_ST(sockring_attach(&k->p, at, SOCK_BYTES, framing, FAKE_RING, FAKE_RING), OK);
    k->st.state = SOCKRING_STATE_OPEN;   /* as sockring_make wrote it */
    CHECK_ST(jam_event_create(&k->to_prog), OK);
    CHECK_ST(jam_handle_duplicate(k->to_prog, SOCKRING_TO_PROG_RIGHTS, &k->prog_to_prog), OK);
    CHECK_ST(jam_channel_create(&k->ch_prog, &k->ch_stack), OK);
    return true;
}

bool fnet_open(struct fnet *f, uint32_t n)
{
    *f = (struct fnet){ .n = n, .bytes = n * SOCK_BYTES };
    CHECK(n && n <= FAKE_SOCKS);
    CHECK_ST(jam_vmo_create(f->bytes, 0, HANDLE_INVALID, &f->vmo), OK);
    uint64_t va = 0;
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), f->vmo, 0, f->bytes,
                          VMAR_READ | VMAR_WRITE, &va), OK);
    f->map = (uint8_t *)(uintptr_t)va;
    for (uint32_t i = 0; i < n; i++)
        CHECK(fsock_open(f, i));
    return true;
}

void fnet_close(struct fnet *f)
{
    for (uint32_t i = 0; i < f->n; i++) {
        struct fsock *k = &f->sk[i];
        handle_t hs[] = { k->to_prog, k->prog_to_prog, k->ch_prog, k->ch_stack };
        for (unsigned j = 0; j < sizeof(hs) / sizeof(hs[0]); j++)
            if (hs[j])
                jam_handle_close(hs[j]);
    }
    if (f->map)   /* the test's own mapping: nothing to do if it fails */
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)f->map,
                             f->bytes);
    if (f->vmo)
        jam_handle_close(f->vmo);
    *f = (struct fnet){ 0 };
}

struct netwait_sock fsock_waitable(struct fsock *k)
{
    return (struct netwait_sock){ .rings = &k->p, .to_prog = k->prog_to_prog, .ch = k->ch_prog };
}

/* ---- netstack's side ------------------------------------------------------------------ */

static void stack_signal(struct fsock *k, signals_t sig)
{
    (void)jam_event_signal(k->to_prog, 0, sig);   /* its own event: can't fail */
}

uint32_t stack_rx(struct fsock *k, uint32_t max, uint64_t limit)
{
    static uint8_t d[SOCKRING_DGRAM_MAX > CHUNK ? SOCKRING_DGRAM_MAX : CHUNK];
    uint32_t n = 0;
    if (limit - k->rx_made < max)
        max = (uint32_t)(limit - k->rx_made);
    if (fsock_dgram(k)) {
        struct sockring_dgram h;
        for (; n < max; n++, k->rx_made++) {
            dg_make(k, k->rx_made, true, &h, d);
            if (sockring_dgram_put(&k->s.rx, &h, d) != OK)
                break;
        }
    } else {
        uint32_t want = max < CHUNK ? max : CHUNK;
        for (uint32_t j = 0; j < want; j++)
            d[j] = st_byte(k, k->rx_made + j, true);
        n = sockring_stream_write(&k->s.rx, d, want);
        k->rx_made += n;
    }
    if (n && sockring_publish(&k->s.rx))
        stack_signal(k, SOCKRING_SIG_RX);
    return n;
}

void stack_rx_end(struct fsock *k)
{
    if (!k->s.rx.ended && sockring_finish(&k->s.rx))
        stack_signal(k, SOCKRING_SIG_RX);
}

uint32_t stack_tx(struct fsock *k)
{
    static uint8_t d[SOCKRING_DGRAM_MAX > CHUNK ? SOCKRING_DGRAM_MAX : CHUNK];
    uint32_t n = 0;
    if (fsock_dgram(k)) {
        struct sockring_dgram h;
        for (; sockring_dgram_take(&k->s.tx, &h, d) == OK; n++, k->tx_took++)
            k->bad_tx += !dg_check(k, k->tx_took, false, &h, d);
    } else {
        for (uint32_t got; (got = sockring_stream_read(&k->s.tx, d, CHUNK)) != 0; n += got)
            for (uint32_t j = 0; j < got; j++, k->tx_took++)
                k->bad_tx += d[j] != st_byte(k, k->tx_took, false);
    }
    if (n && sockring_publish(&k->s.tx))
        stack_signal(k, SOCKRING_SIG_TX_ROOM);
    return n;
}

void stack_state(struct fsock *k, uint32_t state, status_t error)
{
    k->st.state = state;
    k->st.error = error;
    k->st.changes++;
    sockring_status_put(&k->s, &k->st);
    stack_signal(k, SOCKRING_SIG_STATE);
}

void stack_kill(struct fsock *k)
{
    jam_handle_close(k->ch_stack);
    k->ch_stack = HANDLE_INVALID;
    __atomic_store_n(&k->dead, true, __ATOMIC_RELEASE);
}

/* ---- the program's side --------------------------------------------------------------- */

uint32_t prog_read(struct fsock *k, uint32_t max)
{
    static uint8_t d[SOCKRING_DGRAM_MAX > CHUNK ? SOCKRING_DGRAM_MAX : CHUNK];
    uint32_t n = 0;
    if (fsock_dgram(k)) {
        struct sockring_dgram h;
        for (; n < max && sockring_dgram_take(&k->p.rx, &h, d) == OK; n++, k->rx_got++)
            k->bad_rx += !dg_check(k, k->rx_got, true, &h, d);
    } else {
        for (uint32_t got; n < max; n += got) {
            uint32_t want = max - n < CHUNK ? max - n : CHUNK;
            if (!(got = sockring_stream_read(&k->p.rx, d, want)))
                break;
            for (uint32_t j = 0; j < got; j++, k->rx_got++)
                k->bad_rx += d[j] != st_byte(k, k->rx_got, true);
        }
    }
    if (n)   /* this netstack never waits for rx room: no signal to send */
        (void)sockring_publish(&k->p.rx);
    return n;
}

uint32_t prog_write(struct fsock *k, uint32_t max)
{
    static uint8_t d[SOCKRING_DGRAM_MAX > CHUNK ? SOCKRING_DGRAM_MAX : CHUNK];
    uint32_t n = 0;
    if (fsock_dgram(k)) {
        struct sockring_dgram h;
        for (; n < max; n++, k->tx_made++) {
            dg_make(k, k->tx_made, false, &h, d);
            if (sockring_dgram_put(&k->p.tx, &h, d) != OK)
                break;
        }
    } else {
        uint32_t want = max < CHUNK ? max : CHUNK;
        for (uint32_t j = 0; j < want; j++)
            d[j] = st_byte(k, k->tx_made + j, false);
        n = sockring_stream_write(&k->p.tx, d, want);
        k->tx_made += n;
    }
    if (n)   /* this netstack never sleeps on its tx ring: no signal to send */
        (void)sockring_publish(&k->p.tx);
    return n;
}

/* ---- what the rings say ------------------------------------------------------------------ */

uint32_t fsock_expect(struct fsock *k, uint32_t interest)
{
    if (__atomic_load_n(&k->dead, __ATOMIC_ACQUIRE))
        return NETWAIT_HUP | NETWAIT_ERROR;
    struct sockring_page *pg = k->s.page;
    uint32_t bits = 0;
    if (k->st.state == SOCKRING_STATE_CLOSED)
        bits |= NETWAIT_HUP | (k->st.error != OK ? NETWAIT_ERROR : 0);
    bool end = __atomic_load_n(&pg->rx_prod.flags, __ATOMIC_ACQUIRE) & SOCKRING_END;
    uint64_t produced = __atomic_load_n(&pg->rx_prod.count, __ATOMIC_ACQUIRE);
    if ((interest & NETWAIT_READ) && (produced != k->p.rx.count || end))
        bits |= NETWAIT_READ | (end ? NETWAIT_RX_END : 0);
    uint64_t consumed = __atomic_load_n(&pg->tx_cons.count, __ATOMIC_ACQUIRE);
    uint32_t need = fsock_dgram(k) ? sockring_dgram_bytes(SOCKRING_DGRAM_MAX) : 1;
    if ((interest & NETWAIT_WRITE) && k->st.state == SOCKRING_STATE_OPEN &&
        FAKE_RING - (k->p.tx.count - consumed) >= need)
        bits |= NETWAIT_WRITE;
    return bits;
}

/* ---- a set of fake sockets ----------------------------------------------------------------- */

bool fsock_add(struct netwait *w, struct fsock *k, uint32_t interest)
{
    struct netwait_sock s = fsock_waitable(k);
    CHECK_ST(netwait_add_sock(w, &s, interest, k, &k->id), OK);
    k->interest = interest;
    return true;
}

bool fsock_take_out(struct netwait *w, struct fsock *k)
{
    CHECK_ST(netwait_remove(w, k->id), OK);
    k->id = 0;
    return true;
}

bool fsock_gather(struct netwait *w, uint64_t deadline, uint32_t *bits, status_t *errs,
                  status_t *out_st)
{
    static struct netwait_ready got[FAKE_SOCKS];
    uint32_t n = 0;
    status_t st = netwait_wait(w, deadline, got, FAKE_SOCKS, &n);
    memset(bits, 0, FAKE_SOCKS * sizeof(*bits));
    for (uint32_t i = 0; st == OK && i < n; i++) {
        struct fsock *k = got[i].user;
        CHECK(k && k->id == got[i].id);   /* a removed entry is never reported */
        CHECK(!bits[k->idx]);             /* nor anything twice */
        bits[k->idx] = got[i].ready;
        errs[k->idx] = got[i].error;
    }
    *out_st = st;
    return true;
}
