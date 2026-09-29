/* drvtest: checks <jam/driver.h> itself, the same code in both builds (a
 * ktest runs it as a kernel process, utest as a process). It exercises the
 * heap, channels, ports, waits, VMOs and mappings, threads and the clock,
 * checks the device calls refuse bad handles, and, given a channel to a
 * null server (role DRVTEST_NULL), calls it through the generated client,
 * including requests the server must reject. Exit code 0 if every check
 * passed (drv_exit), and one RESULTS line. */
#include <jam/driver.h>
#include <idl/edu.h>
#include <idl/null.h>

#define DRVTEST_NULL 0x40   /* this test's own role: a channel to a null server */
#define MS           1000000ull
#define PAGE         4096u

static unsigned checks, failures;

#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            failures++;                                                       \
            drv_log("FAILED at line %d: %s", __LINE__, #c);                   \
        }                                                                     \
    } while (0)

#define CHECK_ST(e, w)                                                        \
    do {                                                                      \
        status_t _s = (e), _w = (w);                                          \
        checks++;                                                             \
        if (_s != _w) {                                                       \
            failures++;                                                       \
            drv_log("FAILED at line %d: %s is %s, want %s", __LINE__, #e,     \
                    status_str(_s), status_str(_w));                          \
        }                                                                     \
    } while (0)

static void t_basics(const struct driver_start *s)
{
    CHECK(s->name && s->name[0]);
    CHECK(drv_handle(s, DR_PCIDEV) == HANDLE_INVALID);
    uint64_t t0 = drv_clock_ns();
    CHECK_ST(drv_sleep_until(t0 + 2 * MS), OK);
    CHECK(drv_clock_ns() >= t0 + 2 * MS);
    CHECK_ST(drv_sleep_until(0), OK);   /* in the past: at once */
}

static void t_heap(void)
{
    enum { N = 200 };
    uint8_t *p[N];
    uint32_t len[N];
    for (int i = 0; i < N; i++) {
        len[i] = (uint32_t)(i * 37 % 3000) + 1;
        p[i] = drv_malloc(len[i]);
        CHECK(p[i] && ((uintptr_t)p[i] & 15) == 0);
        if (!p[i])
            return;
        for (uint32_t j = 0; j < len[i]; j++)
            p[i][j] = (uint8_t)(i + j);
    }
    for (int i = 1; i < N; i += 2)
        drv_free(p[i]);
    for (int i = 1; i < N; i += 2) {   /* reuse the freed blocks */
        p[i] = drv_malloc(len[i]);
        CHECK(p[i] != NULL);
        if (!p[i])
            return;
        for (uint32_t j = 0; j < len[i]; j++)
            p[i][j] = (uint8_t)(i + j);
    }
    bool intact = true;
    for (int i = 0; i < N; i++)
        for (uint32_t j = 0; j < len[i]; j++)
            if (p[i][j] != (uint8_t)(i + j))
                intact = false;
    CHECK(intact);
    for (int i = 0; i < N; i++)
        drv_free(p[i]);
    /* Bigger than a heap chunk in the kernel build. */
    uint8_t *big = drv_malloc(300u << 10);
    CHECK(big != NULL);
    if (big) {
        big[0] = 1;
        big[(300u << 10) - 1] = 2;
        CHECK(big[0] == 1 && big[(300u << 10) - 1] == 2);
        drv_free(big);
    }
    drv_free(NULL);
}

