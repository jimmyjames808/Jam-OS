/* compositor: the Wayland compositor (docs/G1-PLAN.md). Programs that ask
 * for `svc wayland` open /svc/wayland and speak Wayland (<jwl.h>) to it,
 * each on a channel of its own; it keeps their surfaces, places their
 * windows and composes them onto the output.
 *
 * Startup handles:
 *   SR_RESOURCE   the root resource with RIGHT_ROOT_SCREEN: the framebuffer
 *                 (output.c); without it, or with no framebuffer, headless.
 *   SR_USER + 0   the server end of /svc/wayland: the svc protocol's
 *                 connect makes each opener's connection (conn.c). init
 *                 keeps it, so a restarted compositor serves the same one.
 *   SR_USER + 1   optional, headless only: a VMO to compose into (at least
 *                 width * height * 4 bytes, read and written), for a test
 *                 or a screenshot to look at; without it the image is a
 *                 VMO of our own.
 *   SR_USER + 2   optional, `testscene` only: a channel for its reports.
 * Arguments: `headless` (no framebuffer: compose into memory),
 * `size=<w>x<h>` (the headless output, default 1280x800), `hz=<n>` (the
 * paint clock, display.hz: 60 by default), `threads=<n>` (painting
 * threads, the loop's included: a few by default), and `testscene`
 * followed by its commands (testscene.c: windows with no client, for the
 * tests; a test power only the starter can give, never a client).
 *
 * The loop (the service-loop rule, ARCHITECTURE.md "How a service waits"):
 * one thread, one port. A turn takes what the port has (new connections,
 * clients with messages), serves each client a budget (conn.c), paints if
 * the clock says so and answers the frame callbacks the paint showed, and
 * writes what each client is owed. It never blocks on anyone: nothing it
 * sends waits (libjwl's flow control holds what a client is slow to read),
 * and it makes no calls to other processes. It sleeps until the port has
 * something, the paint clock's next tick if there is damage, or the next
 * hidden surface's frame callbacks; never while a client's budget left
 * messages over. Painting (paint.c) runs on a few worker threads while
 * this one waits for them. */
#include <fun.h>
#include <idl/svc.h>
#include <splash.h>
#include "paint.h"

#define DEFAULT_W  1280
#define DEFAULT_H  800
#define MAX_SIDE   8192
#define HELD_RETRY (10 * NS_PER_MS)   /* a client's full channel: try writing again after this */
#define SVC_BUDGET 16u                /* connects per turn */

struct comp comp;

uint32_t comp_serial(void)
{
    if (++comp.serial == 0)
        comp.serial = 1;
    return comp.serial;
}

/* "size=<w>x<h>": OK with the two, each 1 to MAX_SIDE. */
static bool parse_size(const char *s, int32_t *w, int32_t *h)
{
    int32_t v[2] = { 0, 0 };
    for (unsigned i = 0; i < 2; i++) {
        if (*s < '0' || *s > '9')
            return false;
        while (*s >= '0' && *s <= '9' && v[i] <= MAX_SIDE)
            v[i] = v[i] * 10 + (*s++ - '0');
        if (v[i] < 1 || v[i] > MAX_SIDE || *s != (i ? '\0' : 'x'))
            return false;
        s++;
    }
    *w = v[0];
    *h = v[1];
    return true;
}

/* "<key>=<n>" with n 1 to max: true with n in *out. */
static bool parse_num(const char *arg, const char *key, uint32_t max, uint32_t *out)
{
    size_t k = strlen(key);
    if (strncmp(arg, key, k) || arg[k] != '=')
        return false;
    uint32_t v = 0;
    const char *s = arg + k + 1;
    while (*s >= '0' && *s <= '9' && v <= max)
        v = v * 10 + (uint32_t)(*s++ - '0');
    if (*s || v < 1 || v > max)
        return false;
    *out = v;
    return true;
}

/* The arguments before `testscene` (whose index goes to *scene_at, 0 if
 * none). */
struct args {
    int32_t w, h;
    uint32_t hz, threads;
    int scene_at;
};

