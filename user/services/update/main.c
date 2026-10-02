/* bin/update: the fetcher of `update` (docs/M9-PLAN.md "update: a new
 * build from the Mac"). It asks tools/update-server.py on the Mac for the
 * build it serves, over a UDP socket of its own to the server's port
 * UPDWIRE_PORT, with the fetcher's window (<updfetch.h>); stores the
 * kernel and the boot image in two VMOs; and offers them to init, which
 * checks them against the manifest and loads them (<update.h>).
 *
 * It parses what the network sends, so it holds almost nothing: /svc/net
 * (its list) and the offer channel the shell took from init, nothing
 * else. It can't load a kernel; it can only offer bytes that init checks.
 * The shell's `update` starts it as a helper (sh_run_helper):
 *   argv: update <server address> load|check <running version> <running git>
 *   SR_USER + 0   the offer channel (initctl.update_offer)
 *   SR_USER + 2   the shell's stop channel: Ctrl+C (or the shell gone)
 * Its lines are the shell's. Exit: 0 init took the build (or, with
 * `check`, would have), 1 not, 2 usage, 130 stopped. */
#include <ipv4.h>
#include <net.h>
#include <updfetch.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc net-sys\n");   /* the network's reserve (tools/checkwants.py: services only) */

#define ROLE_OFFER  (SR_USER + 0)
#define ROLE_STOP   (SR_USER + 2)
#define UP_WAIT     (10 * NS_PER_S)    /* for the network's address */
#define ANSWER_WAIT (60 * NS_PER_S)    /* init copies and hashes the build */
#define STOP_LOOK   (100 * NS_PER_MS)  /* how often the fetch looks for Ctrl+C */
/* The socket's rx ring: the fetcher's whole window of replies (32 of at
 * most a datagram each, 47.6 KiB as records) and room to spare. */
#define UPDATE_RX_RING (64u * 1024)
_Static_assert(UPDFETCH_WINDOW * (SOCKRING_DGRAM_HDR + NET_DGRAM_MAX) <= UPDATE_RX_RING,
               "the window fits the ring");
/* Bytes as MB with one decimal: the two arguments of "%lu.%u". */
#define MB(b)       ((unsigned long)((b) / 1000000)), ((unsigned)((b) / 100000 % 10))

/* The fetch's side of the io (<updfetch.h>). */
struct fetch {
    struct net_sock sock;                         /* connected to the server */
    handle_t        vmo[UPDATE_FILES];            /* the files as they come */
    uint64_t        size[UPDATE_FILES];           /* their sizes, the manifest's */
    uint8_t         manifest[UPDATE_MANIFEST_MAX];
    uint32_t        manifest_len;
};

static struct fetch fetch;
static struct net_dgram dgram;

/* The shell asked us to stop (Ctrl+C), or is gone. */
static bool stop_asked(void)
{
    signals_t seen;
    handle_t stop = startup_handle(ROLE_STOP);
    return stop && jam_object_wait_one(stop, SIG_READABLE | SIG_PEER_CLOSED, 0, &seen) == OK &&
           (seen & (SIG_READABLE | SIG_PEER_CLOSED));
}

static status_t io_send(void *ctx, const uint8_t *d, size_t len)
{
    struct fetch *f = ctx;
    return net_sendto_async(&f->sock, 0, 0, d, len);   /* 0:0, the connected server */
}

