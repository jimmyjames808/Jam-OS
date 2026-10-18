/* The POSIX hand-over (<posixfds.h>, user/lib/fdmsg.c) and spawn's
 * POSIX additions: a hand-over of every kind there and back, one of more
 * than 64 handles over several messages, one given to a spawned child
 * with spawn_args.handover, and a child's 8 MiB stack (spawn_args
 * .stack_size). The helpers the malformed and fuzz tests share
 * (pfdtest.h) are here too; those tests are posixfds_bad.c.
 *
 * Children: "utest posix-handover" reads its hand-over and reports on
 * descriptor 0 (a pipe); "utest posix-stack" touches 7 MiB of its
 * stack. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <posixfds.h>
#include <wants.h>
#include "pfdtest.h"
#include "utest.h"

/* ---- helpers (pfdtest.h) ------------------------------------------------------------ */

static status_t collect_fd(void *ctx, const struct posixfds_fd *rec, const void *data,
                           const handle_t *hs)
{
    struct pfd_collect *c = ctx;
    c->total++;
    if (c->refuse != OK || c->n == PFD_KEEP) {
        for (unsigned i = 0; i < rec->nhandles; i++)
            jam_handle_close(hs[i]);
        return c->refuse != OK ? c->refuse : ERR_NO_RESOURCES;
    }
    struct pfd_got *g = &c->got[c->n++];
    memset(g, 0, sizeof(*g));
    g->rec = *rec;
    memcpy(g->data, data, rec->data_len < PFD_DATA ? rec->data_len : PFD_DATA);
    for (unsigned i = 0; i < rec->nhandles; i++)
        g->hs[i] = hs[i];
    return OK;
}

void pfd_collect_close(struct pfd_collect *c)
{
    for (unsigned i = 0; i < c->n; i++)
        for (unsigned j = 0; j < c->got[i].rec.nhandles; j++)
            jam_handle_close(c->got[i].hs[j]);
    c->n = 0;
}

static void collect_reset(void *ctx)
{
    struct pfd_collect *c = ctx;
    c->resets++;
    pfd_collect_close(c);
}

static void collect_secure(void *ctx)
{
    struct pfd_collect *c = ctx;
    c->secures++;
}

struct posixfds_sink pfd_sink(struct pfd_collect *c)
{
    memset(c, 0, sizeof(*c));
    return (struct posixfds_sink){ .ctx = c, .fd = collect_fd, .reset = collect_reset,
                                   .secure = collect_secure };
}

uint64_t pfd_handles(void)
{
    struct job_info ji;
    if (jam_job_get_info(own_job(), &ji) != OK)
        return 0;
    return ji.used[JOB_LIMIT_HANDLES];
}

status_t pfd_make(uint32_t kind, handle_t hs[POSIXFDS_KIND_HANDLES])
{
    uint32_t types[POSIXFDS_KIND_HANDLES];
    int k = posixfds_kind_handles(kind, types);
    status_t st = k < 0 ? ERR_INVALID_ARGS : OK;
    for (int i = 0; st == OK && i < k; i++) {
        handle_t other;
        if (types[i] == OBJ_CHANNEL && (st = jam_channel_create(&hs[i], &other)) == OK)
            jam_handle_close(other);
        else if (types[i] == OBJ_VMO)
            st = jam_vmo_create(PAGE_SIZE, 0, &hs[i]);
        else if (types[i] == OBJ_EVENT)
            st = jam_event_create(&hs[i]);
    }
    return st;
}

struct posixfds_msg *pfd_hdr(struct pfd_raw *m)
{
    return (struct posixfds_msg *)m->b;
}

void pfd_raw_begin(struct pfd_raw *m, uint16_t seq, bool last, const char *cwd)
{
    memset(m, 0, sizeof(*m));
    struct posixfds_msg *h = pfd_hdr(m);
    h->magic = POSIXFDS_MAGIC;
    h->version = POSIXFDS_VERSION;
    h->seq = seq;
    h->flags = last ? POSIXFDS_LAST : 0;
    m->n = sizeof(*h);
    if (seq == 0) {
        h->state = (struct posixfds_state){ .umask = 022, .ppid = 7, .pgid = 7, .sid = 1,
                                            .sigmask = POSIXFDS_SIG(2), .sigignore = 0 };
        h->cwd_len = (uint32_t)strlen(cwd) + 1;
        memcpy(m->b + m->n, cwd, h->cwd_len);
        m->n += (h->cwd_len + 7) & ~7u;
    }
}