static void parse_args(int argc, char **argv, struct args *a)
{
    *a = (struct args){ .w = DEFAULT_W, .h = DEFAULT_H };
    for (int i = 1; i < argc && !a->scene_at; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "headless"))
            comp.headless = true;
        else if (!strcmp(s, "testscene"))
            a->scene_at = i;
        else if (!(!strncmp(s, "size=", 5) && parse_size(s + 5, &a->w, &a->h)) &&
                 !parse_num(s, "hz", 1000, &a->hz) && !parse_num(s, "threads", 64, &a->threads))
            printf("compositor: argument \"%s\" ignored\n", s);
    }
}

static status_t setup(int argc, char **argv, struct args *a)
{
    parse_args(argc, argv, a);
    comp.svc = startup_handle(SR_USER + 0);
    comp.vmar = startup_handle(SR_SELF_VMAR);
    if (comp.svc == HANDLE_INVALID || comp.vmar == HANDLE_INVALID)
        return ERR_BAD_HANDLE;
    clock_init(a->hz);
    status_t st = output_open(comp.headless, a->w, a->h);
    if (st != OK)
        return st;
    comp.headless = !output.screen;
    scene_init(output.width, output.height, SPLASH_BG);
    st = paint_init(a->threads);
    if (st == OK)
        st = jam_port_create(&comp.port);
    if (st == OK)
        st = jam_port_bind(comp.port, comp.svc, COMP_KEY_SVC, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    return st;
}

/* The shared channel speaks only svc.connect. */
static uint32_t no_protocol(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                            uint32_t *rhn)
{
    static const struct svc_ops none;
    return svc_dispatch(&none, ctx, req, n, rep, rhs, rhn);
}

/* New connections: OK, or the shared channel's end (init is gone). */
static status_t serve_svc(void)
{
    for (unsigned n = 0; n < SVC_BUDGET; n++) {
        status_t st = svc_serve_request(comp.svc, no_protocol, conn_connect, NULL);
        if (st == ERR_SHOULD_WAIT)
            return OK;
        if (st != OK)
            return st;
    }
    return OK;
}

/* How long the loop may sleep. */
static uint64_t next_deadline(void)
{
    if (conn_more())
        return 0;
    uint64_t d = clock_deadline();
    if (conn_held()) {
        uint64_t r = now() + HELD_RETRY;
        d = r < d ? r : d;
    }
    return d;
}

/* Wait for the port (until deadline), then take every packet it has. The
 * shared channel's end: its status. */
static status_t take_packets(uint64_t deadline)
{
    struct port_packet pkt;
    status_t st = jam_port_wait(comp.port, deadline, &pkt);
    bool svc = false;
    while (st == OK) {
        if (pkt.key == COMP_KEY_SVC)
            svc = true;
        else
            conn_ready(pkt.key);
        st = jam_port_wait(comp.port, 0, &pkt);
    }
    if (st != ERR_TIMED_OUT && st != ERR_SHOULD_WAIT)
        return st;
    return svc ? serve_svc() : OK;
}

int main(int argc, char **argv)
{
    struct args a;
    status_t st = setup(argc, argv, &a);
    if (st != OK) {
        printf("compositor: can't start: %s\n", status_str(st));
        return 1;
    }
    printf("compositor: ready, %s %dx%d, painting on %u threads at %lu Hz\n",
           output.screen ? "on the screen" : "headless", scene.width, scene.height,
           pool_threads(), (unsigned long)(NS_PER_S / comp.period_ns));
    if (a.scene_at)
        return testscene_run(argc, argv, a.scene_at + 1);
    st = serve_svc();   /* connects queued before we bound the port */
    while (st == OK) {
        conn_serve_all();
        clock_turn();
        conn_flush_all();   /* the frame callbacks the paint answered */
        st = take_packets(next_deadline());
    }
    printf("compositor: /svc/wayland's channel: %s: ending\n", status_str(st));
    return st == ERR_PEER_CLOSED ? 0 : 1;
}
