/* utest: netlog's core (<netlog.h>): its datagrams over hostile input
 * (golden bytes from tools/netlog-recv.py's struct layouts), and the
 * sender on a scripted clock against a fake log and a fake Mac that loses,
 * delays and reorders, goes away and comes back. The fake log is like the
 * kernel's: a ring that can drop its oldest bytes, and every line netlog
 * says lands in it, so a sender that logged its own sends would grow it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <crashlog.h>
#include <netlog.h>
#include <os.h>
#include <wire.h>
#include "utest.h"

#define LOG_MAX  (300u << 10)
#define QMAX     512u
#define BOOT     0x1122334455667788ull

/* A datagram on its way, either direction. */
struct dgram {
    uint64_t at;
    bool     to_mac;
    size_t   len;
    uint8_t  d[NETLOG_DATA_MAX];
};

/* The fake log, the fake Mac and the scripted network. */
struct net {
    uint64_t now;
    char     log[LOG_MAX];
    uint64_t log_len, log_first;     /* the ring holds [log_first, log_len) */
    unsigned says;                   /* lines netlog said */
    char     last_say[128];          /* the last of them */
    /* the Mac */
    bool     mac_up;
    unsigned drop;                   /* 1 in N datagrams lost, either way (0: none) */
    bool     reorder;
    uint64_t have[NETLOG_STREAMS];   /* bytes it has of each stream */
    uint64_t lost[NETLOG_STREAMS];   /* bytes it was told were lost (NETLOG_F_LOST) */
    char     got[NETLOG_STREAMS][LOG_MAX];
    bool     ended[NETLOG_STREAMS];
    /* counts */
    unsigned sent, sent_this_poll, max_per_poll, not_whole_lines;
    uint64_t max_unacked, top;       /* top: one past the highest byte sent */
    struct dgram q[QMAX];
    unsigned nq;
};

static struct net nt;
static struct netlog nl;

static void log_append(const char *s)
{
    size_t n = strlen(s);
    if (nt.log_len + n + 1 <= LOG_MAX) {
        memcpy(nt.log + nt.log_len, s, n);
        nt.log[nt.log_len + n] = '\n';
        nt.log_len += n + 1;
    }
}

static int64_t log_read(void *ctx, uint64_t pos, void *buf, uint64_t cap, uint64_t *first)
{
    (void)ctx;
    if (pos < nt.log_first)
        pos = nt.log_first;
    if (pos > nt.log_len)
        pos = nt.log_len;   /* past the end: no bytes, and where the end is (klog_read's) */
    *first = pos;
    uint64_t n = pos < nt.log_len ? nt.log_len - pos : 0;
    n = n < cap ? n : cap;
    memcpy(buf, nt.log + pos, n);
    return (int64_t)n;
}

static void put(bool to_mac, const uint8_t *d, size_t len)
{
    /* A pseudo-random 1 in nt.drop lost: every Nth exactly would line up
     * with the go-back rounds and lose the same datagram every time. */
    static uint32_t r = 1, count;
    r = r * 1664525u + 1013904223u;
    count++;
    if (nt.nq == QMAX || (nt.drop && (r >> 16) % nt.drop == 0))
        return;   /* lost on the way */
    struct dgram *g = &nt.q[nt.nq++];
    g->to_mac = to_mac;
    g->len = len;
    memcpy(g->d, d, len);
    g->at = nt.now + (nt.reorder ? 1 + (count * 7) % 23 : 1) * NS_PER_MS;
}

static status_t net_send(void *ctx, const uint8_t *d, size_t len)
{
    (void)ctx;
    struct netlog_data x;
    if (netlog_data_decode(d, len, &x) != OK)
        return ERR_INVALID_ARGS;
    nt.sent++;
    if (++nt.sent_this_poll > nt.max_per_poll)
        nt.max_per_poll = nt.sent_this_poll;
    if (x.offset >= nt.top && x.length == NETLOG_TEXT_MAX && x.text[x.length - 1] != '\n')
        nt.not_whole_lines++;   /* counted on the first send only */
    if (x.offset + x.length > nt.top)
        nt.top = x.offset + x.length;
    put(true, d, len);
    return OK;
}

static void net_say(void *ctx, const char *line)
{
    (void)ctx;
    nt.says++;
    snprintf(nt.last_say, sizeof(nt.last_say), "%s", line);
    log_append(line);   /* as bin/netlog's printf lands in the kernel log */
}