status_t pfd_raw_fd(struct pfd_raw *m, int32_t fd, uint16_t kind, uint32_t status,
                    uint64_t offset, const void *data, uint32_t len)
{
    int k = posixfds_kind_handles(kind, NULL);
    if (k < 0 || m->nrec == 16 || m->n + sizeof(struct posixfds_fd) + len + 8 > PFD_RAW ||
        m->nh + (unsigned)k > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    status_t st = pfd_make(kind, &m->h[m->nh]);
    if (st != OK)
        return st;
    struct posixfds_fd r = { .fd = fd, .kind = kind, .nhandles = (uint16_t)k, .status = status,
                             .data_len = len, .offset = offset };
    m->rec_at[m->nrec++] = m->n;
    memcpy(m->b + m->n, &r, sizeof(r));
    m->n += sizeof(r);
    memcpy(m->b + m->n, data, len);
    m->n += (len + 7) & ~7u;
    m->nh += (uint32_t)k;
    pfd_hdr(m)->nfds++;
    pfd_hdr(m)->nhandles = m->nh;
    return OK;
}

status_t pfd_raw_send(handle_t ch, struct pfd_raw *m)
{
    status_t st = jam_channel_write(ch, m->b, m->n, m->h, m->nh);
    m->nh = 0;
    return st;
}

void pfd_raw_drop(struct pfd_raw *m)
{
    for (uint32_t i = 0; i < m->nh; i++)
        jam_handle_close(m->h[i]);
    m->nh = 0;
}

uint32_t pfd_queued(handle_t ch)
{
    struct handle_info info;
    return jam_handle_info(ch, &info) == OK ? info.queued : UINT32_MAX;
}

/* ---- a hand-over there and back ------------------------------------------------------ */

static uint64_t koid_of(handle_t h)
{
    struct handle_info info;
    return jam_handle_info(h, &info) == OK ? info.koid : 0;
}

/* One descriptor of kind for a test: its handles fresh, its data the
 * kind's shape; koids[] gets the handles' koids. */
static status_t test_desc(struct posixfds_desc *d, handle_t *hs, uint64_t *koids, int32_t fd,
                          uint16_t kind)
{
    static const char *const data[POSIXFDS_KIND_COUNT] = {
        [POSIXFDS_FILE] = "/data/f", [POSIXFDS_DIR] = "/data/dir", [POSIXFDS_PIPE] = "left",
        [POSIXFDS_DEV] = "null", [POSIXFDS_ETC] = "/etc/passwd", [POSIXFDS_MEMFD] = "",
        [POSIXFDS_PROC] = "/proc/self/statm",
    };
    const char *s = data[kind] ? data[kind] : "xyz";
    static uint32_t dup_target;   /* a DUP's data: the descriptor sent just before (fd + 3) */
    *d = (struct posixfds_desc){
        .fd = fd, .kind = kind, .status = kind == POSIXFDS_FILE ? 2 | POSIXFDS_O_APPEND : 0,
        .offset = kind == POSIXFDS_FILE ? 123456789 : kind == POSIXFDS_DIR ? 7 : 0,
        .data = s, .data_len = (uint32_t)strlen(s) + (*s ? 1 : 0), .handles = hs,
    };
    if (kind == POSIXFDS_DUP) {
        dup_target = (uint32_t)fd + 3;
        d->data = &dup_target;
        d->data_len = sizeof(dup_target);
    }
    status_t st = pfd_make(kind, hs);
    for (int i = 0; st == OK && i < posixfds_kind_handles(kind, NULL); i++)
        koids[i] = koid_of(hs[i]);
    return st;
}

/* What c got matches the n descriptors of d (any order of arrival is
 * the sent order), handles by koid. */
static bool same_fds(const struct pfd_collect *c, const struct posixfds_desc *d, unsigned n,
                     uint64_t (*koids)[POSIXFDS_KIND_HANDLES])
{
    CHECK_EQ(c->n, n);
    for (unsigned i = 0; i < n; i++) {
        const struct pfd_got *g = &c->got[i];
        CHECK_EQ(g->rec.fd, d[i].fd);
        CHECK_EQ(g->rec.kind, d[i].kind);
        CHECK_EQ(g->rec.status, d[i].status);
        CHECK_EQ(g->rec.offset, d[i].offset);
        CHECK_EQ(g->rec.data_len, d[i].data_len);
        CHECK(!memcmp(g->data, d[i].data, d[i].data_len));
        CHECK_EQ(g->rec.nhandles, (unsigned)posixfds_kind_handles(d[i].kind, NULL));
        for (unsigned j = 0; j < g->rec.nhandles; j++)
            CHECK_EQ(koid_of(g->hs[j]), koids[i][j]);
    }
    return true;
}

static struct pfd_collect col;   /* the tests' sink */

/* Every kind once, with the state, there and back. */
bool t_posixfds_round_trip(void)
{
    uint64_t before = pfd_handles();
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    struct posixfds_sink sink = pfd_sink(&col);
    struct posixfds_state st0;
    char cwd[POSIXFDS_PATH_MAX];
    CHECK_ST(posixfds_read(b, &sink, &st0, cwd), ERR_NOT_FOUND);   /* nothing sent */

    struct posixfds_desc d[POSIXFDS_KIND_COUNT - 1];
    handle_t hs[POSIXFDS_KIND_COUNT - 1][POSIXFDS_KIND_HANDLES];
    uint64_t koids[POSIXFDS_KIND_COUNT - 1][POSIXFDS_KIND_HANDLES];
    for (uint16_t k = 1; k < POSIXFDS_KIND_COUNT; k++)
        CHECK_ST(test_desc(&d[k - 1], hs[k - 1], koids[k - 1], 1000 - 3 * k, k), OK);
    struct posixfds_out out = {
        .state = { .umask = 027, .ppid = 12, .pgid = 12, .sid = 3,
                   .sigmask = POSIXFDS_SIG(2) | POSIXFDS_SIG(64),
                   .sigignore = POSIXFDS_SIG(13),
                   .sigpending = POSIXFDS_SIG(15) | POSIXFDS_SIG(40) },
        .cwd = "/data/home", .fds = d, .nfds = POSIXFDS_KIND_COUNT - 1,
    };
    CHECK_ST(posixfds_write(a, &out), OK);
    CHECK_EQ(pfd_queued(b), 1u);
    struct posixfds_state got;
    CHECK_ST(posixfds_read(b, &sink, &got, cwd), OK);
    CHECK(!memcmp(&got, &out.state, sizeof(got)));
    CHECK(!strcmp(cwd, "/data/home"));
    if (!same_fds(&col, d, out.nfds, koids))
        return false;
    CHECK_EQ(col.resets, 0u);
    CHECK_EQ(col.secures, 0u);
    CHECK_EQ(pfd_queued(b), 0u);
    pfd_collect_close(&col);

    /* No descriptors at all: one message, the state alone; marked
     * AT_SECURE, which the sink hears once. */
    out.nfds = 0;
    out.cwd = "/";
    out.secure = true;
    CHECK_ST(posixfds_write(a, &out), OK);
    CHECK_ST(posixfds_read(b, &sink, &got, cwd), OK);
    CHECK_EQ(col.n, 0u);
    CHECK_EQ(col.secures, 1u);
    CHECK(!strcmp(cwd, "/"));
    jam_handle_close(a);
    CHECK_ST(posixfds_read(b, &sink, &got, cwd), ERR_NOT_FOUND);   /* closed, nothing queued */
    jam_handle_close(b);
    CHECK_EQ(pfd_handles(), before);
    return true;
}

/* 50 files and 20 sockets: 180 handles, several messages, all of them
 * arriving, in order. */
bool t_posixfds_many_handles(void)
{
    enum { FILES = 50, SOCKS = 20, N = FILES + SOCKS };
    static struct posixfds_desc d[N];
    static handle_t hs[N][POSIXFDS_KIND_HANDLES];
    static uint64_t koids[N][POSIXFDS_KIND_HANDLES];
    static struct pfd_collect many;   /* PFD_KEEP is enough for N */
    _Static_assert(N <= PFD_KEEP, "the collect keeps them all");
    uint64_t before = pfd_handles();
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    for (unsigned i = 0; i < N; i++)
        CHECK_ST(test_desc(&d[i], hs[i], koids[i], (int32_t)(i * 13 % POSIXFDS_FD_MAX),
                           i < FILES ? POSIXFDS_FILE : POSIXFDS_SOCKET), OK);
    struct posixfds_out out = { .cwd = "/boot", .fds = d, .nfds = N };
    CHECK_ST(posixfds_write(a, &out), OK);
    CHECK(pfd_queued(b) >= 3u);   /* 180 handles: at least three messages */
    struct posixfds_sink sink = pfd_sink(&many);
    struct posixfds_state got;
    char cwd[POSIXFDS_PATH_MAX];
    CHECK_ST(posixfds_read(b, &sink, &got, cwd), OK);
    if (!same_fds(&many, d, N, koids))
        return false;
    CHECK_EQ(pfd_queued(b), 0u);
    pfd_collect_close(&many);
    jam_handle_close(a);
    jam_handle_close(b);
    CHECK_EQ(pfd_handles(), before);
    return true;
}

/* ---- spawn ----------------------------------------------------------------------------- */

/* "utest posix-handover": read the hand-over from the startup channel and
 * write "<count> <cwd> <umask> <ppid>" on descriptor 0, a pipe. */
static int child_handover(void)
{
    struct posixfds_sink sink = pfd_sink(&col);
    struct posixfds_state s;
    static char cwd[POSIXFDS_PATH_MAX];
    status_t st = posixfds_read(startup_channel(), &sink, &s, cwd);
    if (st != OK || !col.n || col.got[0].rec.fd != 0 || col.got[0].rec.kind != POSIXFDS_PIPE)
        return 60;
    char line[128];
    int n = snprintf(line, sizeof(line), "%u %s %u %u", col.total, cwd, s.umask, s.ppid);
    st = jam_channel_write(col.got[0].hs[0], line, (uint32_t)n, NULL, 0);
    pfd_collect_close(&col);
    return st == OK ? 0 : 61;
}

/* "utest posix-stack": 7 MiB of locals, every page written from the top
 * down: a native stack (128 KiB) faults, an 8 MiB one doesn't. */
static __attribute__((noinline)) int child_stack(void)
{
    volatile uint8_t big[7u << 20];
    for (size_t at = sizeof(big); at >= PAGE_SIZE; at -= PAGE_SIZE)
        big[at - 1] = 1;
    big[0] = 1;
    return big[sizeof(big) - 1] == 1 && big[0] == 1 ? 0 : 62;
}

int posixfds_child(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "posix-handover"))
        return child_handover();
    if (!strcmp(argv[1], "posix-stack"))
        return child_stack();
    return 127;
}

