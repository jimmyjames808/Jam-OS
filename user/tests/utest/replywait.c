/* utest: channel_reply_wait (abi/syscalls.def), a server's reply and its
 * wait for the next request in one call. The reply reaches its caller by
 * txid, also from a second process holding the same end (a successor
 * answering a call its predecessor took); a reply to a caller that has
 * gone is no reason not to wait; the port form; the mark is written
 * before the wait begins, and not at all when the reply fails; and what it
 * refuses: bad pointers, missing rights, wrong handle types, fields of the
 * other form.
 *
 * The child modes are here too: "rw-reply" (the second process) and
 * "rw-reader", the reader of the kernel's
 * chanread_reply_wait_kill_loses_nothing (kernel/test/test_chanrw.c,
 * which has the reason; the log VMO's layout is the RW_* below, the same
 * as there). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define RW_WAIT (5 * NS_PER_S)   /* every wait here is answered well before */

/* One request: its txid (stamped by the kernel) and a word. */
struct rw_req {
    uint32_t txid;
    uint32_t word;
};

/* A client thread: one channel_call on ch with `word`, the reply's word
 * into `got`. */
struct rw_client {
    handle_t ch;
    uint32_t word;
    uint64_t timeout;   /* ns; 0: RW_WAIT */
    status_t st;
    uint32_t got;
};

static void rw_client_main(void *arg)
{
    struct rw_client *c = arg;
    struct rw_req req = { 0, c->word }, rep = { 0, 0 };
    uint32_t n = 0;
    struct channel_call_args a = {
        .h = c->ch, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)&req,
        .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)&rep,
        .ractual = (uint64_t)(uintptr_t)&n, .flags = CHANNEL_CALL_TIMEOUT,
        .deadline_ns = c->timeout ? c->timeout : RW_WAIT,
    };
    c->st = jam_channel_call(&a);
    c->got = c->st == OK && n == sizeof(rep) ? rep.word : 0;
}

static uint8_t client_stack[16384] __attribute__((aligned(16)));

static bool client_start(struct rw_client *c, handle_t *th)
{
    CHECK_ST(thread_spawn("rw-client", rw_client_main, c, client_stack, sizeof(client_stack), th),
             OK);
    return true;
}

/* A reply_wait's arguments: reply `rep` on h (HANDLE_INVALID: none), the
 * next request from `wait` into *req, with a timeout. */
static struct channel_reply_wait_args rw_args(handle_t h, const struct rw_req *rep, handle_t wait,
                                              struct rw_req *req, uint32_t *nb, uint64_t timeout)
{
    struct channel_reply_wait_args a = {
        .h = h, .wait = wait, .bytes = (uint64_t)(uintptr_t)req, .bytes_cap = sizeof(*req),
        .actual_bytes = (uint64_t)(uintptr_t)nb, .flags = CHANNEL_REPLY_WAIT_TIMEOUT,
        .deadline_ns = timeout,
    };
    if (h != HANDLE_INVALID) {
        a.rbytes = (uint64_t)(uintptr_t)rep;
        a.rn = sizeof(*rep);
    }
    return a;
}

/* The basics, and the mark: a client calls, we take its request with a
 * first reply_wait (no reply), answer it with a second, which writes the
 * mark and then waits on. The client has its answer and the mark is set
 * while that second call still waits. */
static status_t rw_server_st;
static bool rw_server_back;   /* atomic */
static uint64_t rw_mark;      /* the kernel writes it */
static int32_t rw_rstatus;    /* the kernel writes it */

static void rw_server_main(void *arg)
{
    handle_t b = (handle_t)(uintptr_t)arg;
    struct rw_req req = { 0, 0 }, rep;
    uint32_t nb = 0;
    struct channel_reply_wait_args a = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, RW_WAIT);
    rw_server_st = jam_channel_reply_wait(&a);
    if (rw_server_st != OK || nb != sizeof(req))
        return;
    rep = (struct rw_req){ req.txid, req.word + 1 };
    a = rw_args(b, &rep, b, &req, &nb, RW_WAIT);
    a.mark = (uint64_t)(uintptr_t)&rw_mark;
    a.reply_status = (uint64_t)(uintptr_t)&rw_rstatus;
    rw_server_st = jam_channel_reply_wait(&a);   /* waits until the client's end closes */
    __atomic_store_n(&rw_server_back, true, __ATOMIC_RELEASE);
}

static uint8_t server_stack[16384] __attribute__((aligned(16)));

