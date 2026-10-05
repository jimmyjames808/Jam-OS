/* compositor: the Wayland compositor (docs/G1-PLAN.md). Programs that ask
 * for `svc wayland` open /svc/wayland and speak Wayland (<jwl.h>) to it,
 * each on a channel of its own; it keeps their surfaces, places their
 * windows and composes them onto the output.
 *
 * Startup handles:
 *   SR_USER + 0   the server end of /svc/wayland: the svc protocol's
 *                 connect makes each opener's connection (conn.c). init
 *                 keeps it, so a restarted compositor serves the same one.
 *   SR_USER + 1   optional, headless only: a VMO to compose into (at least
 *                 width * height * 4 bytes, read and written), for a test
 *                 or a screenshot to look at; without it the image is a
 *                 VMO of our own.
 * Arguments: `headless` (no framebuffer: compose into memory) and
 * `size=<w>x<h>` (the headless output, default 1280x800). Not built yet:
 * the framebuffer (framebuffer_take), so every run is headless for now.
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
 * messages over. */
#include <idl/svc.h>
#include <splash.h>
#include "comp.h"

#define DEFAULT_W  1280
#define DEFAULT_H  800
#define MAX_SIDE   8192
#define PAINT_HZ   60
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

/* The headless image: the starter's VMO (SR_USER + 1) or one of our own,
 * mapped read-write. */
static status_t headless_image(int32_t w, int32_t h)
{
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4, size = 0, addr = 0;
    uint64_t mapped = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    handle_t vmo = startup_handle(SR_USER + 1);
    status_t st = OK;
    if (vmo == HANDLE_INVALID)
        st = jam_vmo_create(mapped, 0, HANDLE_INVALID, &vmo);
    if (st == OK)
        st = jam_vmo_get_size(vmo, &size);
    if (st == OK && size < mapped)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = jam_vmar_map(comp.vmar, vmo, 0, mapped, VMAR_READ | VMAR_WRITE, &addr);
    if (vmo != HANDLE_INVALID)
        jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return st;
    scene.pixels = (uint32_t *)(uintptr_t)addr;
    scene.stride = (uint32_t)w;
    return OK;
}

static status_t setup(int argc, char **argv)
{
    int32_t w = DEFAULT_W, h = DEFAULT_H;
    comp.headless = true;   /* the framebuffer is not built yet */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "headless"))
            comp.headless = true;
        else if (strncmp(argv[i], "size=", 5) || !parse_size(argv[i] + 5, &w, &h))
            printf("compositor: argument \"%s\" ignored\n", argv[i]);
    }
    comp.svc = startup_handle(SR_USER + 0);
    comp.vmar = startup_handle(SR_SELF_VMAR);
    comp.period_ns = NS_PER_S / PAINT_HZ;
    if (comp.svc == HANDLE_INVALID || comp.vmar == HANDLE_INVALID)
        return ERR_BAD_HANDLE;
    scene_init(w, h, SPLASH_BG);
    status_t st = headless_image(w, h);
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

/* Paint if there is something to show and the clock allows; answer the
 * frame callbacks that are due. */
static void paint_turn(void)
{
    uint64_t t = now();
    bool want = !damage_empty(&scene.damage) || surfaces_waiting_paint();
    if (want && t >= scene.last_paint_ns + comp.period_ns) {
        headless_compose();
        uint64_t done = now();
        scene.paints++;
        scene.last_paint_ns = done;
        comp.stats.paints++;
        comp.stats.last_paint_ns = done - t;
        if (done - t > comp.stats.worst_paint_ns)
            comp.stats.worst_paint_ns = done - t;
        surfaces_frame_done(done, true);
    } else if (t >= surfaces_hidden_deadline()) {
        surfaces_frame_done(t, false);
    }
}

/* How long the loop may sleep. */
static uint64_t next_deadline(void)
{
    if (conn_more())
        return 0;
    uint64_t d = surfaces_hidden_deadline();
    if (!damage_empty(&scene.damage) || surfaces_waiting_paint()) {
        uint64_t p = scene.last_paint_ns + comp.period_ns;
        d = p < d ? p : d;
    }
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
    status_t st = setup(argc, argv);
    if (st != OK) {
        printf("compositor: can't start: %s\n", status_str(st));
        return 1;
    }
    printf("compositor: ready, headless %dx%d\n", scene.width, scene.height);
    st = serve_svc();   /* connects queued before we bound the port */
    while (st == OK) {
        conn_serve_all();
        paint_turn();
        conn_flush_all();   /* the frame callbacks the paint answered */
        st = take_packets(next_deadline());
    }
    printf("compositor: /svc/wayland's channel: %s: ending\n", status_str(st));
    return st == ERR_PEER_CLOSED ? 0 : 1;
}