/* Run "utest <mode>" in a fresh job with a (stack, hand-over); wait for it. */
static status_t spawn_posix(const char *mode, uint64_t stack, const struct posixfds_out *ho,
                            struct process_info *info)
{
    handle_t job, proc;
    status_t st = new_job(&job);
    if (st != OK) {
        posixfds_discard(ho);
        return st;
    }
    const char *argv[] = { "utest", mode };
    struct spawn_args a = { .path = "bin/utest", .name = mode, .argc = 2, .argv = argv,
                            .job = job, .stack_size = stack, .handover = ho };
    st = spawn(&a, &proc);
    if (st == OK) {
        st = spawn_wait(proc, 20 * NS_PER_S, info);
        jam_handle_close(proc);
    }
    jam_handle_close(job);
    return st;
}

/* A child given a hand-over of 41 descriptors (81 handles: two messages)
 * reads it whole before main's work and answers on its descriptor 0. */
bool t_posixfds_spawn_child(void)
{
    enum { N = 41 };
    static struct posixfds_desc d[N];
    static handle_t hs[N][POSIXFDS_KIND_HANDLES];
    static uint64_t koids[N][POSIXFDS_KIND_HANDLES];
    uint64_t before = pfd_handles();
    handle_t ours;
    CHECK_ST(jam_channel_create(&ours, &hs[0][0]), OK);
    d[0] = (struct posixfds_desc){ .fd = 0, .kind = POSIXFDS_PIPE, .status = 1,
                                   .handles = hs[0] };
    for (unsigned i = 1; i < N; i++)
        CHECK_ST(test_desc(&d[i], hs[i], koids[i], (int32_t)(i + 2), POSIXFDS_FILE), OK);
    struct posixfds_out out = { .state = { .umask = 077, .ppid = 4321 }, .cwd = "/data/w",
                                .fds = d, .nfds = N };
    struct process_info info;
    CHECK_ST(spawn_posix("posix-handover", 0, &out, &info), OK);
    CHECK_EQ(info.exit_code, 0);
    char line[128] = { 0 };
    uint32_t n = 0, nh = 0;
    struct channel_read_args r = {
        .h = ours, .rd_cap = sizeof(line) - 1, .rd_bytes = (uint64_t)(uintptr_t)line,
        .rd_actual = (uint64_t)(uintptr_t)&n, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&r), OK);
    CHECK(!strcmp(line, "41 /data/w 63 4321"));
    jam_handle_close(ours);
    CHECK_EQ(pfd_handles(), before);
    return true;
}