static const struct netlog_io net_io = { .ctx = &nt, .send = net_send, .say = net_say };

/* The Mac: netlog-recv.py's rules. */
static void mac_take(const uint8_t *d, size_t len)
{
    struct netlog_data x;
    if (!nt.mac_up || netlog_data_decode(d, len, &x) != OK || x.boot_id != BOOT)
        return;
    uint64_t *have = &nt.have[x.stream];
    if (x.acked > *have)
        *have = x.acked;   /* not in this test: the Mac never loses its state */
    if ((x.flags & NETLOG_F_LOST) && x.offset > *have) {
        nt.lost[x.stream] += x.offset - *have;
        *have = x.offset;
    }
    uint64_t end = x.offset + x.length;
    if (x.offset <= *have && *have < end && end <= LOG_MAX) {
        memcpy(nt.got[x.stream] + *have, x.text + (*have - x.offset), end - *have);
        *have = end;
    }
    if ((x.flags & NETLOG_F_END) && *have == end)
        nt.ended[x.stream] = true;
    uint8_t ack[NETLOG_ACK_SIZE];
    netlog_ack_encode(x.stream, BOOT, *have, ack);
    put(false, ack, sizeof(ack));
}

static void deliver(void)
{
    for (;;) {
        unsigned best = QMAX;
        for (unsigned i = 0; i < nt.nq; i++)
            if (nt.q[i].at <= nt.now && (best == QMAX || nt.q[i].at < nt.q[best].at))
                best = i;
        if (best == QMAX)
            return;
        struct dgram g = nt.q[best];
        nt.q[best] = nt.q[--nt.nq];
        if (g.to_mac)
            mac_take(g.d, g.len);
        else
            netlog_ack(&nl, g.d, g.len, nt.now);
    }
}

/* Run until `until` (scripted ns) or until nothing more can happen. */
static void run_until(uint64_t until)
{
    while (nt.now < until) {
        deliver();   /* bin/netlog polls again after what arrived */
        nt.sent_this_poll = 0;
        uint64_t next = netlog_poll(&nl, nt.now);
        for (unsigned i = 0; i < NETLOG_STREAMS; i++)
            if (nl.s[i].on && nl.s[i].high - nl.s[i].acked > nt.max_unacked)
                nt.max_unacked = nl.s[i].high - nl.s[i].acked;
        for (unsigned i = 0; i < nt.nq; i++)
            if (nt.q[i].at < next)
                next = nt.q[i].at;
        if (next == UINT64_MAX)
            return;
        nt.now = next > nt.now ? next : nt.now + 1;
    }
}

/* A log of n lines, some longer than a datagram. */
static void make_log(unsigned n)
{
    memset(&nt, 0, sizeof(nt));
    char line[3200];
    for (unsigned i = 0; i < n; i++) {
        unsigned len = (unsigned)snprintf(line, sizeof(line), "[%5u.%03u] line %u:", i / 100,
                                          i % 100 * 10, i);
        unsigned extra = i % 97 == 5 ? 3000 : (i * 37) % 120;
        for (unsigned k = 0; k < extra && len + 1 < sizeof(line); k++)
            line[len++] = (char)('a' + (i + k) % 26);
        line[len] = '\0';
        log_append(line);
    }
}

static void start(void)
{
    struct netlog_source src = { .read = log_read, .end = UINT64_MAX };
    nt.mac_up = true;
    netlog_start(&nl, &net_io, BOOT, &src);
}

