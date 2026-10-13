/* wl-window-test: a window from a ported program (G2, docs/G2-PLAN.md).
 * The first Wayland window on Jam OS drawn through the real libwayland:
 * it binds wl_compositor, wl_shm, xdg_wm_base and, when offered, the
 * decoration manager (so, by the owner's rule, the compositor gives it
 * its title bar), makes an xdg toplevel and draws into wl_shm buffers on
 * memfds: moving colour bands, redrawn on every frame callback.
 *
 *     wl-window-test [seconds]   draw until closed, or for that many seconds
 *
 * Output: "connected", "configured <w>x<h>", "frames <n>" when it ends,
 * then "wl-window-test: ok". Exit 1 if a global is missing or it can't
 * connect. This is Jam OS's (BSD-2-Clause), not libwayland's. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

struct app {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct zxdg_decoration_manager_v1 *deco;
    struct wl_surface *surface;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;
    int width, height;          /* the size asked for (0: ours) */
    bool configured, closed;
    unsigned frames;
    double until;               /* stop at this time (0: never) */
};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---- the registry ---------------------------------------------------------------- */

static void on_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
                      uint32_t version)
{
    struct app *a = data;
    if (!strcmp(iface, "wl_compositor"))
        a->compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, "wl_shm"))
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "zxdg_decoration_manager_v1"))
        a->deco = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}

static void on_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data, (void)r, (void)name;
}

static const struct wl_registry_listener registry_listener = { on_global, on_global_remove };

/* ---- xdg ------------------------------------------------------------------------- */

static void on_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { on_ping };

static void draw(struct app *a);

static void on_xsurface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    struct app *a = data;
    xdg_surface_ack_configure(xs, serial);
    if (!a->configured) {
        a->configured = true;
        printf("configured %dx%d\n", a->width ? a->width : 640, a->height ? a->height : 400);
        draw(a);
    }
}

static const struct xdg_surface_listener xsurface_listener = { on_xsurface_configure };

static void on_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                                  struct wl_array *states)
{
    struct app *a = data;
    (void)t, (void)states;
    if (w > 0 && h > 0) {
        a->width = w;
        a->height = h;
    }
}

static void on_toplevel_close(void *data, struct xdg_toplevel *t)
{
    struct app *a = data;
    (void)t;
    a->closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = { on_toplevel_configure,
                                                                 on_toplevel_close };

/* ---- drawing --------------------------------------------------------------------- */

static void on_buffer_release(void *data, struct wl_buffer *b)
{
    (void)data;
    wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = { on_buffer_release };

static void on_frame(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { on_frame };

/* The jam colours: raspberry, apricot, blackcurrant, and a light ground. */
static const uint32_t bands[] = { 0xffc2185b, 0xfff2a541, 0xff4a2c5e, 0xfff6efe6 };

static void draw(struct app *a)
{
    int w = a->width ? a->width : 640, h = a->height ? a->height : 400;
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("wl-window-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        fprintf(stderr, "wl-window-test: memfd: %s\n", strerror(errno));
        a->closed = true;
        return;
    }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        fprintf(stderr, "wl-window-test: mmap: %s\n", strerror(errno));
        close(fd);
        a->closed = true;
        return;
    }
    /* Diagonal bands that slide one pixel a frame. */
    unsigned shift = a->frames * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * w + x] = bands[((unsigned)(x + y) + shift) / 48 % 4];
    munmap(px, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &buffer_listener, NULL);
    wl_surface_attach(a->surface, b, 0, 0);
    wl_surface_damage(a->surface, 0, 0, w, h);
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    wl_surface_commit(a->surface);
    a->frames++;
}

static void on_frame(void *data, struct wl_callback *cb, uint32_t time)
{
    struct app *a = data;
    (void)time;
    wl_callback_destroy(cb);
    if (!a->closed)
        draw(a);
}

int main(int argc, char **argv)
{
    struct app a = { 0 };
    if (argc > 1)
        a.until = now_s() + atof(argv[1]);
    struct wl_display *wl = wl_display_connect(NULL);
    if (!wl) {
        fprintf(stderr, "wl-window-test: can't connect: %s\n", strerror(errno));
        return 1;
    }
    printf("connected\n");
    struct wl_registry *reg = wl_display_get_registry(wl);
    wl_registry_add_listener(reg, &registry_listener, &a);
    wl_display_roundtrip(wl);
    if (!a.compositor || !a.shm || !a.wm) {
        fprintf(stderr, "wl-window-test: missing %s%s%s\n", a.compositor ? "" : "wl_compositor ",
                a.shm ? "" : "wl_shm ", a.wm ? "" : "xdg_wm_base");
        return 1;
    }
    xdg_wm_base_add_listener(a.wm, &wm_listener, &a);
    a.surface = wl_compositor_create_surface(a.compositor);
    a.xsurface = xdg_wm_base_get_xdg_surface(a.wm, a.surface);
    xdg_surface_add_listener(a.xsurface, &xsurface_listener, &a);
    a.toplevel = xdg_surface_get_toplevel(a.xsurface);
    xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
    xdg_toplevel_set_title(a.toplevel, "wl-window-test (libwayland)");
    xdg_toplevel_set_app_id(a.toplevel, "wl-window-test");
    if (a.deco) {
        struct zxdg_toplevel_decoration_v1 *d =
            zxdg_decoration_manager_v1_get_toplevel_decoration(a.deco, a.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(a.surface);
    while (!a.closed && wl_display_dispatch(wl) != -1)
        if (a.until && now_s() >= a.until)
            break;
    printf("frames %u\n", a.frames);
    printf("wl-window-test: ok\n");
    wl_display_disconnect(wl);
    return 0;
}