static void t_channels(void)
{
    handle_t a, b, c, d, dup, got[4];
    uint8_t msg[8] = { 0, 0, 0, 0, 'h', 'e', 'y', '!' }, in[16];
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_create(&a, &b), OK);
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, got, 4, &nh), ERR_SHOULD_WAIT);
    CHECK_ST(drv_channel_write(a, msg, sizeof(msg), NULL, 0), OK);
    CHECK_ST(drv_channel_read(b, in, 4, &n, got, 4, &nh), ERR_BUFFER_TOO_SMALL);
    CHECK(n == sizeof(msg));
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, got, 4, &nh), OK);
    CHECK(n == sizeof(msg) && nh == 0 && in[4] == 'h' && in[7] == '!');
    /* A handle moves with a message. */
    CHECK_ST(drv_channel_create(&c, &d), OK);
    CHECK_ST(drv_channel_write(a, msg, sizeof(msg), &d, 1), OK);
    CHECK_ST(drv_handle_close(d), ERR_BAD_HANDLE);   /* it left our table */
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, got, 4, &nh), OK);
    CHECK(nh == 1);
    if (nh == 1) {
        CHECK_ST(drv_channel_write(c, msg, sizeof(msg), NULL, 0), OK);
        CHECK_ST(drv_channel_read(got[0], in, sizeof(in), &n, NULL, 0, &nh), OK);
        CHECK_ST(drv_handle_close(got[0]), OK);
    }
    CHECK_ST(drv_handle_close(c), OK);
    /* Duplicates, and the peer closing. */
    CHECK_ST(drv_handle_duplicate(a, RIGHT_SAME, &dup), OK);
    CHECK_ST(drv_handle_close(a), OK);
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, NULL, 0, &nh), ERR_SHOULD_WAIT);
    CHECK_ST(drv_handle_close(dup), OK);
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, NULL, 0, &nh), ERR_PEER_CLOSED);
    signals_t seen = 0;
    CHECK_ST(drv_object_wait_one(b, SIG_PEER_CLOSED, 0, &seen), OK);
    CHECK(seen & SIG_PEER_CLOSED);
    CHECK_ST(drv_handle_close(b), OK);
    CHECK_ST(drv_handle_close(b), ERR_BAD_HANDLE);
}

static void t_ports(void)
{
    handle_t port, a, b;
    struct port_packet pkt;
    uint8_t msg[4] = { 0 };
    CHECK_ST(drv_port_create(&port), OK);
    CHECK_ST(drv_channel_create(&a, &b), OK);
    CHECK_ST(drv_port_bind(port, b, 7, SIG_READABLE, PORT_BIND_ONCE), OK);
    CHECK_ST(drv_port_wait(port, drv_clock_ns() + MS, &pkt), ERR_TIMED_OUT);
    CHECK_ST(drv_channel_write(a, msg, sizeof(msg), NULL, 0), OK);
    CHECK_ST(drv_port_wait(port, drv_clock_ns() + 2000 * MS, &pkt), OK);
    CHECK(pkt.key == 7 && pkt.type == PORT_PACKET_SIGNAL && (pkt.signal.observed & SIG_READABLE));
    signals_t seen = 0;
    CHECK_ST(drv_object_wait_one(a, SIG_READABLE, drv_clock_ns() + MS, &seen), ERR_TIMED_OUT);
    CHECK_ST(drv_handle_close(a), OK);
    CHECK_ST(drv_handle_close(b), OK);
    CHECK_ST(drv_handle_close(port), OK);
}