bool t_netlog_golden(void)
{
    static const uint8_t data[] = {
        0x4a, 0x4e, 0x4c, 0x47, 0x01, 0x01, 0x01, 0x01, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
        0x22, 0x11, 0xe8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x68, 0x69, 0x0a,
    };
    static const uint8_t ack[] = {
        0x4a, 0x4e, 0x4c, 0x47, 0x01, 0x02, 0x01, 0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
        0x22, 0x11, 0xeb, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    struct netlog_data d;
    CHECK_ST(netlog_data_decode(data, sizeof(data), &d), OK);
    CHECK(d.stream == NETLOG_CRASH && d.flags == NETLOG_F_LOST && d.boot_id == BOOT &&
          d.offset == 1000 && d.acked == 900 && d.seq == 7 && d.length == 3 &&
          !memcmp(d.text, "hi\n", 3));
    uint8_t out[NETLOG_DATA_MAX];
    size_t n = 0;
    CHECK_ST(netlog_data_encode(&d, out, &n), OK);
    CHECK(n == sizeof(data) && !memcmp(out, data, n));
    uint8_t s = 0;
    uint64_t boot = 0, acked = 0;
    CHECK_ST(netlog_ack_decode(ack, sizeof(ack), &s, &boot, &acked), OK);
    CHECK(s == NETLOG_CRASH && boot == BOOT && acked == 1003);
    netlog_ack_encode(s, boot, acked, out);
    CHECK(!memcmp(out, ack, sizeof(ack)));
    return true;
}

bool t_netlog_hostile(void)
{
    uint8_t d[NETLOG_DATA_MAX + 8];
    struct netlog_data x = { .stream = NETLOG_LIVE, .boot_id = 1, .offset = 10, .acked = 5,
                             .length = 2, .text = (const uint8_t *)"a\n" };
    size_t n = 0;
    CHECK_ST(netlog_data_encode(&x, d, &n), OK);
    for (size_t len = 0; len <= n + 1; len++)
        CHECK(netlog_data_decode(d, len, &x) == (len == n ? OK : ERR_INVALID_ARGS));
    static const struct { size_t at; uint8_t v; } bytes[] = {
        { 0, 0 }, { 4, 2 }, { 5, NETLOG_ACK }, { 6, NETLOG_STREAMS }, { 7, 0x04 }, { 38, 1 },
        { 39, 1 }, { 36, 3 }, { 24, 11 },   /* acked past the offset */
    };
    for (unsigned i = 0; i < sizeof(bytes) / sizeof(bytes[0]); i++) {
        uint8_t c[NETLOG_DATA_MAX];
        memcpy(c, d, n);
        c[bytes[i].at] = bytes[i].v;
        if (netlog_data_decode(c, n, &x) != ERR_INVALID_ARGS)
            FAIL("byte %zu = %u taken", bytes[i].at, bytes[i].v);
    }
    struct netlog_data bad[] = {
        { .length = 0 },                                            /* empty, not the end */
        { .length = NETLOG_TEXT_MAX + 1, .text = d },
        { .stream = NETLOG_STREAMS, .length = 1, .text = d },
        { .flags = 0x80, .length = 1, .text = d },
        { .offset = 1, .acked = 2, .length = 1, .text = d },
        { .offset = UINT64_MAX, .length = 1, .text = d },           /* wraps */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t c[NETLOG_DATA_MAX];
        size_t m = 0;
        if (netlog_data_encode(&bad[i], c, &m) == OK &&
            netlog_data_decode(c, m, &x) == OK)
            FAIL("bad datagram %u went through", i);
    }
    uint8_t ack[NETLOG_ACK_SIZE + 1], s;
    uint64_t boot, acked;
    netlog_ack_encode(0, 1, 2, ack);
    CHECK_ST(netlog_ack_decode(ack, NETLOG_ACK_SIZE - 1, &s, &boot, &acked), ERR_INVALID_ARGS);
    CHECK_ST(netlog_ack_decode(ack, NETLOG_ACK_SIZE + 1, &s, &boot, &acked), ERR_INVALID_ARGS);
    ack[7] = 1;
    CHECK_ST(netlog_ack_decode(ack, NETLOG_ACK_SIZE, &s, &boot, &acked), ERR_INVALID_ARGS);
    uint32_t r = 3;
    for (unsigned round = 0; round < 2000; round++) {
        size_t len = round % (NETLOG_DATA_MAX + 4);
        for (size_t i = 0; i < len; i++) {
            r = r * 1664525u + 1013904223u;
            d[i] = (uint8_t)(r >> 24);
        }
        CHECK(netlog_data_decode(d, len, &x) != OK);
        CHECK(netlog_ack_decode(d, len, &s, &boot, &acked) != OK);
    }
    return true;
}

/* The whole log arrives, from its first byte, over a network that loses
 * one datagram in 7 and reorders; netlog never says a word about it. */
bool t_netlog_whole_log(void)
{
    make_log(2000);
    nt.drop = 7;
    nt.reorder = true;
    start();
    uint64_t len = nt.log_len;
    run_until(600 * NS_PER_S);
    CHECK_EQ(nt.have[NETLOG_LIVE], len);
    CHECK(!memcmp(nt.got[NETLOG_LIVE], nt.log, len));
    CHECK_EQ(nt.lost[NETLOG_LIVE], 0);
    CHECK(nt.max_per_poll <= NETLOG_BURST);
    CHECK(nt.max_unacked <= NETLOG_WINDOW);
    CHECK(nt.not_whole_lines <= (2000 / 97 + 1) * 2);   /* only the 3000-byte lines split */
    CHECK(nl.resent_rounds > 0);
    /* a stray say would also be text the Mac lacks: the log didn't grow */
    CHECK_EQ(nt.log_len, len);
    /* new lines go out too, and the sender rests when there is nothing */
    log_append("one more line");
    run_until(nt.now + 10 * NS_PER_S);
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);
    CHECK_EQ(netlog_poll(&nl, nt.now), UINT64_MAX);
    return true;
}