bool t_reply_wait_mark_before_wait(void)
{
    handle_t a, b, srv;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    rw_mark = 0;
    rw_rstatus = 99;
    __atomic_store_n(&rw_server_back, false, __ATOMIC_RELAXED);
    CHECK_ST(thread_spawn("rw-server", rw_server_main, (void *)(uintptr_t)b, server_stack,
                          sizeof(server_stack), &srv),
             OK);
    struct rw_client c = { .ch = a, .word = 41 };
    rw_client_main(&c);
    CHECK_ST(c.st, OK);
    CHECK_EQ(c.got, 42);
    /* The reply went out, so the mark is there (or comes within the call),
     * and the server is still waiting for its next request. */
    uint64_t end = now() + NS_PER_S;
    while (!__atomic_load_n(&rw_mark, __ATOMIC_ACQUIRE) && now() < end)
        jam_nanosleep(now() + NS_PER_MS);
    CHECK_EQ(__atomic_load_n(&rw_mark, __ATOMIC_ACQUIRE), 1);
    CHECK(!__atomic_load_n(&rw_server_back, __ATOMIC_ACQUIRE));
    CHECK_ST(jam_handle_close(a), OK);   /* its wait ends: nothing queued, the peer gone */
    if (!wait_threads(&srv, 1))
        return false;
    CHECK_ST(rw_server_st, ERR_PEER_CLOSED);
    CHECK_EQ(rw_rstatus, OK);   /* the reply's status: written as the call returned */
    CHECK_ST(jam_handle_close(b), OK);
    return true;
}

/* Our second reply_wait's reply goes to the caller that is still waiting
 * for its txid; a reply after its caller gave up is queued on the
 * caller's end as a stray message, and goes out all the same (mark set);
 * with the caller's end closed, the reply fails with ERR_PEER_CLOSED (no
 * mark) and the wait goes on, here on another channel. */
bool t_reply_wait_gone_caller(void)
{
    handle_t a, b, x, y, th;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct rw_client c = { .ch = a, .word = 7, .timeout = 30 * NS_PER_MS };
    if (!client_start(&c, &th))
        return false;
    struct rw_req req, rep;
    uint32_t nb = 0;
    struct channel_reply_wait_args ra = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, RW_WAIT);
    CHECK_ST(jam_channel_reply_wait(&ra), OK);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(c.st, ERR_TIMED_OUT);   /* gone before we answered */
    uint64_t mark = 0;
    int32_t rs = 99;
    rep = (struct rw_req){ req.txid, 8 };
    ra = rw_args(b, &rep, b, &req, &nb, 0);
    ra.mark = (uint64_t)(uintptr_t)&mark;
    ra.reply_status = (uint64_t)(uintptr_t)&rs;
    CHECK_ST(jam_channel_reply_wait(&ra), ERR_TIMED_OUT);   /* nothing more to read */
    CHECK_EQ(rs, OK);
    CHECK_EQ(mark, 1);
    struct rw_req stray = { 0, 0 };
    struct channel_read_args rd = {
        .h = a, .bytes_cap = sizeof(stray), .bytes = (uint64_t)(uintptr_t)&stray,
        .actual_bytes = (uint64_t)(uintptr_t)&nb,
    };
    CHECK_ST(jam_channel_read(&rd), OK);   /* the stray reply, queued */
    CHECK(stray.txid == req.txid && stray.word == 8);

    /* The caller's end gone: no reply, no mark, and the wait still goes on. */
    CHECK_ST(jam_channel_create(&x, &y), OK);
    CHECK_ST(jam_channel_write(x, &rep, sizeof(rep), NULL, 0), OK);
    CHECK_ST(jam_handle_close(a), OK);
    mark = 0;
    ra = rw_args(b, &rep, y, &req, &nb, RW_WAIT);
    ra.mark = (uint64_t)(uintptr_t)&mark;
    ra.reply_status = (uint64_t)(uintptr_t)&rs;
    CHECK_ST(jam_channel_reply_wait(&ra), OK);
    CHECK_EQ(rs, ERR_PEER_CLOSED);
    CHECK_EQ(mark, 0);
    CHECK(nb == sizeof(req) && req.word == 8);
    ra = rw_args(b, &rep, b, &req, &nb, RW_WAIT);
    CHECK_ST(jam_channel_reply_wait(&ra), ERR_PEER_CLOSED);   /* and then nothing to wait for */
    CHECK_ST(jam_handle_close(b), OK);
    CHECK_ST(jam_handle_close(x), OK);
    CHECK_ST(jam_handle_close(y), OK);
    return true;
}

/* "utest rw-reply <txid> <word>": answer txid with word on SR_USER, a
 * duplicate of the server end another process took the call on; the wait
 * after it times out. 0: as expected. */