static void t_vmos(void)
{
    handle_t v;
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, in[8];
    CHECK_ST(drv_vmo_create(3 * PAGE, 0, &v), OK);
    CHECK_ST(drv_vmo_write(v, 100, buf, sizeof(buf)), OK);
    CHECK_ST(drv_vmo_read(v, 100, in, sizeof(in)), OK);
    CHECK(in[0] == 1 && in[7] == 8);
    CHECK_ST(drv_vmo_read(v, 3 * PAGE - 4, in, sizeof(in)), ERR_OUT_OF_RANGE);
    void *m = NULL;
    CHECK_ST(drv_vmo_map(v, 0, 3 * PAGE, 0, &m), ERR_INVALID_ARGS);
    CHECK_ST(drv_vmo_map(v, 0, 3 * PAGE, VMAR_READ | VMAR_WRITE, &m), OK);
    if (m) {
        volatile uint8_t *p = m;
        CHECK(p[100] == 1 && p[107] == 8);
        p[5000] = 0x5a;
        CHECK_ST(drv_vmo_read(v, 5000, in, 1), OK);
        CHECK(in[0] == 0x5a);
        CHECK_ST(drv_vmo_unmap(m, 3 * PAGE), OK);
    }
    /* Not page-aligned: the pointer is to the offset itself. */
    void *m2 = NULL;
    CHECK_ST(drv_vmo_map(v, 101, 10, VMAR_READ, &m2), OK);
    if (m2) {
        CHECK(((volatile uint8_t *)m2)[0] == 2);
        CHECK_ST(drv_vmo_unmap(m2, 10), OK);
    }
    CHECK_ST(drv_handle_close(v), OK);
    /* The mapping keeps the VMO: close first, then use and unmap. */
    CHECK_ST(drv_vmo_create(PAGE, 0, &v), OK);
    CHECK_ST(drv_vmo_map(v, 0, PAGE, VMAR_READ | VMAR_WRITE, &m), OK);
    CHECK_ST(drv_handle_close(v), OK);
    if (m) {
        ((volatile uint8_t *)m)[10] = 3;
        CHECK(((volatile uint8_t *)m)[10] == 3);
        CHECK_ST(drv_vmo_unmap(m, PAGE), OK);
    }
}

static void worker(void *arg)
{
    handle_t ch = *(handle_t *)arg;
    uint8_t msg[8] = { 0, 0, 0, 0, 42, 0, 0, 0 };
    drv_channel_write(ch, msg, sizeof(msg), NULL, 0);
}

static void t_threads(void)
{
    handle_t a, b;
    uint8_t in[8];
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_create(&a, &b), OK);
    CHECK_ST(drv_thread_start("drvtest-worker", worker, &a), OK);
    signals_t seen = 0;
    CHECK_ST(drv_object_wait_one(b, SIG_READABLE, drv_clock_ns() + 5000 * MS, &seen), OK);
    CHECK_ST(drv_channel_read(b, in, sizeof(in), &n, NULL, 0, &nh), OK);
    CHECK(n == 8 && in[4] == 42);
    CHECK_ST(drv_thread_start("bad", NULL, NULL), ERR_INVALID_ARGS);
    CHECK_ST(drv_handle_close(a), OK);
    CHECK_ST(drv_handle_close(b), OK);
}

/* No device here: the device calls must refuse, not crash. */
static void t_device_refusals(void)
{
    volatile void *mm = NULL;
    uint32_t val = 0;
    uint64_t addrs[1], pin = 0;
    handle_t v;
    CHECK(drv_mmio_map(HANDLE_INVALID, 0, PAGE, VMO_CACHE_UC, &mm) != OK);
    CHECK(drv_pci_config_read(HANDLE_INVALID, 0, 4, &val) != OK);
    CHECK(drv_pci_config_write(HANDLE_INVALID, 0x40, 4, 0) != OK);
    CHECK(drv_interrupt_ack(HANDLE_INVALID) != OK);
    CHECK_ST(drv_vmo_create(PAGE, 0, &v), OK);
    CHECK(drv_vmo_pin(v, HANDLE_INVALID, 0, PAGE, addrs, &pin) != OK);
    CHECK(drv_vmo_unpin(v, 12345) != OK);
    CHECK(drv_mmio_map(v, 0, PAGE, VMO_CACHE_UC, &mm) != OK);   /* a VMO isn't a BAR */
    CHECK(drv_pci_config_read(v, 0, 4, &val) != OK);
    CHECK_ST(drv_handle_close(v), OK);
}

/* A raw request to the null server: its reply's status. */
static status_t raw_call(handle_t ch, void *req, uint32_t n)
{
    uint8_t rep[64];
    uint32_t rn = 0;
    status_t st = drv_channel_call(ch, req, n, rep, sizeof(rep), &rn, drv_clock_ns() + 5000 * MS);
    return st == OK ? idl_rep_status(rep, rn, rn) : st;
}