/* The Mac away for ten minutes: the waits double to NETLOG_WAIT_MAX, one
 * datagram per try, one line said (so the log grows by one line, not one
 * per try); then it is back: one line, and every byte arrives. */
bool t_netlog_mac_away(void)
{
    make_log(300);
    start();
    nt.mac_up = false;
    run_until(600 * NS_PER_S);
    CHECK_EQ(nt.says, 1);
    CHECK(strstr(nt.last_say, "doesn't answer"));
    CHECK_EQ(nl.wait, NETLOG_WAIT_MAX);
    /* the first window, then per round ~1 datagram: 600 s at 30 s is ~25 rounds */
    CHECK(nt.sent < NETLOG_WINDOW / NETLOG_TEXT_MAX + 4 * nl.resent_rounds);
    CHECK(nl.resent_rounds < 40);
    nt.mac_up = true;
    run_until(nt.now + 120 * NS_PER_S);
    CHECK_EQ(nt.says, 2);
    CHECK(strstr(nt.last_say, "answers again"));
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);   /* both said lines included */
    CHECK(!memcmp(nt.got[NETLOG_LIVE], nt.log, nt.log_len));
    return true;
}

/* The ring dropped bytes before they were sent: the first datagram says
 * so, and the Mac marks it instead of waiting. Then, while the Mac is
 * away, the ring drops what was sent but not acked: the resend says so. */
bool t_netlog_ring_dropped(void)
{
    make_log(1000);
    nt.log_first = 5000;
    start();
    run_until(60 * NS_PER_S);
    CHECK_EQ(nt.lost[NETLOG_LIVE], 5000);
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);
    CHECK(!memcmp(nt.got[NETLOG_LIVE] + 5000, nt.log + 5000, nt.log_len - 5000));
    uint64_t was = nt.log_len;
    nt.mac_up = false;
    for (unsigned i = 0; i < 200; i++)
        log_append("a line written while the Mac is away");
    run_until(nt.now + 5 * NS_PER_S);
    nt.log_first = was + 3000;   /* gone before the Mac came back */
    nt.mac_up = true;
    run_until(nt.now + 120 * NS_PER_S);
    CHECK_EQ(nt.lost[NETLOG_LIVE], 5000 + 3000);
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);
    return true;
}

/* Acks that don't fit what was sent change nothing. */
bool t_netlog_forged_acks(void)
{
    make_log(50);
    start();
    nt.mac_up = false;
    run_until(NS_PER_MS);   /* the first window is out, nothing acked */
    uint64_t high = nl.s[NETLOG_LIVE].high;
    CHECK(high > 0);
    uint8_t a[NETLOG_ACK_SIZE];
    netlog_ack_encode(NETLOG_LIVE, BOOT, nt.log_len + 1, a);  /* past what the log holds */
    netlog_ack(&nl, a, sizeof(a), nt.now);
    netlog_ack_encode(NETLOG_LIVE, BOOT + 1, high, a);       /* another boot */
    netlog_ack(&nl, a, sizeof(a), nt.now);
    netlog_ack_encode(NETLOG_CRASH, BOOT, 0, a);             /* a stream not on */
    netlog_ack(&nl, a, sizeof(a), nt.now);
    netlog_ack(&nl, "nonsense", 8, nt.now);
    CHECK_EQ(nl.ignored, 4);
    CHECK_EQ(nl.s[NETLOG_LIVE].acked, 0);
    netlog_ack_encode(NETLOG_LIVE, BOOT, 100, a);
    netlog_ack(&nl, a, sizeof(a), nt.now);
    netlog_ack_encode(NETLOG_LIVE, BOOT, 50, a);             /* an old one, overtaken */
    netlog_ack(&nl, a, sizeof(a), nt.now);
    CHECK_EQ(nl.s[NETLOG_LIVE].acked, 100);
    CHECK_EQ(nl.skipped, 0);
    return true;
}