static status_t io_begin(void *ctx, const struct update_manifest *m, const uint8_t *text,
                         size_t len)
{
    struct fetch *f = ctx;
    for (unsigned i = 0; i < UPDATE_FILES; i++) {
        if (f->vmo[i])
            jam_handle_close(f->vmo[i]);   /* a restart: the snapshot was gone */
        f->vmo[i] = HANDLE_INVALID;
    }
    for (unsigned i = 0; i < UPDATE_FILES; i++) {
        uint64_t pages = (m->file[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        status_t st = jam_vmo_create(pages, 0, HANDLE_INVALID, &f->vmo[i]);
        if (st != OK)
            return st;
        f->size[i] = m->file[i].size;
    }
    memcpy(f->manifest, text, len);   /* len <= a datagram's, <= UPDATE_MANIFEST_MAX: parsed */
    f->manifest_len = (uint32_t)len;
    printf("update: the server has %s (git %s): kernel %lu.%u MB, boot image %lu.%u MB\n",
           m->version, m->git, MB(m->file[UPDATE_KERNEL].size), MB(m->file[UPDATE_BOOTFS].size));
    return OK;
}

static status_t io_store(void *ctx, unsigned file, uint64_t offset, const uint8_t *data,
                         size_t len)
{
    struct fetch *f = ctx;
    return jam_vmo_write(f->vmo[file], offset, data, len);
}

/* Every datagram queued on the socket to the fetcher. OK, or the
 * socket's failure (ERR_PEER_CLOSED: netstack is gone). */
static status_t take_all(struct updfetch *u, uint32_t host)
{
    status_t st;
    while ((st = net_sock_take(&fetch.sock, &dgram)) == OK)
        if (dgram.addr == host && dgram.port == UPDWIRE_PORT)
            updfetch_reply(u, dgram.data, dgram.len, now());
    return st == ERR_SHOULD_WAIT ? OK : st;
}

/* Another quarter of the build has come (*quarter: the last one said):
 * say so. */
static void progress(const struct updfetch *u, unsigned *quarter)
{
    unsigned q = u->total ? (unsigned)(u->stored * 4 / u->total) : 0;
    if (q <= *quarter || q >= 4)
        return;
    *quarter = q;
    printf("update: %u%% (%lu.%u of %lu.%u MB)\n", q * 25, MB(u->stored), MB(u->total));
}

/* Why the fetcher gave up, in words. A request unanswered UPDFETCH_TRIES
 * times is ERR_TIMED_OUT: no answer at all, answers that stopped, or
 * answers that never matched (more ignored than a window's repeats: a
 * server whose files aren't its manifest's). */
static void say_failure(const struct updfetch *u)
{
    if (u->why == ERR_TIMED_OUT && !u->replies)
        printf("update: no answer from the server (is tools/update-server.py running on the "
               "Mac, and net.host its address?)\n");
    else if (u->why == ERR_TIMED_OUT && u->ignored > UPDFETCH_WINDOW)
        printf("update: the server's answers don't match its manifest (%lu ignored): the "
               "fetch failed\n", (unsigned long)u->ignored);
    else if (u->why == ERR_TIMED_OUT)
        printf("update: the server stopped answering: the fetch failed\n");
    else
        printf("update: the fetch failed (%s)\n", status_str(u->why));
}

/* The whole fetch from host. OK when both files are in; ERR_CANCELED on
 * Ctrl+C; else why it failed (said). */
static status_t run_fetch(uint32_t host)
{
    static struct updfetch u;
    const struct updfetch_io io = { &fetch, io_send, io_begin, io_store };
    uint64_t t0 = now();
    unsigned quarter = 0;
    status_t st = OK;
    updfetch_start(&u, &io);
    while (st == OK) {
        uint64_t t = now(), next = updfetch_poll(&u, t);
        if (u.state == UPDFETCH_DONE || u.state == UPDFETCH_FAILED)
            break;
        if (stop_asked())
            return ERR_CANCELED;
        progress(&u, &quarter);
        (void)net_sock_wait(&fetch.sock, next < t + STOP_LOOK ? next : t + STOP_LOOK);
        st = take_all(&u, host);
    }
    uint64_t ms = (now() - t0) / NS_PER_MS;
    if (st != OK) {
        printf("update: the network failed (%s)\n", status_str(st));
        return st;
    }
    if (u.state == UPDFETCH_FAILED) {
        say_failure(&u);
        return u.why;
    }
    printf("update: fetched %lu.%u MB in %lu.%lu s (%lu requests, %lu sent again, %lu replies "
           "ignored)\n", MB(u.total), (unsigned long)(ms / 1000), (unsigned long)(ms / 100 % 10),
           (unsigned long)u.sent, (unsigned long)u.resent, (unsigned long)u.ignored);
    return OK;
}

/* The fetched build offered to init on ch (the VMOs go, read-only); its
 * answer into *a. */
static status_t offer(handle_t ch, bool check_only, struct update_answer *a)
{
    struct update_offer *o = calloc(1, sizeof(*o));
    if (!o)
        return ERR_NO_MEMORY;
    o->magic = UPDATE_OFFER_MAGIC;
    o->flags = check_only ? UPDATE_OFFER_CHECK_ONLY : 0;
    o->manifest_len = fetch.manifest_len;
    memcpy(o->manifest, fetch.manifest, fetch.manifest_len);
    handle_t hs[UPDATE_FILES];
    unsigned nh = 0;
    status_t st = OK;
    for (; st == OK && nh < UPDATE_FILES; nh++) {
        o->bytes[nh] = fetch.size[nh];
        st = jam_handle_duplicate(fetch.vmo[nh], RIGHT_READ | RIGHT_TRANSFER, &hs[nh]);
        if (st != OK)
            break;
    }
    if (st == OK)
        st = jam_channel_write(ch, o, sizeof(*o), hs, UPDATE_FILES);   /* moves the handles */
    for (unsigned i = 0; st != OK && i < nh; i++)
        jam_handle_close(hs[i]);
    free(o);
    signals_t seen;
    if (st == OK)
        st = jam_object_wait_one(ch, SIG_READABLE, now() + ANSWER_WAIT, &seen);
    uint32_t got = 0;
    struct channel_read_args r = {
        .h = ch, .bytes_cap = sizeof(*a), .bytes = (uint64_t)(uintptr_t)a,
        .actual_bytes = (uint64_t)(uintptr_t)&got,
    };
    if (st == OK)
        st = jam_channel_read(&r);
    if (st == OK && (got != sizeof(*a) || a->magic != UPDATE_ANSWER_MAGIC))
        st = ERR_INTERNAL;
    return st;
}

/* init's answer, in words; the exit status. */
static int say_answer(const struct update_answer *a, bool check_only, const char *from_version,
                      const char *from_git)
{
    if (a->why == UPDATE_ACCEPTED && a->status == OK) {
        printf("update: %s (%s) -> %.*s (%.*s): checked by init in %u ms, %s\n", from_version,
               from_git, (int)UPDATE_VERSION_MAX, a->version, (int)UPDATE_GIT_MAX, a->git,
               a->check_ms, check_only ? "not loaded (-n): the running build stays"
                                       : "stored: the next reboot runs it");
        return 0;
    }
    bool per_file = a->why == UPDATE_BAD_SIZE || a->why == UPDATE_SHORT_VMO ||
                    a->why == UPDATE_BAD_HASH;
    printf("update: init refused it: %s%s%s (%s); the running build is unchanged\n",
           update_why_str(a->why), per_file ? ": " : "", per_file ? update_file_name(a->file) : "",
           status_str(a->status));
    return 1;
}

/* The socket to host's update port, once the network has an address. */
static status_t open_socket(uint32_t host)
{
    handle_t net = net_svc();
    if (!net) {
        printf("update: no network (netstack isn't running)\n");
        return ERR_NOT_FOUND;
    }
    status_t st = net_wait_up(net, now() + UP_WAIT, NULL);
    if (st != OK) {
        printf("update: the network has no address (%s): net.address in /data/etc/settings\n",
               status_str(st));
        return st;
    }
    st = net_udp_open_rings(net, 0, 0, UPDATE_RX_RING, &fetch.sock);   /* a window of replies */
    if (st == OK)
        st = net_connect(&fetch.sock, host, UPDWIRE_PORT);
    if (st != OK)
        printf("update: no socket (%s)\n", status_str(st));
    return st;
}

int main(int argc, char **argv)
{
    uint32_t host;
    const char *end;
    handle_t ch = startup_handle(ROLE_OFFER);
    if (argc != 5 || !ipv4_parse(argv[1], &host, &end) || *end ||
        (strcmp(argv[2], "load") && strcmp(argv[2], "check")) || !ch) {
        printf("usage: update <server address> load|check <running version> <running git>, "
               "with init's offer channel (the shell's `update` starts it)\n");
        return 2;
    }
    bool check_only = !strcmp(argv[2], "check");
    printf("update: asking %s:%u for its build\n", argv[1], UPDWIRE_PORT);
    status_t st = open_socket(host);
    if (st == OK)
        st = run_fetch(host);
    net_close(&fetch.sock);   /* nothing more from the network */
    if (st == ERR_CANCELED) {
        printf("update: stopped; the running build is unchanged\n");
        return 130;
    }
    if (st != OK) {
        printf("update: the running build is unchanged\n");
        return 1;
    }
    struct update_answer a;
    memset(&a, 0, sizeof(a));
    st = offer(ch, check_only, &a);
    if (st != OK) {
        printf("update: init didn't answer (%s)\n",
               status_str(st));
        return 1;
    }
    return say_answer(&a, check_only, argv[3], argv[4]);
}