static void t_null_protocol(handle_t ch)
{
    uint64_t v = 0;
    uint32_t sum = 0;
    CHECK_ST(null_ping(ch, 0x1234567890abcdefull, &v), OK);
    CHECK(v == 0x1234567890abcdefull);
    CHECK_ST(null_add(ch, 2, 3, &sum), OK);
    CHECK(sum == 5);
    CHECK_ST(null_add_until(ch, drv_clock_ns() + 5000 * MS, 0xffffffffu, 2, &sum), OK);
    CHECK(sum == 1);
    uint8_t data[16], rev[16];
    for (int i = 0; i < 16; i++)
        data[i] = (uint8_t)(i * 3);
    CHECK_ST(null_reverse(ch, data, rev), OK);
    bool reversed = true;
    for (int i = 0; i < 16; i++)
        reversed &= rev[i] == data[15 - i];
    CHECK(reversed);
    bool all = true;
    for (uint64_t i = 0; i < 100; i++)
        all &= null_ping(ch, i, &v) == OK && v == i;
    CHECK(all);

    /* What the server must refuse. */
    uint32_t edu_result = 0;
    CHECK_ST(edu_factorial(ch, 5, &edu_result), ERR_NOT_SUPPORTED);   /* another protocol */
    struct null_ping_req q = { 0, NULL_PING, 1 };
    CHECK_ST(raw_call(ch, &q, 6), ERR_INVALID_ARGS);                   /* no ordinal */
    CHECK_ST(raw_call(ch, &q, sizeof(q) - 1), ERR_INVALID_ARGS);       /* wrong size */
    q.ordinal = NULL_PING + 100;
    CHECK_ST(raw_call(ch, &q, sizeof(q)), ERR_NOT_SUPPORTED);          /* no such method */
    uint8_t *huge = drv_malloc(2000);                                  /* bigger than any request */
    if (huge) {
        for (int i = 0; i < 2000; i++)
            huge[i] = 0xee;
        CHECK_ST(raw_call(ch, huge, 2000), ERR_INVALID_ARGS);
        drv_free(huge);
    }
    /* A request carrying a handle (written, not called: the reply queues). */
    handle_t x, y;
    CHECK_ST(drv_channel_create(&x, &y), OK);
    struct null_ping_req hq = { 0x77, NULL_PING, 1 };
    CHECK_ST(drv_channel_write(ch, &hq, sizeof(hq), &x, 1), OK);
    signals_t seen = 0;
    CHECK_ST(drv_object_wait_one(ch, SIG_READABLE, drv_clock_ns() + 5000 * MS, &seen), OK);
    struct idl_rep_hdr rh = { 0, 0 };
    uint32_t n = 0, nh = 0;
    CHECK_ST(drv_channel_read(ch, &rh, sizeof(rh), &n, NULL, 0, &nh), OK);
    CHECK(n == sizeof(rh) && rh.txid == 0x77 && rh.status == ERR_INVALID_ARGS);
    /* The server closed the handle it was sent. */
    CHECK_ST(drv_object_wait_one(y, SIG_PEER_CLOSED, drv_clock_ns() + 5000 * MS, &seen), OK);
    CHECK_ST(drv_handle_close(y), OK);
    /* And it still answers. */
    CHECK_ST(null_ping(ch, 99, &v), OK);
    CHECK(v == 99);
}

int driver_main(const struct driver_start *s)
{
    t_basics(s);
    t_heap();
    t_channels();
    t_ports();
    t_vmos();
    t_threads();
    t_device_refusals();
    handle_t ch = drv_handle(s, DRVTEST_NULL);
    if (ch != HANDLE_INVALID)
        t_null_protocol(ch);
    else
        drv_log("no null server: protocol checks skipped");
    /* drv_report puts "<name>: " in front. */
    if (failures)
        drv_report("%u of %u checks FAILED", failures, checks);
    else
        drv_report("%u checks passed", checks);
    drv_exit(failures ? 1 : 0);
}