static int rw_reply(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    uint32_t txid = 0, word = 0;
    for (const char *p = argv[2]; *p; p++)
        txid = txid * 10 + (uint32_t)(*p - '0');
    for (const char *p = argv[3]; *p; p++)
        word = word * 10 + (uint32_t)(*p - '0');
    handle_t h = startup_handle(SR_USER);
    struct rw_req rep = { txid, word }, req;
    uint32_t nb = 0;
    uint64_t mark = 0;
    int32_t rs = 99;
    struct channel_reply_wait_args a = rw_args(h, &rep, h, &req, &nb, NS_PER_MS);
    a.mark = (uint64_t)(uintptr_t)&mark;
    a.reply_status = (uint64_t)(uintptr_t)&rs;
    status_t st = jam_channel_reply_wait(&a);
    if (st != ERR_TIMED_OUT)
        return 3;
    return rs == OK && mark == 1 ? 0 : 4;
}

/* The successor's case (M11.6): the call is taken by this process, and
 * answered by another one holding a duplicate of the same server end. */
bool t_reply_wait_second_process(void)
{
    handle_t a, b, b2, job, proc, th;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct rw_client c = { .ch = a, .word = 5 };
    if (!client_start(&c, &th))
        return false;
    struct rw_req req;
    uint32_t nb = 0;
    struct channel_reply_wait_args ra = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, RW_WAIT);
    CHECK_ST(jam_channel_reply_wait(&ra), OK);
    CHECK(nb == sizeof(req) && req.word == 5);
    CHECK_ST(jam_handle_duplicate(b, RIGHT_SAME, &b2), OK);
    CHECK_ST(new_job(&job), OK);
    char arg[16];
    snprintf(arg, sizeof(arg), "%u", req.txid);
    const char *argv[] = { "utest", "rw-reply", arg, "6" };
    struct spawn_handle x = { SR_USER, b2 };
    struct spawn_args sa = {
        .path = "bin/utest", .name = "utest-rw-reply", .argc = 4, .argv = argv, .job = job,
        .extra = &x, .nextra = 1,
    };
    CHECK_ST(spawn(&sa, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(c.st, OK);
    CHECK_EQ(c.got, 6);   /* the other process's answer */
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    return true;
}

/* The port form: wait on a port the server end is bound to, read the
 * channel it names, answer with the next wait on the port. */
bool t_reply_wait_port(void)
{
    handle_t a, b, port, th;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(jam_port_create(&port), OK);
    CHECK_ST(jam_port_bind(port, b, 9, SIG_READABLE, PORT_BIND_PERSISTENT), OK);
    struct rw_client c = { .ch = a, .word = 11 };
    if (!client_start(&c, &th))
        return false;
    struct port_packet pkt = { 0 };
    struct channel_reply_wait_args pa = {
        .h = HANDLE_INVALID, .wait = port, .packet = (uint64_t)(uintptr_t)&pkt,
        .flags = CHANNEL_REPLY_WAIT_TIMEOUT, .deadline_ns = RW_WAIT,
    };
    CHECK_ST(jam_channel_reply_wait(&pa), OK);
    CHECK(pkt.key == 9 && pkt.type == PORT_PACKET_SIGNAL);
    struct rw_req req, rep;
    uint32_t nb = 0;
    struct channel_read_args rd = {
        .h = b, .bytes_cap = sizeof(req), .bytes = (uint64_t)(uintptr_t)&req,
        .actual_bytes = (uint64_t)(uintptr_t)&nb,
    };
    CHECK_ST(jam_channel_read(&rd), OK);
    rep = (struct rw_req){ req.txid, 12 };
    uint64_t mark = 0;
    pa.h = b;
    pa.rbytes = (uint64_t)(uintptr_t)&rep;
    pa.rn = sizeof(rep);
    pa.mark = (uint64_t)(uintptr_t)&mark;
    pa.deadline_ns = 0;
    CHECK_ST(jam_channel_reply_wait(&pa), ERR_TIMED_OUT);   /* answered; no packet since */
    CHECK_EQ(mark, 1);
    if (!wait_threads(&th, 1))
        return false;
    CHECK_ST(c.st, OK);
    CHECK_EQ(c.got, 12);
    /* Each form takes only its own fields. */
    struct rw_req q;
    struct channel_reply_wait_args bad = rw_args(HANDLE_INVALID, NULL, port, &q, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&bad), ERR_INVALID_ARGS);   /* a channel's fields */
    pa = (struct channel_reply_wait_args){ .wait = b, .packet = (uint64_t)(uintptr_t)&pkt };
    CHECK_ST(jam_channel_reply_wait(&pa), ERR_INVALID_ARGS);    /* a port's field */
    CHECK_ST(jam_handle_close(port), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    return true;
}

/* What it refuses, and that nothing was sent or taken when it does. */
static bool rw_refusals_args(handle_t a, handle_t b)
{
    struct rw_req req, rep = { 1, 2 };
    uint32_t nb = 0;
    CHECK_ST(jam_channel_reply_wait((const struct channel_reply_wait_args *)16), ERR_INVALID_ARGS);
    struct channel_reply_wait_args x = rw_args(b, &rep, b, &req, &nb, 0);
    x.reserved = 1;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    x = rw_args(b, &rep, b, &req, &nb, 0);
    x.flags |= 1u << 5;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    x = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, 0);
    x.rn = 4;   /* reply bytes, but no reply */
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    x = rw_args(b, &rep, b, &req, &nb, 0);
    x.rn = 65537;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_OUT_OF_RANGE);
    x = rw_args(b, &rep, b, &req, &nb, 0);
    x.rh = 16;   /* a bad handle array */
    x.rhn = 1;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    int32_t rs = 99;
    x = rw_args(b, &rep, b, &req, &nb, 0);
    x.rbytes = 16;   /* a bad reply: nothing sent, its status says so */
    x.reply_status = (uint64_t)(uintptr_t)&rs;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    CHECK_EQ(rs, ERR_INVALID_ARGS);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(a, SIG_READABLE, 0, &seen), ERR_TIMED_OUT);   /* nothing sent */
    return true;
}