/* stack_size: 8 MiB carries 7 MiB of locals; the default 128 KiB doesn't
 * (the child is killed); over SPAWN_STACK_MAX is refused, its hand-over's
 * handles closed. */
bool t_spawn_stack_size(void)
{
    uint64_t before = pfd_handles();
    struct process_info info;
    CHECK_ST(spawn_posix("posix-stack", 8ull << 20, NULL, &info), OK);
    CHECK_EQ(info.exit_code, 0);
    CHECK_EQ(info.killed, 0u);
    CHECK_ST(spawn_posix("posix-stack", 0, NULL, &info), OK);
    CHECK_EQ(info.killed, 1u);
    handle_t hs[POSIXFDS_KIND_HANDLES];
    CHECK_ST(pfd_make(POSIXFDS_PIPE, hs), OK);
    struct posixfds_desc d = { .fd = 0, .kind = POSIXFDS_PIPE, .handles = hs };
    struct posixfds_out out = { .cwd = "/", .fds = &d, .nfds = 1 };
    CHECK_ST(spawn_posix("posix-stack", SPAWN_STACK_MAX + 1, &out, &info), ERR_OUT_OF_RANGE);
    CHECK_EQ(pfd_handles(), before);
    return true;
}

/* The list's word `posix` (<wants.h>): once, alone on its line, granting
 * /tmp writable (M13.6's U7b); `svc session` parses as any service (the
 * build and `allow` decide who may have it). */
bool t_wants_posix(void)
{
    static struct wants w;
    const char *good = "posix\nsvc net\n";
    CHECK_ST(wants_parse(good, strlen(good), &w), OK);
    CHECK(w.posix);
    CHECK_EQ(w.n, 2u);
    CHECK(!strcmp(w.grant[0], "/tmp:w"));
    CHECK(!strcmp(w.grant[1], "/svc/net"));
    CHECK_ST(wants_parse("svc net\n", 8, &w), OK);
    CHECK(!w.posix);
    static const char *const bad[] = { "posix\nposix\n", "posix now\n", "Posix\n", " posix\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_ST(wants_parse(bad[i], strlen(bad[i]), &w), ERR_INVALID_ARGS);
        CHECK(!w.posix);
    }
    CHECK_ST(wants_parse("svc session\n", 12, &w), OK);
    CHECK(!strcmp(w.grant[0], "/svc/" SVC_SESSION));
    return true;
}