/* bin/netlog started again (init restarts it) while the Mac kept its
 * file: the new sender starts at 0 and the Mac acks far past its first
 * window. It skips to the Mac's offset and sends only what is new; before,
 * it went back to 0 for ever. On an ended stream the skip is bounded the
 * same way, by the stream's end. */
bool t_netlog_sender_restarted(void)
{
    make_log(3000);   /* far more than a window */
    start();
    run_until(120 * NS_PER_S);
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);
    CHECK(nt.log_len > 2 * NETLOG_WINDOW);
    log_append("a line the first sender never saw");
    struct netlog_source src = { .read = log_read, .end = UINT64_MAX };
    netlog_start(&nl, &net_io, BOOT, &src);   /* the second sender, from byte 0 */
    unsigned before = nt.sent;
    run_until(nt.now + 30 * NS_PER_S);
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);
    CHECK(!memcmp(nt.got[NETLOG_LIVE], nt.log, nt.log_len));
    CHECK_EQ(nl.skipped, 1);
    CHECK_EQ(nl.s[NETLOG_LIVE].acked, nt.log_len);
    /* one window from 0, then the new line: not the whole log again */
    CHECK(nt.sent - before <= NETLOG_WINDOW / 1000 + 4);
    CHECK_EQ(netlog_poll(&nl, nt.now), UINT64_MAX);
    return true;
}

/* After a panic: the panicked boot's log from init's VMO, its own stream,
 * with an end. */
bool t_netlog_crash_stream(void)
{
    static const char text[] = "[ 1.000] the boot that panicked\n[ 2.000] PANIC: a test\n";
    struct crashlog_header h = {
        .magic = CRASHLOG_MAGIC, .version = CRASHLOG_VERSION, .panics = 1,
        .text_len = sizeof(text) - 1, .name = "boot-0042", .message = "a test",
    };
    handle_t v;
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &v), OK);
    CHECK_ST(jam_vmo_write(v, 0, &h, sizeof(h)), OK);
    CHECK_ST(jam_vmo_write(v, sizeof(h), text, sizeof(text) - 1), OK);
    struct netlog_vmo_ctx ctx;
    struct netlog_source crash;
    char name[32];
    CHECK_ST(netlog_crash_source(v, &ctx, &crash, name), OK);
    CHECK(!strcmp(name, "boot-0042") && crash.end == sizeof(text) - 1);
    make_log(20);
    start();
    netlog_add_crash(&nl, &crash);
    run_until(60 * NS_PER_S);
    CHECK_EQ(nt.have[NETLOG_CRASH], sizeof(text) - 1);
    CHECK(!memcmp(nt.got[NETLOG_CRASH], text, sizeof(text) - 1));
    CHECK(nt.ended[NETLOG_CRASH] && nl.s[NETLOG_CRASH].done);
    CHECK_EQ(nt.says, 1);
    CHECK(strstr(nt.last_say, "the last boot's log is sent"));
    CHECK_EQ(nt.have[NETLOG_LIVE], nt.log_len);   /* its say line included */
    /* a damaged header is no crash log */
    h.text_len = PAGE_SIZE;
    CHECK_ST(jam_vmo_write(v, 0, &h, sizeof(h)), OK);
    CHECK_ST(netlog_crash_source(v, &ctx, &crash, name), ERR_INVALID_ARGS);
    jam_handle_close(v);
    return true;
}

/* The live source over the real kernel log, from its first byte. */
bool t_netlog_klog_source(void)
{
    handle_t root = startup_handle(SR_RESOURCE), rd, klog;
    if (!root || jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_ROOT_KLOG, &rd) != OK) {
        printf("utest: %s: no RIGHT_ROOT_KLOG on our root: skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(jam_klog_open(rd, &klog), OK);
    struct netlog_source src;
    netlog_klog_source(klog, &src);
    CHECK_EQ(src.end, UINT64_MAX);
    char buf[256];
    uint64_t first = 1;
    int64_t n = src.read(src.ctx, 0, buf, sizeof(buf), &first);
    CHECK(n > 0);   /* the boot's start (or the ring's oldest byte) */
    uint64_t again = 0;
    char buf2[256];
    CHECK_EQ(src.read(src.ctx, first, buf2, (uint64_t)n, &again), n);
    CHECK(again == first && !memcmp(buf, buf2, (size_t)n));
    jam_handle_close(klog);
    jam_handle_close(rd);
    return true;
}