static bool rw_refusals_handles(handle_t a, handle_t b)
{
    struct rw_req req, rep = { 1, 2 };
    uint32_t nb = 0;
    handle_t ev, port, ro, wo;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_port_create(&port), OK);
    struct channel_reply_wait_args x = rw_args(b, &rep, ev, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_WRONG_TYPE);   /* wait on an event */
    x = rw_args(port, &rep, b, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_WRONG_TYPE);   /* reply on a port */
    x = rw_args(HANDLE_INVALID, NULL, 0x7fff0000u, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_BAD_HANDLE);
    CHECK_ST(jam_handle_duplicate(b, RIGHT_READ | RIGHT_WAIT, &ro), OK);
    CHECK_ST(jam_handle_duplicate(b, RIGHT_WRITE | RIGHT_WAIT, &wo), OK);
    x = rw_args(ro, &rep, ro, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_ACCESS_DENIED);   /* no RIGHT_WRITE to reply */
    x = rw_args(HANDLE_INVALID, NULL, wo, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_ACCESS_DENIED);   /* no RIGHT_READ to wait */
    x = rw_args(wo, &rep, ro, &req, &nb, 0);   /* each with its own right: fine */
    CHECK_ST(jam_channel_reply_wait(&x), ERR_TIMED_OUT);
    CHECK_ST(jam_channel_read(&(struct channel_read_args){
                 .h = a, .bytes_cap = sizeof(req), .bytes = (uint64_t)(uintptr_t)&req }),
             OK);   /* that reply */
    CHECK_ST(jam_handle_close(ro), OK);
    CHECK_ST(jam_handle_close(wo), OK);
    CHECK_ST(jam_handle_close(ev), OK);
    CHECK_ST(jam_handle_close(port), OK);
    return true;
}

/* Bad buffers once the reply went out: a bad mark ends the call before
 * the wait (the request stays queued); a bad request buffer loses the
 * request (as channel_read), never its handle; a request too big stays
 * queued with its sizes. */
static bool rw_refusals_buffers(handle_t a, handle_t b)
{
    struct rw_req req, rep = { 1, 2 };
    uint32_t nb = 0, nh = 0;
    CHECK_ST(jam_channel_write(a, &rep, sizeof(rep), NULL, 0), OK);
    struct channel_reply_wait_args x = rw_args(b, &rep, b, &req, &nb, 0);
    x.mark = 16;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    struct channel_read_args rd = {
        .h = a, .bytes_cap = sizeof(req), .bytes = (uint64_t)(uintptr_t)&req,
    };
    CHECK_ST(jam_channel_read(&rd), OK);   /* the reply went out */
    x = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), OK);   /* and the request was still queued */
    CHECK_EQ(nb, sizeof(rep));
    uint8_t big[64] = { 0 };
    CHECK_ST(jam_channel_write(a, big, sizeof(big), NULL, 0), OK);
    x = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, 0);
    CHECK_ST(jam_channel_reply_wait(&x), ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(nb, sizeof(big));
    x.bytes = (uint64_t)(uintptr_t)big;   /* the request was still queued */
    x.bytes_cap = sizeof(big);
    CHECK_ST(jam_channel_reply_wait(&x), OK);
    handle_t ev, got = 0;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_channel_write(a, &rep, sizeof(rep), &ev, 1), OK);
    x = rw_args(HANDLE_INVALID, NULL, b, &req, &nb, 0);
    x.bytes = 16;
    x.handles = (uint64_t)(uintptr_t)&got;
    x.handles_cap = 1;
    x.actual_handles = (uint64_t)(uintptr_t)&nh;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    x.bytes = (uint64_t)(uintptr_t)&req;
    CHECK_ST(jam_channel_reply_wait(&x), ERR_TIMED_OUT);   /* it was consumed */
    struct port_packet *badpkt = (struct port_packet *)16;
    handle_t port;
    CHECK_ST(jam_port_create(&port), OK);
    struct port_packet up = { .key = 3, .type = PORT_PACKET_USER };
    CHECK_ST(jam_port_queue(port, &up), OK);
    x = (struct channel_reply_wait_args){ .wait = port, .packet = (uint64_t)(uintptr_t)badpkt };
    CHECK_ST(jam_channel_reply_wait(&x), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(port), OK);
    return true;
}

