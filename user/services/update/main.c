/* bin/update: the fetcher of `update` (docs/M9-PLAN.md "update: a new
 * build from the Mac"). It asks tools/update-server.py on the Mac for the
 * build it serves, over a UDP socket of its own to the server's port
 * UPDWIRE_PORT, with the fetcher's window (<updfetch.h>); stores the
 * kernel and the boot image in two VMOs (and the boot menu in a third,
 * when the manifest names one); and offers them to init, which
 * checks the manifest's signature, then the files against it, and loads
 * them (<update.h>). It doesn't check the signature itself: only init's
 * check counts.
 *
 * It parses what the network sends, so it holds almost nothing: /svc/net
 * (its list) and the offer channel the shell took from init, nothing
 * else. It can't load a kernel; it can only offer bytes that init checks.
 * The shell's `update` starts it as a helper (sh_run_helper):
 *   argv: update <server address> load|check|write <running version> <running git>
 *         [force]
 *   (write, plain `update` and `update -w`: load, and have init write the
 *   build to the stick too; load, `update -m`: into memory only; check,
 *   `update -n`: checked, nothing loaded or written; force: a build whose
 *   network default differs from this one's is taken)
 *   SR_USER + 0   the offer channel (initctl.update_offer)
 *   SR_USER + 2   the shell's stop channel: Ctrl+C (or the shell gone)
 * Its lines are the shell's; the last one says what `reboot` and `reboot
 * -f` start now. Nothing here reboots. Exit: 0 init took the build (or,
 * with `check`, would have; with `write`, the stick has it too), 1 not,
 * 2 usage, 3 loaded but not written to the stick, 130 stopped. */
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
#define WRITE_WAIT  (300 * NS_PER_S)   /* ... and writes it to the stick (`write`) */
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
    handle_t        vmo[UPDATE_PARTS];            /* the files as they come (the menu's: 0
                                                   * if the manifest names none) */
    uint64_t        size[UPDATE_PARTS];           /* their sizes, the manifest's */
    unsigned        parts;                        /* how many: UPDATE_FILES, + 1 with a menu */
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
    for (unsigned i = 0; i < UPDATE_PARTS; i++) {
        if (f->vmo[i])
            jam_handle_close(f->vmo[i]);   /* a restart: the snapshot was gone */
        f->vmo[i] = HANDLE_INVALID;
        f->size[i] = 0;
    }
    f->parts = UPDATE_FILES + (m->has_menu ? 1u : 0u);
    for (unsigned i = 0; i < f->parts; i++) {
        uint64_t pages = (m->file[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        status_t st = jam_vmo_create(pages, 0, HANDLE_INVALID, &f->vmo[i]);
        if (st != OK)
            return st;
        f->size[i] = m->file[i].size;
    }
    memcpy(f->manifest, text, len);   /* len <= a datagram's, <= UPDATE_MANIFEST_MAX: parsed */
    f->manifest_len = (uint32_t)len;
    char menu[40] = ", no boot menu";
    if (m->has_menu)
        snprintf(menu, sizeof(menu), ", boot menu %lu bytes",
                 (unsigned long)m->file[UPDATE_MENU].size);
    printf("update: the server has %s (git %s): kernel %lu.%u MB, boot image %lu.%u MB%s\n",
           m->version, m->git, MB(m->file[UPDATE_KERNEL].size), MB(m->file[UPDATE_BOOTFS].size),
           menu);
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

/* The fetched build offered to init on ch with flags (UPDATE_OFFER_*; the
 * VMOs go, read-only); its answer into *a. */
static status_t offer(handle_t ch, uint32_t flags, struct update_answer *a)
{
    struct update_offer *o = calloc(1, sizeof(*o));
    if (!o)
        return ERR_NO_MEMORY;
    o->magic = UPDATE_OFFER_MAGIC;
    o->flags = flags;
    o->manifest_len = fetch.manifest_len;
    memcpy(o->manifest, fetch.manifest, fetch.manifest_len);
    handle_t hs[UPDATE_PARTS];
    unsigned nh = 0;
    status_t st = OK;
    for (; st == OK && nh < fetch.parts; nh++) {
        o->bytes[nh] = fetch.size[nh];
        st = jam_handle_duplicate(fetch.vmo[nh], RIGHT_READ | RIGHT_TRANSFER, &hs[nh]);
        if (st != OK)
            break;
    }
    if (st == OK)
        st = jam_channel_write(ch, o, sizeof(*o), hs, fetch.parts);   /* moves the handles */
    for (unsigned i = 0; st != OK && i < nh; i++)
        jam_handle_close(hs[i]);
    free(o);
    signals_t seen;
    if (st == OK)
        st = jam_object_wait_one(ch, SIG_READABLE,
                                 now() + (flags & UPDATE_OFFER_WRITE ? WRITE_WAIT : ANSWER_WAIT),
                                 &seen);
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

/* What became of the boot menu (a stick write that carried one). */
static void say_menu(const struct update_answer *a)
{
    if (a->menu == UPDATE_MENU_NONE)
        return;
    if (a->menu == UPDATE_MENU_REFUSED)
        printf("update: %s:\n  %.*s\n", update_menu_str(a->menu), (int)UPDATE_MENU_WHY_MAX,
               a->menu_why);
    else if (a->menu == UPDATE_MENU_NOT_WRITTEN)
        printf("update: %s (%s)\n", update_menu_str(a->menu), status_str(a->menu_status));
    else
        printf("update: %s\n", update_menu_str(a->menu));
}

/* Is the offered build (its signed manifest's version and git, as init
 * read them) the one running? Said only for a clean commit: two builds of
 * a tree with changes ("-dirty") can differ under one version and git. */
static bool is_running(const struct update_answer *a, const char *version, const char *git)
{
    size_t n = strlen(git);
    if (!strcmp(git, "unknown") || (n >= 6 && !strcmp(git + n - 6, "-dirty")))
        return false;
    return !strncmp(a->version, version, sizeof(a->version)) &&
           !strncmp(a->git, git, sizeof(a->git));
}

/* init's answer to an offer with flags, in words (but the menu's and what
 * comes next: say_next); the exit status. */
static int say_verdict(const struct update_answer *a, uint32_t flags, const char *from_version,
                       const char *from_git)
{
    if ((a->why == UPDATE_ACCEPTED && a->status == OK) || a->why == UPDATE_NOT_WRITTEN) {
        bool taken = a->why == UPDATE_ACCEPTED;
        printf("update: %s (%s) -> %.*s (%.*s): checked by init in %u ms, %s", from_version,
               from_git, (int)UPDATE_VERSION_MAX, a->version, (int)UPDATE_GIT_MAX, a->git,
               a->check_ms,
               flags & UPDATE_OFFER_CHECK_ONLY ? "not loaded (-n): the running build stays, the "
                                                 "stick is untouched"
               : !(flags & UPDATE_OFFER_WRITE) ? "loaded into memory only (-m)"
               : !taken                        ? "loaded, but init couldn't write it to the stick"
               : a->already                    ? "loaded; the stick has it already: nothing "
                                                 "written"
                                               : "loaded and written to the stick");
        if (!taken)
            printf(" (%s: %s)", update_write_step_str(a->write_step), status_str(a->status));
        printf("\n");
        return taken ? 0 : 3;
    }
    if (a->why == UPDATE_NET_CHANGE) {
        printf("update: init refused it: its network default is %.*s, this build's %.*s: "
               "`update -f` takes it anyway (the PC then sends on that network); the running "
               "build is unchanged\n", (int)UPDATE_NET_MAX, a->net, (int)UPDATE_NET_MAX,
               a->net_running[0] ? a->net_running : "not known");
        return 1;
    }
    if (a->why == UPDATE_NEEDS_NEWER) {
        printf("update: init refused it: it needs a newer build than this one to take it (it "
               "has \"%.*s\", which this build doesn't know); the running build is "
               "unchanged\n",
               (int)UPDATE_EXT_KEY_MAX, a->needs);
        return 1;
    }
    bool per_file = a->why == UPDATE_BAD_SIZE || a->why == UPDATE_SHORT_VMO ||
                    a->why == UPDATE_BAD_HASH;
    printf("update: init refused it: %s%s%s (%s); the running build is unchanged\n",
           update_why_str(a->why), per_file ? ": " : "", per_file ? update_file_name(a->file) : "",
           status_str(a->status));
    return 1;
}

/* The last line: what `reboot` (kexec: the loaded build) and `reboot -f`
 * (the firmware: whatever the stick boots) start now. Nothing for a check
 * or a refusal: the verdict said the running build stays. */
static void say_next(const struct update_answer *a, uint32_t flags, bool running)
{
    if (flags & UPDATE_OFFER_CHECK_ONLY)
        return;
    if (a->why == UPDATE_NOT_WRITTEN)
        printf("update: loaded, but NOT written to the stick: `reboot` starts the new build "
               "from memory; %s, and `reboot -f` or a power-off starts that\n",
               update_stick_str(a->stick));
    if (a->why != UPDATE_ACCEPTED || a->status != OK)
        return;
    if (!(flags & UPDATE_OFFER_WRITE))
        printf("update: loaded into memory only: `reboot` starts it now; the stick is "
               "untouched, so `reboot -f` and a power-off bring back the stick's build\n");
    else if (a->already && running)
        printf("update: this is the build running now, and the stick has it already: nothing "
               "to do (it is loaded too: `reboot` starts a fresh copy of it)\n");
    else if (a->already)
        printf("update: the stick has this build already, and it is loaded: `reboot` starts it "
               "now, `reboot -f` restarts through the firmware (the stick boots it too)\n");
    else
        printf("update: written to the stick and loaded: `reboot` starts it now, `reboot -f` "
               "restarts through the firmware (the stick boots it too)\n");
}

/* init's answer, in words, the menu's fate, then what comes next; the
 * exit status (the menu's fate doesn't change it: the build is what was
 * asked for). */
static int say_answer(const struct update_answer *a, uint32_t flags, const char *from_version,
                      const char *from_git)
{
    int rc = say_verdict(a, flags, from_version, from_git);
    say_menu(a);
    say_next(a, flags, is_running(a, from_version, from_git));
    return rc;
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
    bool force = argc == 6 && !strcmp(argv[5], "force");
    if ((argc != 5 && !force) || !ipv4_parse(argv[1], &host, &end) || *end ||
        (strcmp(argv[2], "load") && strcmp(argv[2], "check") && strcmp(argv[2], "write")) ||
        !ch) {
        printf("usage: update <server address> load|check|write <running version> "
               "<running git> [force], with init's offer channel (the shell's `update` starts "
               "it)\n");
        return 2;
    }
    uint32_t flags = !strcmp(argv[2], "check")   ? UPDATE_OFFER_CHECK_ONLY
                     : !strcmp(argv[2], "write") ? UPDATE_OFFER_WRITE
                                                 : 0;
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
    st = offer(ch, flags | (force ? UPDATE_OFFER_FORCE : 0), &a);
    if (st != OK) {
        printf("update: init didn't answer (%s)\n", status_str(st));
        return 1;
    }
    return say_answer(&a, flags, argv[3], argv[4]);
}