bool t_reply_wait_refusals(void)
{
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    bool ok = rw_refusals_args(a, b) && rw_refusals_handles(a, b) && rw_refusals_buffers(a, b);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    if (!ok)
        return false;
    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    CHECK_EQ(after.used[JOB_LIMIT_MSG_BYTES], before.used[JOB_LIMIT_MSG_BYTES]);
    return true;
}

/* ---- the kernel's chanread_reply_wait_kill_loses_nothing ---------------------- */

/* The log VMO's layout: kernel/test/test_chanrw.c's RW_*. */
#define RW_ENTRIES 48
#define RW_STRIDE  (20u << 10)
#define RW_LEN     0    /* the request's length, written by the kernel */
#define RW_MARK    8    /* the mark of the reply to it, written by the kernel */
#define RW_DATA    64
#define RW_MSG_MAX (16u << 10)
#define RW_SIZE    (PAGE_SIZE + RW_ENTRIES * RW_STRIDE)

/* "utest rw-reader": take SR_USER's requests with channel_reply_wait, each
 * straight into the next entry of SR_USER + 1, and answer each (its txid
 * word and its entry's index) in the call that takes the next, with the
 * mark in its own entry. Each request is counted in the header after the
 * call that took it returned. */
static int rw_reader(void)
{
    handle_t ch = startup_handle(SR_USER), log = startup_handle(SR_USER + 1);
    uint64_t addr = 0;
    if (jam_vmar_map(startup_handle(SR_SELF_VMAR), log, 0, RW_SIZE, VMAR_READ | VMAR_WRITE,
                     &addr) != OK)
        return 1;
    volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)addr;
    for (uint64_t off = 0; off < RW_SIZE; off += PAGE_SIZE)
        base[off] = base[off];   /* mapped before any call: a copy never faults */
    uint32_t *hdr = (uint32_t *)(uintptr_t)addr;
    __atomic_store_n(&hdr[1], 1, __ATOMIC_RELEASE);   /* ready */
    uint32_t rep[2];
    struct channel_reply_wait_args a = {
        .h = HANDLE_INVALID, .wait = ch, .bytes_cap = RW_MSG_MAX, .deadline_ns = DEADLINE_NEVER,
    };
    for (uint32_t k = 0; k < RW_ENTRIES; k++) {
        uint8_t *e = (uint8_t *)(uintptr_t)(addr + PAGE_SIZE + (uint64_t)k * RW_STRIDE);
        a.bytes = (uint64_t)(uintptr_t)(e + RW_DATA);
        a.actual_bytes = (uint64_t)(uintptr_t)(e + RW_LEN);
        status_t st = jam_channel_reply_wait(&a);
        if (st != OK)
            return st == ERR_PEER_CLOSED ? 0 : 2;
        __atomic_store_n(&hdr[0], k + 1, __ATOMIC_RELEASE);
        memcpy(&rep[0], e + RW_DATA, 4);
        rep[1] = k;
        a.h = ch;
        a.rbytes = (uint64_t)(uintptr_t)rep;
        a.rn = sizeof(rep);
        a.mark = (uint64_t)(uintptr_t)(e + RW_MARK);
    }
    return 0;
}

int replywait_child(int argc, char **argv)
{
    const char *m = argv[1] + 3;   /* after "rw-" */
    if (!strcmp(m, "reply"))
        return rw_reply(argc, argv);
    if (!strcmp(m, "reader"))
        return rw_reader();
    printf("utest: rw mode \"%s\" unknown\n", m);
    return 127;
}
